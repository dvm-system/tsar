//===--- PrivateAnalysis.h - Private Variable Analyzer ----------*- C++ -*-===//
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
// This file defines passes to determine formal arguments which can be
// preserved by the function (e.g. saved to the global memory).
//
//===----------------------------------------------------------------------===//

#ifndef SAPFOR_ADDRESSACCESS_H
#define SAPFOR_ADDRESSACCESS_H

#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/Pass.h>
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"

namespace llvm {
  class AddressAccessAnalyser :
          public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;

    AddressAccessAnalyser() : ModulePass(ID) {
      initializeAddressAccessAnalyserPass(*PassRegistry::getPassRegistry());
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override;

    bool runOnModule(Module &M) override;

    static void runOnFunction(Function *F);

    static bool isNonTrivialPointerType(Type *);

    static void setNocaptureToAll(Function* );

    static bool isCaptured(Argument *arg);

    static std::tuple<Value*,int,bool> applyDefinitionConstraint(Value *V, std::map<Value*, int> &registry);

    static std::tuple<Value*,int,bool> applyUsageConstraint(Instruction *I, int opNo, int curRang);

    void print(raw_ostream &OS, const Module *M) const override;

    void releaseMemory() override {};
  };
}

#endif //SAPFOR_ADDRESSACCESS_H
