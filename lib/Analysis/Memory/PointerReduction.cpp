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

bool getAllStoreOperandInstructions(Instruction *Inst, DenseSet<Instruction *> &Storage) {
  if (dyn_cast<StoreInst>(Inst)) {
    return true;
  }
  for (auto *User : Inst->users()) {
    if (auto *ChildInst = dyn_cast<Instruction>(User)) {
      if (getAllStoreOperandInstructions(ChildInst, Storage)) {
        Storage.insert(Inst);
      }
    }
  }
  return false;
}

void handleLoadsInBB(BasicBlock *BB, DenseMap<BasicBlock *, Instruction *> &lastInstructions, DenseSet<BasicBlock *> &changedLastInst) {
  for (auto &Instr : BB->getInstList()) {
    if (auto *Load = dyn_cast<LoadInst>(&Instr)) {
      Instruction *lastVal = lastInstructions[BB];
      if (Load->user_empty()) {
        continue;
      }
      DenseSet<Instruction *> instructions;
      getAllStoreOperandInstructions(Load, instructions);
      for (auto *Inst : instructions) {
          lastInstructions[Inst->getParent()] = Inst;
          changedLastInst.insert(Inst->getParent());
      }
      Load->replaceAllUsesWith(lastVal);
//      Load->dropAllReferences();
//      Load->eraseFromParent();
    }
  }
  if (pred_size(BB) == 1 && changedLastInst.find(BB) == changedLastInst.end()) {
      lastInstructions[BB] = lastInstructions[BB->getSinglePredecessor()];
  }
}

struct phiNodeLink {
  PHINode *phiNode;
  phiNodeLink *parent;

  explicit phiNodeLink(phiNodeLink *node) : phiNode(nullptr), parent(node) { }

  explicit phiNodeLink(PHINode *phi) : phiNode(phi), parent(nullptr) { }

  PHINode *getPhi() {
    if (phiNode) {
        return phiNode;
    }
    return parent->getPhi();
  }
};

void handleLoads(
    Loop *L,
    BasicBlock *BB,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<BasicBlock *> &completedBlocks,
    DenseSet<BasicBlock *> &changedLastInst,
    bool init = false)
{
  if (completedBlocks.find(BB) != completedBlocks.end()) {
    return;
  }
  if (!init) {
    handleLoadsInBB(BB, lastInstructions, changedLastInst);
  }
  completedBlocks.insert(BB);
  for (auto *Succ : successors(BB)) {
    if (L->contains(Succ)) {
      handleLoads(L, Succ, lastInstructions, completedBlocks, changedLastInst);
    }
  }
}

void freeLinks(DenseMap<BasicBlock *, phiNodeLink *> &phiLinks) {
  for (auto It : phiLinks) {
      delete It.second;
  }
}

void insertPhiNodes(
    BasicBlock *BB,
    DenseMap<BasicBlock *, phiNodeLink *> &phiLinks,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<PHINode *> &uniqueNodes,
    Value *V,
    bool init = false)
{
  if (phiLinks.find(BB) != phiLinks.end()) {
    return;
  }
  bool needsCreate = false;
  if (pred_size(BB) == 1 && !init) {
    if (phiLinks.find(BB->getSinglePredecessor()) != phiLinks.end()) {
        auto *parentLink = phiLinks.find(BB->getSinglePredecessor())->second;
        phiLinks[BB] = new phiNodeLink(parentLink);
    } else {
      needsCreate = true;
    }
  } else if (!init) {
    needsCreate = true;
  }
  if (needsCreate) {
    auto *phi = PHINode::Create(V->getType()->getPointerElementType(), 0,
        "phi." + BB->getName(), &BB->front());
    phiLinks[BB] = new phiNodeLink(phi);
    uniqueNodes.insert(phi);
  }
  for (auto *Succ : successors(BB)) {
    insertPhiNodes(Succ, phiLinks, lastInstructions, uniqueNodes, V);
  }
}

void deleteRedundantPhiNodes(
    DenseMap<BasicBlock *, phiNodeLink *> &phiLinks,
    DenseSet<PHINode *> &uniqueNodes)
{
  for (auto *Phi : uniqueNodes) {
    bool hasSameOperands = true;
    auto *operand = Phi->getOperand(0);
      for (int i = 0; i < Phi->getNumOperands(); ++i) {
        if (operand != Phi->getOperand(i)) {
          hasSameOperands = false;
          break;
        }
      }
      if (hasSameOperands) {
        Phi->replaceAllUsesWith(operand);
        uniqueNodes.erase(Phi);
        phiLinks[Phi->getParent()]->phiNode = nullptr;
        auto *Instr = dyn_cast<Instruction>(operand);
        phiLinks[Phi->getParent()]->parent = phiLinks[Instr->getParent()];
        Phi->eraseFromParent();
      }
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

      auto *BeforeInstr = new LoadInst(V, "load.ptr." + V->getName(), &L->getLoopPredecessor()->back());
      DenseMap<BasicBlock *, phiNodeLink *> phiLinks;
      DenseSet<PHINode *> uniqueNodes;
      DenseMap<BasicBlock *, Instruction *> lastInstructions;
      insertPhiNodes(L->getLoopPredecessor(), phiLinks, lastInstructions, uniqueNodes, V, true);

      lastInstructions[L->getLoopPredecessor()] = BeforeInstr;
      for (auto &P : phiLinks) {
        lastInstructions[P.getFirst()] = P.getSecond()->getPhi();
      }

      DenseSet<BasicBlock *> processedBlocks;
      DenseSet<BasicBlock *> changedLastInst;
      handleLoads(L, L->getLoopPredecessor(), lastInstructions, processedBlocks, changedLastInst,true);

      for (auto *Phi : uniqueNodes) {
        auto *BB = Phi->getParent();
        for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
          Phi->addIncoming(lastInstructions[*Pred], *Pred);
        }
      }
      deleteRedundantPhiNodes(phiLinks, uniqueNodes);

      SmallVector<BasicBlock *, 8> ExitBlocks;
      L->getExitBlocks(ExitBlocks);
      for (auto *BB : ExitBlocks) {
        new StoreInst(phiLinks[BB]->getPhi(), V, BB->getFirstNonPHI());
      }
      eraseAllStoreInstFromLoop(V, L);
      freeLinks(phiLinks);
    }
  });
  return false;
}
