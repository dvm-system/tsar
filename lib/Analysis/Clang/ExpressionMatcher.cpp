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
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
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
  public ClangMatchASTBase<MatchExprVisitor, Value *, DynTypedNode>,
  public RecursiveASTVisitor<MatchExprVisitor> {

  enum AccessKind : uint8_t {
    AK_Store,
    AK_AddrOf,
    AK_Other
  };

public:
  MatchExprVisitor(SourceManager &SrcMgr, Matcher &MM,
    UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
      ClangMatchASTBase(SrcMgr, MM, Unmatched, LocMap, MacroMap) {}

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
    LLVM_DEBUG(dbgs() << "[EXPR MATCHER]: match at ";
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
                      << D->getName() << "\n");
    VisitItem(DynTypedNode::create(*D), D->getLocation());
    return true;
  }

  bool TraverseStmt(Stmt *S) {
    struct StashParent {
      StashParent(Stmt *S, SmallVectorImpl<Stmt *> &Ps) : Parents(Ps) {
        Parents.push_back(S);
      }
      ~StashParent() { Parents.pop_back(); }
      SmallVectorImpl<Stmt *> &Parents;
    };
    if (!S)
      return true;
    LLVM_DEBUG(dbgs() << "[EXPR MATCHER]: visit " << S->getStmtClassName()
                      << "\n");
    if (auto CE = dyn_cast<CallExpr>(S)) {
      if (!CE->getDirectCallee()) {
        [[maybe_unused]] StashParent Stash{CE->getCallee(), mParents};
        // We match expression which computes callee before this call.
        if (!TraverseStmt(CE->getCallee()))
          return false;
      }
      VisitItem(DynTypedNode::create(*S), S->getBeginLoc());
      for (auto Arg : CE->arguments()) {
        [[maybe_unused]] StashParent Stash{Arg, mParents};
        if (!TraverseStmt(Arg))
          return false;
      }
      return true;
    }
    if (auto *U{dyn_cast<UnaryExprOrTypeTraitExpr>(S)};
        U && U->isArgumentType()) {
      // If the statement is sizeof(int[M][K]), references to M and K are
      // visited twice, so we traverse them manually to avoid double matching.
      if (auto *T{
              dyn_cast<VariableArrayType>(U->getArgumentType().getTypePtr())}) {
        for (auto *C : U->children()) {
          [[maybe_unused]] StashParent Stash{C, mParents};
          if (!TraverseStmt(C))
            return false;
        }
        return true;
      }
    }
    if (auto *SE{dyn_cast<ArraySubscriptExpr>(S)}) {
      {
        // We use scope to reload stash on exit.
        [[maybe_unused]] StashParent Stash{S, mParents};
        if (!RecursiveASTVisitor::TraverseStmt(S))
          return false;
      }
      auto [InArraySubscriptBase, AK] =
          isInArraySubscriptBaseAndAccessKind(S);
      // Match current statement if it is an innermost array subscript
      // expression. Note, that assignment-like expressions have been already
      // matched.
      if (!mHasChildArraySubscript && AK != AK_Store && AK != AK_AddrOf)
        VisitItem(DynTypedNode::create(*SE), SE->getExprLoc());
      mHasChildArraySubscript = InArraySubscriptBase;
      return true;
    }
    if (auto UO = dyn_cast<clang::UnaryOperator>(S);
        UO && (UO->isPrefix() || UO->isPostfix())) {
      // Match order: `load` then `store`. Note, that `load` and `store` have
      // the same location in a source code.
      // For `++ <expr>` we match `++` with store and `<expr>` with load.
      VisitItem(DynTypedNode::create(*UO->getSubExpr()), UO->getOperatorLoc());
      VisitItem(DynTypedNode::create(*S), UO->getOperatorLoc());
      [[maybe_unused]] StashParent Stash{UO->getSubExpr(), mParents};
      return TraverseStmt(UO->getSubExpr());
    }
    if (auto DRE{dyn_cast<DeclRefExpr>(S)}) {
      // There is no load from pointer if the variable has an array type.
      // We do not match reference to the array name to avoid multiple match
      // with a single `load` instruction (ArraySubscriptExpr must be matched
      // only).
      if (auto VD{dyn_cast<VarDecl>(DRE->getDecl())};
          VD && isa<clang::ArrayType>(VD->getType()) &&
          std::get<bool>(isInArraySubscriptBaseAndAccessKind(S)))
        return true;
    }
    if (isa<ReturnStmt>(S) || isa<DeclRefExpr>(S) ||
        isa<clang::UnaryOperator>(S) &&
            cast<clang::UnaryOperator>(S)->getOpcode() ==
                clang::UnaryOperatorKind::UO_Deref) {
      VisitItem(DynTypedNode::create(*S), S->getBeginLoc());
    } else if (auto BO = dyn_cast<clang::BinaryOperator>(S);
             BO && BO->isAssignmentOp()) {
      // In case of compound assignment (for example,  +=) match load at first.
      if (BO->isCompoundAssignmentOp())
        VisitItem(DynTypedNode::create(*BO->getLHS()), BO->getExprLoc());
      VisitItem(DynTypedNode::create(*S), BO->getExprLoc());
    } else if (auto *ME = dyn_cast<MemberExpr>(S)) {
      VisitItem(DynTypedNode::create(*S), ME->getMemberLoc());
    }
    [[maybe_unused]] StashParent Stash{S, mParents};
    return RecursiveASTVisitor::TraverseStmt(S);
  }

private:
  std::tuple<bool, AccessKind>
  isInArraySubscriptBaseAndAccessKind(const Stmt *S) {
    auto *Prev{S};
    bool IsInArraySubscriptBase{false};
    for (auto *P : reverse(mParents)) {
      if (auto *SE{dyn_cast<ArraySubscriptExpr>(P)}) {
        if (SE->getBase() != Prev)
          return std::tuple{IsInArraySubscriptBase, AK_Other};
        IsInArraySubscriptBase = true;
      } else {
        if (auto BO{dyn_cast<clang::BinaryOperator>(P)};
            BO && BO->isAssignmentOp() && BO->getLHS() == Prev)
          return std::tuple{IsInArraySubscriptBase, AK_Store};
        if (auto UO{dyn_cast<clang::UnaryOperator>(P)})
          if (UO->isPrefix() || UO->isPostfix())
            return std::tuple{IsInArraySubscriptBase, AK_Store};
          else if (UO->getOpcode() == UnaryOperatorKind::UO_AddrOf)
            return std::tuple{IsInArraySubscriptBase, AK_AddrOf};
        if (!isa<CastExpr>(P))
          return std::tuple{IsInArraySubscriptBase, AK_Other};
      }
      Prev = P;
    }
    return std::tuple{IsInArraySubscriptBase, AK_Other};
  }

  SmallVector<Stmt *, 8> mParents;
  bool mHasChildArraySubscript{false};
};
}

