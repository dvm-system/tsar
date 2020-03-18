//===- ParallelLoop.h ------ Parallel Loop Analysis  ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file defines passes to determine parallelization opportunities of loops.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_PARALLEL_LOOP_H
#define TSAR_ANALYSIS_PARALLEL_LOOP_H

#include "tsar/Analysis/Parallel/Passes.h"
#include "bcl/utility.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>

namespace llvm {
class Loop;
}

namespace tsar {
class ParallelInfo {
public:
  ParallelInfo(bool HostOnly = true) : mHostOnly(HostOnly) {}
  bool isHostOnly() const noexcept { return mHostOnly; }
private:
  bool mHostOnly;
};

/// List of loops which could be executed in a parallel way.
using ParallelLoopInfo = llvm::DenseMap<const llvm::Loop *, ParallelInfo>;
}

namespace llvm {
/// Determine loops which could be executed in a parallel way.
class ParallelLoopPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ParallelLoopPass() : FunctionPass(ID) {
    initializeParallelLoopPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override { mParallelLoops.clear(); }

  /// Return list of loops which could be executed in a parallel way.
  tsar::ParallelLoopInfo &getParallelLoopInfo() { return mParallelLoops; }

  /// Return list of loops which could be executed in a parallel way.
  const tsar::ParallelLoopInfo &getParallelLoopInfo() const {
    return mParallelLoops;
  }

private:
  tsar::ParallelLoopInfo mParallelLoops;
};
}

#endif//TSAR_ANALYSIS_PARALLEL_LOOP_H
