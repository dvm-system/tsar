//===--- GlobalLiveMemory.h - Global Live Memory Analysis -------*- C++ -*-===//
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
// This file implements passes to determine global live memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_GLOBAL_LIVE_MEMORY_H
#define TSAR_GLOBAL_LIVE_MEMORY_H

#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class GlobalLiveMemory : public ModulePass, private bcl::Uncopyable {
public:
  using IterprocLiveMemoryInfo =
    DenseMap<Function *, std::unique_ptr<tsar::LiveSet>,
      DenseMapInfo<Function *>,
      tsar::TaggedDenseMapPair<
        bcl::tagged<Function *, Function>,
        bcl::tagged<std::unique_ptr<tsar::LiveSet>, tsar::LiveSet>>>;

  static char ID;

  GlobalLiveMemory() : ModulePass(ID) {
    initializeGlobalLiveMemoryPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  const IterprocLiveMemoryInfo &getLiveMemoryInfo() const noexcept {
    return mInterprocLiveMemory;
  }

  IterprocLiveMemoryInfo &getLiveMemoryInfo() noexcept {
    return mInterprocLiveMemory;
  }

private:
  IterprocLiveMemoryInfo mInterprocLiveMemory;
};

/// Results of inter-procedural live memory analysis for an each function in a
/// call graph.
using InterprocLiveMemoryInfo = GlobalLiveMemory::IterprocLiveMemoryInfo;
}
#endif//TSAR_GLOBAL_LIVE_MEMORY_H
