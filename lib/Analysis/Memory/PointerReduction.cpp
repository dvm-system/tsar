#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/PointerReduction.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/PassSupport.h>
#include <llvm/Transforms/Scalar.h>

using namespace tsar;
using namespace llvm;

char PointerReductionPass::ID = 0;

INITIALIZE_PASS_BEGIN(PointerReductionPass, "ptr-red",
    "Pointer Reduction Pass", false, false)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass);
INITIALIZE_PASS_END(PointerReductionPass, "ptr-red",
    "Pointer Reduction Pass", false, false)

void PointerReductionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
}

bool hasVolatileLoadInstInLoop(Value *V, Loop *L) {
  for (auto *User : V->users()) {
    if (auto *SI = dyn_cast<LoadInst>(User)) {
      if (L->contains(SI) && SI->isVolatile()) {
        return true;
      }
    }
  }
  return false;
}

bool hasPossibleReductionUses(Instruction *Instr) {
  const static auto opCodes = {
      BinaryOperator::Add,
      BinaryOperator::FAdd,
      BinaryOperator::Sub,
      BinaryOperator::FSub,
      BinaryOperator::Mul,
      BinaryOperator::FMul,
      BinaryOperator::UDiv,
      BinaryOperator::SDiv,
      BinaryOperator::FDiv,

  };
  for (auto *User : Instr->users()) {
    if (auto *Op = dyn_cast<Instruction>(User)) {
      if (Op->isAssociative()) {
        for (auto opCode : opCodes) {
          if (Op->getOpcode() == opCode) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

void eraseAllStoreInstFromLoop(Value *V, Loop *L) {
  for (auto *User : V->users()) {
    if (auto *SI = dyn_cast<StoreInst>(User)) {
      if (L->contains(SI)) {
        SI->eraseFromParent();
      }
    }
  }
}

bool validateValue(Value *V, Loop *L) {
  for (auto *User : V->users()) {
    auto *GEP = dyn_cast<GetElementPtrInst>(User);
    auto *Call = dyn_cast<CallInst>(User);
    if ((GEP && L->contains(GEP)) || (Call && L->contains(Call))) {
      return false;
    }
  }
  return true;
}

void handleLoadsInBB(BasicBlock *BB, DenseMap<BasicBlock *, Instruction *> &lastInstructions) {
  Instruction *lastVal = lastInstructions[BB];
  for (auto &Instr : BB->getInstList()) {
    if (auto *Load = dyn_cast<LoadInst>(&Instr)) {
      auto *oldLastVal = lastVal;
      if (Load->user_empty()) {
        continue;
      }
      if (!Load->isUsedOutsideOfBlock(BB)) {
        lastVal = Load->user_back();
      } else {
        for (auto *User : Load->users()) {
          if (auto *I = dyn_cast<Instruction>(User)) {
            if (I->getParent() == BB) {
              lastVal = I;
            } else {
              lastInstructions[I->getParent()] = I;
            }
          }
        }
      }
      Load->replaceAllUsesWith(oldLastVal);
//      Load->dropAllReferences();
//      Load->eraseFromParent();
    }
  }
  lastInstructions[BB] = lastVal;
}

void handleLoads(
    Loop *L,
    BasicBlock *BB,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<BasicBlock *> &completedBlocks,
    bool init = false)
{
  if (completedBlocks.find(BB) != completedBlocks.end()) {
    return;
  }
  if (!init) {
    handleLoadsInBB(BB, lastInstructions);
  }
  completedBlocks.insert(BB);
  for (auto *Succ : successors(BB)) {
    if (L->contains(Succ)) {
      handleLoads(L, Succ, lastInstructions, completedBlocks);
    }
  }
}

void insertPhiNodes(
    BasicBlock *BB,
    DenseMap<BasicBlock *, PHINode *> &phiNodes,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<PHINode *> &uniqueNodes,
    Value *V,
    bool init = false)
{
  if (phiNodes.find(BB) != phiNodes.end()) {
    return;
  }
  bool needsCreate = false;
  if (pred_size(BB) == 1 && !init) {
    if (phiNodes.find(BB->getSinglePredecessor()) != phiNodes.end()) {
      phiNodes[BB] = phiNodes.find(BB->getSinglePredecessor())->second;
    } else {
      needsCreate = true;
    }
  } else if (!init) {
    needsCreate = true;
  }
  if (needsCreate) {
    auto *phi = PHINode::Create(V->getType()->getPointerElementType(), 0,
        "phi." + BB->getName(), &BB->front());
    phiNodes[BB] = phi;
    uniqueNodes.insert(phi);
  }
  for (auto *Succ : successors(BB)) {
    insertPhiNodes(Succ, phiNodes, lastInstructions, uniqueNodes, V);
  }
}

bool PointerReductionPass::runOnFunction(Function &F) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

  // 1. Find memory that was marked anti after the first step;
  // 2. Check if this is a pointer that is dereferenced inside the loop;
  // 3. Copy its value at the preheader and store it back after the loop;
  // 4. Replace all load/store instructions in the loop's body
  //    with corresponding operations with this copied memory;
  // 5. Check if this helped; TODO

  for_each_loop(LI, [this, &TraitPool](Loop *L) {
    auto &Pool = TraitPool[L->getLoopID()];
    SmallDenseSet<Value *> Values;
    if (!Pool) {
      Pool = make_unique<DIMemoryTraitRegionPool>();
    } else {
      for (auto &T : *Pool) {
        if (T.is<trait::Anti>()) {
          for (auto &V : *T.getMemory()) {
            if (V.pointsToAliveValue()) {
              for (auto *User : V->users()) {
                if (auto *LI = dyn_cast<LoadInst>(User)) {
                  if (L->contains(LI) && hasPossibleReductionUses(LI)) {
                    Values.insert(LI->getPointerOperand());
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }

    for (auto *V : Values) {
      // we cannot replace anything if the pointer value can change somehow
      if (!validateValue(V, L)) {
        break;
      }

      // Load instructions that are marked volatile cannot be removed
      if (hasVolatileLoadInstInLoop(V, L)) {
        break;
      }

      auto *BeforeInstr = new LoadInst(V, "LoadPtr", &L->getLoopPredecessor()->back());
      DenseMap<BasicBlock *, PHINode *> phiNodes;
      DenseSet<PHINode *> uniqueNodes;
      DenseMap<BasicBlock *, Instruction *> lastInstructions;
      insertPhiNodes(L->getLoopPredecessor(), phiNodes, lastInstructions, uniqueNodes, V, true);

      lastInstructions[L->getLoopPredecessor()] = BeforeInstr;
      for (auto &P : phiNodes) {
        lastInstructions[P.first] = P.second;
      }

      DenseSet<BasicBlock *> processedBlocks;
      handleLoads(L, L->getLoopPredecessor(), lastInstructions, processedBlocks, true);

      for (auto *Phi : uniqueNodes) {
        auto *BB = Phi->getParent();
        for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
          phiNodes[BB]->addIncoming(lastInstructions[*Pred], *Pred);
        }
      }

      SmallVector<BasicBlock *, 8> ExitBlocks;
      L->getExitBlocks(ExitBlocks);
      for (auto *BB : ExitBlocks) {
        new StoreInst(phiNodes[BB], V, BB->getFirstNonPHI());
      }
      eraseAllStoreInstFromLoop(V, L);
    }
  });
  return false;
}
