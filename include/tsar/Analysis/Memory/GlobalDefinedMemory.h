//===--- GlobalDefinedMemory.h --- Global Defined Memory Analysis ----------*- C++ -*-===//
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
// This file implements pass to determine global defined memory locations.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_GLOBAL_DEFINED_MEMORY_H
#define TSAR_GLOBAL_DEFINED_MEMORY_H

#include <tsar/Analysis/Memory/Passes.h>
#include <tsar/Analysis/Memory/DefinedMemory.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
  class GlobalDefinedMemory : public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;
    GlobalDefinedMemory() : ModulePass(ID) {
      initializeGlobalDefinedMemoryPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module &SCC) override;
    void getAnalysisUsage(AnalysisUsage& AU) const override;

    /// Returns results of reach definition analysis.
    tsar::InterprocDefInfo & getInterprocDefInfo() noexcept { return mInterprocDefInfo; }

    /// Returns results of reach definition analysis.
    const tsar::InterprocDefInfo & getInterprocDefInfo() const noexcept {
      return mInterprocDefInfo;
    }


  private:
    tsar::InterprocDefInfo mInterprocDefInfo;
  };

}
#endif//TSAR_GLOBAL_DEFINED_MEMORY_H
