//=== ExpressionMatcher.h - High and Low Level Matcher (Flang) --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// Classes and functions from this file match expressions in Clang AST and
// appropriate expressions in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/Bimap.h"
#include "tsar/Analysis/Flang/Passes.h"
#include "tsar/Support/Tags.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <flang/Parser/parse-tree.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Pass.h>

#ifndef TSAR_FLANG_EXPRESSION_MATCHER_H
#define TSAR_FLANG_EXPRESSION_MATCHER_H

namespace tsar {
class FlangTransformationContext;
}

namespace llvm {
class Value;

/// This per-function pass matches expressions in a source code (Flang AST) and
/// appropriate expressions in low-level LLVM IR.
///
/// At this moment only call expressions are processed.
class FlangExprMatcherPass :
  public FunctionPass, private bcl::Uncopyable {
  template <typename... Ts> struct NodeInfo {
    using ListT = bcl::TypeList<Ts...>;
    using NodeT = llvm::PointerUnion<Ts...>;
  };

  using NodeInfoT = NodeInfo<Fortran::parser::CallStmt *,
                             Fortran::parser::FunctionReference *>;

public:
  using NodeT = NodeInfoT::NodeT;

  using ExprMatcher = tsar::Bimap <
    bcl::tagged<NodeT, tsar::AST>,
    bcl::tagged<llvm::Value *, tsar::IR>>;

  using ExprASTSet = llvm::DenseSet<NodeT>;

  static char ID;

  FlangExprMatcherPass() : FunctionPass(ID) {
    initializeFlangExprMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override {
    mTfmCtx = nullptr;
    mMatcher.clear();
    mUnmatchedAST.clear();
  }

  void print(raw_ostream &OS, const Module *M) const override;

  /// Return expression matcher for the analyzed function.
  const ExprMatcher & getMatcher() const noexcept { return mMatcher; }

  /// Return unmatched expressions in AST.
  const ExprASTSet & getUnmatchedAST() const noexcept { return mUnmatchedAST; }

private:
  tsar::FlangTransformationContext *mTfmCtx{nullptr};
  ExprMatcher mMatcher;
  ExprASTSet mUnmatchedAST;
};

}

#endif//TSAR_FLANG_EXPRESSION_MATCHER_H
