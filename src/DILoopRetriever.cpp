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
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/IR/DebugInfoMetadata.h>

using namespace llvm;

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
  auto LocRange = L->getLocRange();
  LocRange.getStart().getAsMDNode();
  if (LocRange.getStart() && LocRange.getStart().get())
    MDs.push_back(LocRange.getStart().get());
  if (LocRange.getEnd() && LocRange.getEnd().get())
    MDs.push_back(LocRange.getEnd().get());
  LoopID = MDNode::get(L->getHeader()->getContext(), MDs);
  LoopID->replaceOperandWith(0, LoopID);
  L->setLoopID(LoopID);
  return true;
}

Pass * llvm::createDILoopRetrieverPass() { return new DILoopRetrieverPass(); }
