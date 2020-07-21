//=== ExpressionMatcher.cpp - High and Low Level Matcher --------*- C++ -*-===//
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
// Classes and functions from this file match expressions in Clang AST and
// appropriate expressions in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/ExpressionMatcher.h"
#include "tsar/Analysis/Clang/Matcher.h"
#include "tsar/Core/TransformationContext.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/CallSite.h>

using namespace llvm;
using namespace tsar;
using namespace clang;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-expr-matcher"

STATISTIC(NumMatchExpr, "Number of matched expressions");
STATISTIC(NumNonMatchIRExpr, "Number of non-matched IR expressions");
STATISTIC(NumNonMatchASTExpr, "Number of non-matched AST expressions");

namespace {
class MatchExprVisitor :
  public MatchASTBase<Value *, Stmt *>,
  public RecursiveASTVisitor<MatchExprVisitor> {
public:
  MatchExprVisitor(SourceManager &SrcMgr, Matcher &MM,
    UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
      MatchASTBase(SrcMgr, MM, Unmatched, LocMap, MacroMap) {}

  /// Evaluates declarations expanded from a macro and stores such
  /// declaration into location to macro map.
  void VisitFromMacro(Stmt *S) {
    assert(S->getLocStart().isMacroID() &&
      "Expression must be expanded from macro!");
    auto Loc = S->getLocStart();
    if (Loc.isInvalid())
      return;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return;
    auto Pair = mLocToMacro->insert(
      std::make_pair(Loc.getRawEncoding(), TinyPtrVector<Stmt *>(S)));
    if (!Pair.second)
      Pair.first->second.push_back(S);
  }

   bool VisitCallExpr(Expr *E) {
    if (E->getLocStart().isMacroID()) {
      VisitFromMacro(E);
      return true;
    }
    auto ExprLoc = E->getLocStart();
    if (auto *I = findIRForLocation(ExprLoc)) {
      mMatcher->emplace(E, I);
      ++NumMatchExpr;
      --NumNonMatchIRExpr;
    } else {
      mUnmatchedAST->insert(E);
      ++NumNonMatchASTExpr;
    }
    return true;
  }
};
}

bool ClangExprMatcherPass::runOnFunction(Function &F) {
  releaseMemory();
  auto TfmCtx =
    getAnalysis<TransformationEnginePass>().getContext(*F.getParent());
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  MatchExprVisitor::LocToIRMap LocToExpr;
  MatchExprVisitor::LocToASTMap LocToMacro;
  MatchExprVisitor MatchExpr(SrcMgr,
    mMatcher, mUnmatchedAST, LocToExpr, LocToMacro);
  for (auto &I: instructions(F)) {
    CallSite CS(&I);
    if (!CS)
      continue;
    ++NumNonMatchIRExpr;
    auto Loc = I.getDebugLoc();
    if (Loc) {
      auto Pair = LocToExpr.insert(
        std::make_pair(Loc, TinyPtrVector<Value *>(&I)));
      if (!Pair.second)
        Pair.first->second.push_back(&I);
    }
  }
  for (auto &Pair : LocToExpr)
    std::reverse(Pair.second.begin(), Pair.second.end());
  // It is necessary to build LocToExpr map also if FuncDecl is null,
  // because a number of unmatched expressions should be calculated.
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  MatchExpr.TraverseDecl(FuncDecl);
  MatchExpr.matchInMacro(NumMatchExpr, NumNonMatchASTExpr, NumNonMatchIRExpr);
  return false;
}

void ClangExprMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

char ClangExprMatcherPass::ID = 0;

INITIALIZE_PASS_BEGIN(ClangExprMatcherPass, "clang-expr-matcher",
  "High and Low Expression Matcher", false , true)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangExprMatcherPass, "clang-expr-matcher",
  "High and Low Level Expression Matcher", false, true)

FunctionPass * llvm::createClangExprMatcherPass() {
  return new ClangExprMatcherPass;
}
