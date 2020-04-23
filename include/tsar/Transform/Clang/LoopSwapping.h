//=== LoopSwapping.h - Loop Swapping (Clang) ----*- C++ -*===//
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
//===---------------------------------------------------------------------===//
//
// The file declares a pass to perform swapping of specific loops.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_LOOP_SWAPPING_H
#define TSAR_CLANG_LOOP_SWAPPING_H

#include "tsar/Support/Tags.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Support/GlobalOptions.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace tsar {
  class TransformationContext;
  class DIAliasNode;
  class DIAliasTree;
}

namespace clang {
  class SourceRange;
  class Rewriter;
}

namespace llvm {

class Loop;
class MDNode;

class ClangLoopSwapping : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangLoopSwapping() : FunctionPass(ID) {
    initializeClangLoopSwappingPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override;

private:
  void initializeProviderOnServer();

  bool LoadDependenceAnalysisInfo(Function &F);
  void SwapLoops(const std::vector<std::vector<clang::SourceRange>> &mRangePairs,
                 const std::vector<std::vector<Loop *>> &mLoopPairs);
  std::vector<const tsar::DIAliasTrait *> GetLoopTraits(llvm::MDNode *LoopID);
  tsar::DIDependenceSet &GetLoopDepSet(MDNode *LoopID);
  bool IsSwappingAvailable(std::vector<Loop *> loops);

  bool HasSameReductionKind(std::vector<const tsar::DIAliasTrait *> &traits0,
                            std::vector<const tsar::DIAliasTrait *> &traits1,
                            tsar::SpanningTreeRelation<tsar::DIAliasTree *> &STR);
  bool HasTrueOrAntiDependence(std::vector<const tsar::DIAliasTrait *> &traits0,
                         std::vector<const tsar::DIAliasTrait *> &traits1,
                         tsar::SpanningTreeRelation<tsar::DIAliasTree *> &STR);

  llvm::Function *mFunction = nullptr;
  tsar::TransformationContext *mTfmCtx = nullptr;
  tsar::DIDependencInfo *DIDepInfo = nullptr;
  tsar::DIAliasTree *DIAT = nullptr;
  const tsar::GlobalOptions *mGlobalOpts = nullptr;
  tsar::AnalysisSocket *mSocket = nullptr;
  std::function<tsar::ObjectID(tsar::ObjectID)> getLoopID;
};

}

#endif//TSAR_CLANG_LOOP_SWAPPING_H
