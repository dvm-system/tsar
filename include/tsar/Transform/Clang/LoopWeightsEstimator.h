//=== LoopWeightsEstimator.h - Loop Weights Estimator (Clang) --*- C++ -*===//
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
// This file declares a pass to estimate loop weights in a source code.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_LOOP_WEIGHTS_H
#define TSAR_CLANG_LOOP_WEIGHTS_H


#include <bcl/utility.h>
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/IR/Metadata.h>
#include <llvm/Pass.h>

#include <map>


namespace llvm {
/// This per-module pass performs estimation of loop weights
class ClangLoopWeightsEstimator : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangLoopWeightsEstimator();

  bool runOnModule(Module &M) override;
  const std::map<MDNode *, uint64_t> &getLoopWeights() const noexcept;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::map<Function *, uint64_t> mFunctionBaseWeights{};
  std::map<MDNode *, uint64_t> mLoopBaseWeights{};
  std::map<MDNode *, uint64_t> mLoopEffectiveWeights{};
};
}
#endif//TSAR_CLANG_LOOP_WEIGHTS_H
