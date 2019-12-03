//===--- DefinedMemory.cpp --- Defined Memory Analysis ----------*- C++ -*-===//
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
#ifndef TSAR_GLOBAL_LIVE_MEMORY_H
#define TSAR_GLOBAL_LIVE_MEMORY_H

#include <tsar/Analysis/Memory/Passes.h>
#include <tsar/Analysis/Memory/LiveMemory.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {

  class GlobalLiveMemory : public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;

    typedef DenseMap<const llvm::Function*, std::unique_ptr<tsar::LiveSet>> IterprocLiveMemoryInfo;

    GlobalLiveMemory() : ModulePass(ID){
      initializeGlobalLiveMemoryPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module &SCC) override;
    void getAnalysisUsage(AnalysisUsage& AU) const override;

    const IterprocLiveMemoryInfo& getIterprocLiveMemoryInfo() const noexcept {
      return mIterprocLiveMemoryInfo;
    }

    IterprocLiveMemoryInfo& getIterprocLiveMemoryInfo() noexcept {
      return mIterprocLiveMemoryInfo;
    }

  private:
    IterprocLiveMemoryInfo mIterprocLiveMemoryInfo;
  };

  typedef GlobalLiveMemory::IterprocLiveMemoryInfo IterprocLiveMemoryInfo;

}
#endif//TSAR_GLOBAL_LIVE_MEMORY_H
