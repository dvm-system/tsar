//===- DILoopRetriever.cpp - Loop Debug Info Retriever ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements loop pass which retrieves some debug information for a
// a loop if it is not presented in LLVM IR.
//
//===----------------------------------------------------------------------===//

#include "tsar_pass.h"
#include "tsar_utility.h"
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/IR/DebugInfoMetadata.h>

using namespace llvm;
using namespace tsar;

namespace {
/// This retrieves some debug information for a loop if it is not presented
/// in LLVM IR.
class DILoopRetrieverPass : public LoopPass, private bcl::Uncopyable {
public:
  /// Pass ID, replacement for typeid.
  static char ID;

  /// Constructor.
  DILoopRetrieverPass() : LoopPass(ID) {
    initializeDILoopRetrieverPassPass(*PassRegistry::getPassRegistry());
  }

  /// Retrieves debug information for a specified loop.
  bool runOnLoop(Loop *L, LPPassManager &LPM) override;

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.setPreservesAll();
  }
};
}
char DILoopRetrieverPass::ID = 0;
INITIALIZE_PASS_BEGIN(DILoopRetrieverPass, "loop-diretriever",
                      "Loop Debug Info Retriever", true, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(DILoopRetrieverPass, "loop-diretriever",
                    "Loop Debug Info Retriever", true, false)

bool DILoopRetrieverPass::runOnLoop(Loop *L, LPPassManager &LPM) {
  auto LoopID = L->getLoopID();
  // At this moment the first operand is not initialized, it will be replaced
  // with LoopID later.
  SmallVector<Metadata *, 3> MDs(1);
  if (LoopID) {
    for (unsigned I = 1, EI = LoopID->getNumOperands(); I < EI; ++I) {
      MDNode *Node = cast<MDNode>(LoopID->getOperand(I));
      if (isa<DILocation>(Node))
        return false;
      MDs.push_back(Node);
    }
  }
  auto *F = L->getHeader()->getParent();
  assert(F && "Function which is owner of a loop must not be null!");
  auto DISub = F->getSubprogram();
  auto StabLoc = DISub ?
    DILocation::get(F->getContext(), 0, 0, DISub->getScope()) : nullptr;
  auto DWLang = getLanguage(*F);
  if (DWLang && isFortran(*DWLang)) {
    bool NeedStabLoc = true;
    if (auto Preheader = L->getLoopPreheader()) {
      auto I = Preheader->rbegin();
      ++I;
      if (I->getDebugLoc() && I->getDebugLoc().get()) {
        MDs.push_back(I->getDebugLoc().get());
        NeedStabLoc = false;
      }
    }
    if (auto Latch = L->getLoopLatch()) {
      auto TI = Latch->getTerminator();
      assert(TI && "Basic block is not well formed!");
      if (TI->getDebugLoc() && TI->getDebugLoc().get()) {
        if (NeedStabLoc && StabLoc) {
          MDs.push_back(StabLoc);
          MDs.push_back(TI->getDebugLoc().get());
        } else if (!NeedStabLoc) {
          MDs.push_back(TI->getDebugLoc().get());
        }
      }
    }
  }
  if (!isFortran(*DWLang) || MDs.size() == 1 || !isa<DILocation>(MDs.back())) {
    auto LocRange = L->getLocRange();
    LocRange.getStart().getAsMDNode();
    bool NeedStabLoc = true;
    if (LocRange.getStart() && LocRange.getStart().get()) {
      MDs.push_back(LocRange.getStart().get());
      NeedStabLoc = false;
    }
    if (LocRange.getEnd() && LocRange.getEnd().get()) {
      if (NeedStabLoc && StabLoc) {
        MDs.push_back(StabLoc);
        MDs.push_back(LocRange.getEnd().get());
      } else if (!NeedStabLoc) {
        MDs.push_back(LocRange.getEnd().get());
      }
    }
  }
  LoopID = MDNode::get(L->getHeader()->getContext(), MDs);
  LoopID->replaceOperandWith(0, LoopID);
  L->setLoopID(LoopID);
  return true;
}

Pass * llvm::createDILoopRetrieverPass() { return new DILoopRetrieverPass(); }
