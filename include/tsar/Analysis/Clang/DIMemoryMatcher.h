//===- DIMemoryMatcher.h - High and Metadata Level Memory Matcher *- C++ -*===//
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
// This file defines a pass to match variable in a source high-level code
// and appropriate metadata-level representations of variables.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_DI_MEMORY_MATCHER_H
#define TSAR_CLANG_DI_MEMORY_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Support/Tags.h"
#include <bcl/utility.h>
#include <bcl/tagged.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Pass.h>

namespace clang {
class VarDecl;
}

namespace llvm {
class DIVariable;

/// A pass to match variable in a source high-level code and appropriate
/// metadata-level representations of variables.
///
/// Note that matcher contains canonical declarations (Decl::getCanonicalDecl).
class ClangDIMemoryMatcherPass : public FunctionPass, private bcl::Uncopyable {
public:
  using DIMemoryMatcher = tsar::Bimap <
    bcl::tagged<clang::VarDecl *, tsar::AST>,
    bcl::tagged<llvm::DIVariable *, tsar::MD>>;

  using MemoryASTSet = DenseSet<clang::VarDecl *>;

  static char ID;

  ClangDIMemoryMatcherPass() : FunctionPass(ID) {
    initializeClangDIMemoryMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override { mMatcher.clear(); mUnmatchedAST.clear(); }

  /// Return memory matcher for the last analyzed function and global variables.
  ///
  /// Note that matcher contains canonical declarations (Decl::getCanonicalDecl).
  const DIMemoryMatcher & getMatcher() const noexcept { return mMatcher; }

  /// Return umatched variables in AST.
  ///
  /// For example, macro may prevent successful match.
  /// Note that matcher contains canonical declarations (Decl::getCanonicalDecl).
  const MemoryASTSet & getUnmatchedAST() const noexcept { return mUnmatchedAST; }

private:
  DIMemoryMatcher mMatcher;
  MemoryASTSet mUnmatchedAST;
};

/// This pass match only global variables.
class ClangDIGlobalMemoryMatcherPass :
  public ModulePass, private bcl::Uncopyable {
public:
  using DIMemoryMatcher = ClangDIMemoryMatcherPass::DIMemoryMatcher;
  using MemoryASTSet = ClangDIMemoryMatcherPass::MemoryASTSet;

  static char ID;

  ClangDIGlobalMemoryMatcherPass() : ModulePass(ID) {
    initializeClangDIGlobalMemoryMatcherPassPass(
      *PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override { mMatcher.clear(); mUnmatchedAST.clear(); }

  /// Return memory matcher for the last analyzed function and global variables.
  ///
  /// Note that matcher contains canonical declarations (Decl::getCanonicalDecl).
  const DIMemoryMatcher & getMatcher() const noexcept { return mMatcher; }

  /// Return umatched variables in AST.
  ///
  /// For example, macro may prevent successful match.
  /// Note that matcher contains canonical declarations (Decl::getCanonicalDecl).
  const MemoryASTSet & getUnmatchedAST() const noexcept { return mUnmatchedAST; }

private:
  DIMemoryMatcher mMatcher;
  MemoryASTSet mUnmatchedAST;
};
}
#endif//TSAR_CLANG_DI_MEMORY_MATCHER_H
