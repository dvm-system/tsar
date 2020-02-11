//===-- GlobalDefinedMemory.h -- Global Defined Memory Analysis -*- C++ -*-===//
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
//===---------------------------------------------------------------------===//
//
// This file implements a pass to determine global defined memory locations.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_GLOBAL_DEFINED_MEMORY_H
#define TSAR_GLOBAL_DEFINED_MEMORY_H

#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class GlobalDefinedMemory : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  GlobalDefinedMemory() : ModulePass(ID) {
    initializeGlobalDefinedMemoryPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &SCC) override;
  void getAnalysisUsage(AnalysisUsage& AU) const override;

  /// Return results of interprocedural reach definition analysis.
  tsar::InterprocDefUseInfo & getInterprocDefUseInfo() noexcept {
    return mInterprocDUInfo;
  }

  /// Return results of interprocedural reach definition analysis.
  const tsar::InterprocDefUseInfo & getInterprocDefUseInfo() const noexcept {
    return mInterprocDUInfo;
  }

private:
  tsar::InterprocDefUseInfo mInterprocDUInfo;
};
}
#endif//TSAR_GLOBAL_DEFINED_MEMORY_H
