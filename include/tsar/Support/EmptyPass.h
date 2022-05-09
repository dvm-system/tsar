//===---- EmptyPass.h -------- Trivial Empty Pass ----------------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a trivial empty pass which does nothing. It can be use
// as a stub if configuration of the build does not provide some passes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_EMPTY_PASS_H
#define TSAR_EMPTY_PASS_H

#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
void initializeEmptyFunctionPassPass(PassRegistry &);

class EmptyFunctionPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  EmptyFunctionPass() : FunctionPass(ID) {
    initializeEmptyFunctionPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override { return false; }
};

inline FunctionPass *createEmptyFunctionPass() { return new EmptyFunctionPass; }

void initializeEmptyModulePassPass(PassRegistry &);

class EmptyModulePass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  EmptyModulePass() : ModulePass(ID) {
    initializeEmptyModulePassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(Module &) override { return false; }
};

inline ModulePass *createEmptyModulePass() { return new EmptyModulePass; }
}
#endif//TSAR_EMPTY_PASS_H

