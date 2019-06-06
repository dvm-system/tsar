//=== DeadDeclsElimination.h - Dead Decls Elimination (Clang) ----*- C++ -*===//
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
// This file defines a pass to eliminate dead declarations in a source code.
// Dead declaration is a declaration which is never used. Note, that only
// local declarations can be safely removed.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_DE_DECLS_H
#define TSAR_CLANG_DE_DECLS_H

#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
/// This per-function pass perform source-level elimination of
/// local dead declarations.
class ClangDeadDeclsElimination : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangDeadDeclsElimination() : FunctionPass(ID) {
    initializeClangDeadDeclsEliminationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_CLANG_DE_DECLS_H
