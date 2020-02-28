#include <tsar/Analysis/DFRegionInfo.h>
#include <tsar/Analysis/Memory/DIMemoryTrait.h>
#include <tsar/Analysis/Memory/PointerReduction.h>
#include <tsar/Core/Query.h>
#include <tsar/Support/IRUtils.h>
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

Value *getFirstAssociativeOp(Value *V) {
  for (auto *User : V->users()) {
    if (auto *I = dyn_cast<Instruction>(User)) {
      if (I->isAssociative()) {
        return I;
      }
    }
  }
  return nullptr;
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
    SmallDenseMap<Value *, SetVector<LoadInst *>> Values;
    if (!Pool) {
      Pool = make_unique<DIMemoryTraitRegionPool>();
    } else {
      for (auto &T : *Pool) {
        if (T.is<trait::Anti>()) {
          for (auto &V : *T.getMemory()) {
            if (V.pointsToAliveValue()) {
              for (auto *User : V->users()) {
                if (auto *LI = dyn_cast<LoadInst>(User)) {
                  if (L->contains(LI)) {
                    Values[LI->getPointerOperand()].insert(LI);
                  }
                }
              }
            }
          }
        }
      }
    }

    for (auto &Pair : Values) {
      auto *V = Pair.first;
      auto &Loads = Pair.second;

      if (Loads.empty()) {
        continue;
      }

      // we cannot replace anything if the pointer value can change somehow
      if (!validateValue(V, L)) {
        break;
      }

      // check that instructions that use Loads are all in the same basic block
      bool hasSameBB = true;
      for (auto *Load : Loads) {
        for (auto *User : Load->users()) {
          if (auto *Instr = dyn_cast<Instruction>(User)) {
            if (Load->getParent() != Instr->getParent()) {
              hasSameBB = false;
              break;
            }
          }
        }
        if (!hasSameBB) {
          break;
        }
      }
      if (!hasSameBB) {
        continue;
      }

      auto *BeforeInstr = new LoadInst(V, "tmp_name", &L->getLoopPredecessor()->back());
      BeforeInstr->copyMetadata(*Loads[0]);

      auto *Phi = PHINode::Create(V->getType()->getPointerElementType(), 0, "123", &L->getBlocks().front()->front());
      Phi->addIncoming(BeforeInstr, L->getLoopPreheader());

      Value *lastVal = Phi;
      for (auto *Load : Loads) {
        if (auto *Instr = getFirstAssociativeOp(Load)) {
          Load->replaceAllUsesWith(lastVal);
          lastVal = Instr;
          Load->eraseFromParent();
        }
      }
      Phi->addIncoming(lastVal, L->getBlocks().back());

      SmallVector<BasicBlock *, 8> ExitBlocks;
      L->getExitBlocks(ExitBlocks);
      for (auto *BB : ExitBlocks) {
        new StoreInst(Phi, V, &BB->front());
      }

      eraseAllStoreInstFromLoop(V, L);
    }
  });
  return false;
}
