//===- DILoopRetriever.cpp - Loop Debug Info Retriever ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements loop pass which retrieves some debug information for a
// a loop if it is not presented in LLVM IR.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Transform/Mixed/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/InitializePasses.h>
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

/// Returns first known location in a specified range.
/// This function ignores debug and memory marker intrinsics.
template<class ItrT>
DebugLoc firstInRange(const ItrT &BeginItr, const ItrT &EndItr) {
  for (auto I = BeginItr; I != EndItr; ++I) {
    if (!I->getDebugLoc())
      continue;
    if (auto II = dyn_cast<IntrinsicInst>(&*I))
      if (isDbgInfoIntrinsic(II->getIntrinsicID()) ||
          isMemoryMarkerIntrinsic(II->getIntrinsicID()))
            continue;
    return I->getDebugLoc();
  }
  return DebugLoc();
}
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
  auto Header = L->getHeader();
  assert(Header && "Header of the loop must not be null!");
  auto *F = Header->getParent();
  assert(F && "Function which is owner of a loop must not be null!");
  auto DISub = findMetadata(F);
  auto StabLoc = DISub ?
    DILocation::get(F->getContext(), 0, 0, DISub->getScope()) : nullptr;
  bool NeedStabLoc = true;
  if (auto PreBB = L->getLoopPredecessor())
    if (auto Loc = firstInRange(PreBB->rbegin(), PreBB->rend())) {
      MDs.push_back(Loc.getAsMDNode());
      NeedStabLoc = false;
    }
  if (NeedStabLoc)
    if (auto Loc = firstInRange(Header->begin(), Header->end())) {
      MDs.push_back(Loc.getAsMDNode());
      NeedStabLoc = false;
    }
  if (auto Latch = L->getLoopLatch()) {
    auto TI = Latch->getTerminator();
    assert(TI && "Basic block is not well formed!");
    if (TI->getDebugLoc()) {
      if (NeedStabLoc && StabLoc) {
        MDs.push_back(StabLoc);
        MDs.push_back(TI->getDebugLoc().getAsMDNode());
      } else if (!NeedStabLoc) {
        MDs.push_back(TI->getDebugLoc().getAsMDNode());
      }
    }
  }
  LoopID = MDNode::get(L->getHeader()->getContext(), MDs);
  LoopID->replaceOperandWith(0, LoopID);
  L->setLoopID(LoopID);
  return true;
}

Pass * llvm::createDILoopRetrieverPass() { return new DILoopRetrieverPass(); }
