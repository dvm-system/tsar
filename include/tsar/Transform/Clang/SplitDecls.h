//===- MyFirstPass.h - Source-level Renaming of Local Objects -- *- C++ -*-===//
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
// This file declares a pass to perform renaming of objects into a specified
// scope. The goal of this transformation is to ensure that there is no
// different objects with the same name at a specified scope. The transformation
// also guaranties that names of objects declared in a specified scope do not
// match any name from other scopes.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_CLANG_SPLIT_DECLS_H
#define TSAR_CLANG_SPLIT_DECLS_H

#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeLoc.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <map>
#include <memory>
#include <vector>

namespace llvm {
/// This pass separates variable declaration statements that contain multiple
/// variable declarations at once into single declarations.
class ClangSplitDeclsPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangSplitDeclsPass() : ModulePass(ID) {
    initializeClangSplitDeclsPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_CLANG_SPLIT_DECLS_H
