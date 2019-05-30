//===--- FormatPass.h - Source-level Reformat Pass (Clang) ------*- C++ -*-===//
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
// This file declares a pass to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_FORMAT_PASS_H
#define TSAR_CLANG_FORMAT_PASS_H

#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class Module;

/// This pass tries to reformat sources which have been transformed by
/// previous passes.
class ClangFormatPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  /// \brief Creates pass which adds a specified suffix to transformed sources
  /// (see GlobalOptions) and performs formatting if
  /// (GlobalOptions::NoFormat) is not set.
  ///
  /// Note, if suffix is specified it will be added before the file extension.
  /// If suffix is empty, the original will be stored to
  /// <filenmae>.<extension>.orig and then it will be overwritten.
  ClangFormatPass() : ModulePass(ID) {
    initializeClangFormatPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_CLANG_FORMAT_PASS_H
