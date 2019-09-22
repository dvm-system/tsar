//=== PerfectLoop.h --- High Level Perfect Loop Analyzer --------*- C++ -*-===//
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
// This file defines classes to identify perfect for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_PERFECT_LOOP_H
#define TSAR_CLANG_PERFECT_LOOP_H

#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <llvm/ADT/DenseSet.h>

namespace tsar {
class DFNode;

/// Set of perfect loops.
using PerfectLoopInfo = llvm::DenseSet<DFNode *>;
}

namespace llvm {
/// \brief This per-function pass determines perfect for-loops in a source code.
///
/// A for-loop is treated as perfect if it has no internal for-loops or if it
/// has only one internal for-loop and there are no other statements between
/// loop bounds.
class ClangPerfectLoopPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  ClangPerfectLoopPass() : FunctionPass(ID) {
    initializeClangPerfectLoopPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about perfect loops in an analyzed function.
  tsar::PerfectLoopInfo & getPerfectLoopInfo() noexcept {
    return mPerfectLoopInfo;
  }

  /// Returns information about perfect loops in an analyzed function.
  const tsar::PerfectLoopInfo & getPerfectLoopInfo() const noexcept {
    return mPerfectLoopInfo;
  }

  // Determines perfect loops in a specified functions.
  bool runOnFunction(Function &F) override;

  void releaseMemory() override { mPerfectLoopInfo.clear(); }

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  tsar::PerfectLoopInfo mPerfectLoopInfo;
};
}
#endif// TSAR_CLANG_PERFECT_LOOP_H
