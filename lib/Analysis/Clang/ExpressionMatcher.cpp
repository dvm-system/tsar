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
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Clang/Matcher.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

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
  public MatchASTBase<Value *, DynTypedNode>,
  public RecursiveASTVisitor<MatchExprVisitor> {
public:
  MatchExprVisitor(SourceManager &SrcMgr, Matcher &MM,
    UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
      MatchASTBase(SrcMgr, MM, Unmatched, LocMap, MacroMap) {}

  /// Evaluates declarations expanded from a macro and stores such
  /// declaration into location to macro map.
  void VisitFromMacro(DynTypedNode &&N, SourceLocation Loc) {
    assert(Loc.isMacroID() && "Expression must be expanded from macro!");
    if (Loc.isInvalid())
      return;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return;
    auto I{mLocToMacro->try_emplace(Loc.getRawEncoding()).first};
    I->second.push_back(std::move(N));
  }

  void VisitItem(DynTypedNode &&N, SourceLocation Loc) {
    LLVM_DEBUG(dbgs() << "[EXPR MATCHER]: match at";
               Loc.print(dbgs(), *mSrcMgr); dbgs() << "\n");
    if (Loc.isMacroID()) {
      VisitFromMacro(std::move(N), Loc);
    } else if (auto *I = findIRForLocation(Loc)) {
      mMatcher->emplace(std::move(N), I);
      ++NumMatchExpr;
      --NumNonMatchIRExpr;
    } else {
      mUnmatchedAST->insert(std::move(N));
      ++NumNonMatchASTExpr;
    }
  }

  bool VisitVarDecl(VarDecl *D) {
    LLVM_DEBUG(dbgs() << "[EXPR MATCHER]: visit " << D->getDeclKindName()
                      << D->getName());
    VisitItem(DynTypedNode::create(*D), D->getLocation());
    return true;    
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    LLVM_DEBUG(dbgs() << "[EXPR MATCHER: visit " << S->getStmtClassName()
                      << "\n");
    if (auto CE = dyn_cast<CallExpr>(S)) {
      if (!CE->getDirectCallee()) {
        // We match expression which computes callee before this call.
        if (!TraverseStmt(CE->getCallee()))
          return false;
      }
      VisitItem(DynTypedNode::create(*S), S->getBeginLoc());
      for (auto Arg : CE->arguments())
        if (!TraverseStmt(Arg))
          return false;
      return true;
    }
    if (auto UO = dyn_cast<clang::UnaryOperator>(S);
        UO && (UO->isPrefix() || UO->isPostfix())) {
      // Match order: `load` then `store`. Note, that `load` and `store` have
      // the same location in a source code.
      // For `++ <expr>` we match `++` with store and `<expr>` with load.
      VisitItem(DynTypedNode::create(*UO->getSubExpr()), UO->getOperatorLoc());
      VisitItem(DynTypedNode::create(*S), UO->getOperatorLoc());
      return TraverseStmt(UO->getSubExpr());
    }
    if (isa<ReturnStmt>(S) || isa<DeclRefExpr>(S) ||
        isa<clang::UnaryOperator>(S) &&
            cast<clang::UnaryOperator>(S)->getOpcode() ==
                clang::UnaryOperatorKind::UO_Deref)
      VisitItem(DynTypedNode::create(*S), S->getBeginLoc());
    else if (auto BO = dyn_cast<clang::BinaryOperator>(S);
             BO && BO->isAssignmentOp())
      VisitItem(DynTypedNode::create(*S), BO->getExprLoc());
    else if (auto *ME = dyn_cast<MemberExpr>(S))
      VisitItem(DynTypedNode::create(*S), ME->getMemberLoc());
    return RecursiveASTVisitor::TraverseStmt(S);
  }
};
}

void ClangExprMatcherPass::print(raw_ostream &OS, const llvm::Module *M) const {
  if (mMatcher.empty())
    return;
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  auto TfmCtx = TfmInfo->getContext(*const_cast<Module *>(M));
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  for (auto &Match : mMatcher) {
    tsar::print(OS, cast<Instruction>(Match.get<IR>())->getDebugLoc(),
                GO.PrintFilenameOnly);
    if (auto *S{Match.get<AST>().get<Stmt>()}) {
      OS << " ";
      OS << S->getStmtClassName();
      if (auto UO{ dyn_cast<clang::UnaryOperator>(S) })
        OS << " '" << clang::UnaryOperator::getOpcodeStr(UO->getOpcode())
           << "'";
      else if (auto BO{ dyn_cast<clang::BinaryOperator>(S) })
        OS << " '" << clang::BinaryOperator::getOpcodeStr(BO->getOpcode())
           << "'";
    } else if (auto * D{ Match.get<AST>().get<Decl>() }) {
      OS << " ";
      OS << D->getDeclKindName();
      if (auto ND{ dyn_cast<NamedDecl>(D) })
        OS << " '" << ND->getName() << "'";
    }
    Match.get<IR>()->print(OS);
    OS << "\n";
  }
}

bool ClangExprMatcherPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  if (!TfmInfo)
    return false;
  auto TfmCtx = TfmInfo->getContext(*F.getParent());
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  MatchExprVisitor::LocToIRMap LocToExpr;
  MatchExprVisitor::LocToASTMap LocToMacro;
  MatchExprVisitor MatchExpr(SrcMgr,
    mMatcher, mUnmatchedAST, LocToExpr, LocToMacro);
  for (auto &I: instructions(F)) {
    if (auto II = llvm::dyn_cast<IntrinsicInst>(&I);
        II && (isDbgInfoIntrinsic(II->getIntrinsicID()) ||
               isMemoryMarkerIntrinsic(II->getIntrinsicID())))
      continue;
    if (!isa<CallBase>(I) && !isa<LoadInst>(I) && !isa<StoreInst>(I) &&
        !isa<ReturnInst>(I))
      continue;
    ++NumNonMatchIRExpr;
    auto Loc = I.getDebugLoc();
    if (Loc) {
      auto Itr{ LocToExpr.try_emplace(Loc).first };
      Itr->second.push_back(&I);
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
  MatchExpr.matchInMacro(NumMatchExpr, NumNonMatchASTExpr, NumNonMatchIRExpr,
                         true);
  return false;
}

void ClangExprMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

char ClangExprMatcherPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangExprMatcherPass, "clang-expr-matcher",
  "High and Low Expression Matcher", false , true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(ClangExprMatcherPass, "clang-expr-matcher",
  "High and Low Level Expression Matcher", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

FunctionPass * llvm::createClangExprMatcherPass() {
  return new ClangExprMatcherPass;
}