void ClangExprMatcherPass::print(raw_ostream &OS, const llvm::Module *M) const {
  if (mMatcher.empty() || !mTfmCtx || !mTfmCtx->hasInstance())
    return;
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &SrcMgr = mTfmCtx->getRewriter().getSourceMgr();
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
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  mTfmCtx = TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                          TfmInfo->getContext(*CU))
                    : nullptr;
  if (!mTfmCtx || !mTfmCtx->hasInstance())
    return false;
  auto &SrcMgr = mTfmCtx->getRewriter().getSourceMgr();
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
      LLVM_DEBUG(dbgs() << "[EXPR MATCHER]: remember instruction ";
                 I.print(dbgs()); dbgs() << " at "; Loc.print(dbgs());
                 dbgs() << "\n");
      auto Itr{ LocToExpr.try_emplace(Loc).first };
      Itr->second.push_back(&I);
    }
  }
  for (auto &Pair : LocToExpr)
    std::reverse(Pair.second.begin(), Pair.second.end());
  // It is necessary to build LocToExpr map also if FuncDecl is null,
  // because a number of unmatched expressions should be calculated.
  auto FuncDecl = mTfmCtx->getDeclForMangledName(F.getName());
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
  "High and Low Expression Matcher (Clang)", false , true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(ClangExprMatcherPass, "clang-expr-matcher",
  "High and Low Level Expression Matcher (Clang)", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

FunctionPass * llvm::createClangExprMatcherPass() {
  return new ClangExprMatcherPass;
}
