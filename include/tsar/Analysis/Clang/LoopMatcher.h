//===- LoopMatcher.h ---- High and Low Level Loop Matcher ------*- C++ -*-===//
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
// Classes and functions from this file match loops in a source high-level
// code and appropriate loops in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LOOP_MATCHER_H
#define TSAR_LOOP_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Support/Tags.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Pass.h>

namespace clang {
class Stmt;
class Decl;
}

namespace llvm {
class Function;
class Loop;

/// \brief This per-function pass matches different loops in a source high level
/// code and appropriated loops in low-level LLVM IR.
///
/// This pass may changes !llvm.loop metadata, it updates locations for implicit
/// loops that have been successfully matched. Metadata will be update if they
/// have been already set only.
///
/// TODO (kaniander@gmail.com): Implicit loops which are expanded from macro are
/// not evaluated, because in LLVM IR these loops have locations equal to
/// expansion location. So it is not possible to determine token in macro
/// body where these loops starts without additional analysis of AST.
class LoopMatcherPass :
  public FunctionPass, private bcl::Uncopyable {
public:
  typedef tsar::Bimap<
    bcl::tagged<clang::Stmt *, tsar::AST>,
    bcl::tagged<llvm::Loop *, tsar::IR>> LoopMatcher;

  typedef llvm::DenseSet<clang::Stmt *> LoopASTSet;

  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  LoopMatcherPass() : FunctionPass(ID) {
    initializeLoopMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  /// Matches different loops.
  bool runOnFunction(Function &F) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Returns loop matcher for the last analyzed function.
  const LoopMatcher & getMatcher() const noexcept { return mMatcher; }

  /// \brief Returns unmatched loop in AST.
  ///
  /// For example, if loop in a source code always have only one iteration and
  /// this is obviously determined during generation of LLVM IR then there is no
  /// appropriate loop in LLVM IR. If macro contains implicit and explicit loops
  /// the explicit loops also is not going to be evaluated.
  const LoopASTSet & getUnmatchedAST() const noexcept { return mUnmatchedAST; }

  /// Returns high-level declaration of the last analyzed function.
  clang::Decl * getFunctionDecl() const noexcept { return mFuncDecl; }

  /// Releases allocated memory.
  void releaseMemory() override {
    mMatcher.clear();
    mUnmatchedAST.clear();
    mFuncDecl = nullptr;
  }

private:
  LoopMatcher mMatcher;
  LoopASTSet mUnmatchedAST;
  clang::Decl * mFuncDecl;
};
}

#endif//TSAR_LOOP_MATCHER_H