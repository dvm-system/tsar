//===--- LoopInterchange.cpp - Loop Interchagne (Clang) ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements a pass to perform remove redarray in C programs.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <algorithm>
#include <bcl/utility.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

#define LLVM_DEBUG(X) X

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-remove-redarray"
#define DEBUG_PREFIX "[REMOVE REDARRAY]: "


namespace {
class ClangRemoveRedarray : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangRemoveRedarray() : FunctionPass(ID) {
    initializeClangRemoveRedarrayPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class ClangRemoveRedarrayInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
} // namespace

void ClangRemoveRedarrayInfo::addBeforePass(
    legacy::PassManager &Passes) const {
	dbgs() << DEBUG_PREFIX << "launch before_pass remove_redarray\n";
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  Passes.add(createGlobalsAccessStorage());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(createDIMemoryAnalysisServer());
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createAnalysisWaitServerPass());
}

void ClangRemoveRedarrayInfo::addAfterPass(legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}

void ClangRemoveRedarray::getAnalysisUsage(AnalysisUsage &AU) const {
	dbgs() << DEBUG_PREFIX << "launch getAnalysisUsage remove_redarray\n";
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.addRequired<ClangPerfectLoopPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  //AU.setPreservesAll();
  dbgs() << DEBUG_PREFIX << "end getAnalysisUsage remove_redarray\n";
}

char ClangRemoveRedarray::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangRemoveRedarray, "clang-remove-redarray",
                               "Remove Redarray (Clang)", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangRemoveRedarrayInfo)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(ClangRemoveRedarray, "clang-remove-redarray",
                             "Remove Redarray (Clang)", false, false,
                             TransformationQueryManager::getPassRegistry())

namespace {
class RedarrayClauseVisitor
    : public clang::RecursiveASTVisitor<RedarrayClauseVisitor> {
public:
  explicit RedarrayClauseVisitor(
      SmallVectorImpl<std::tuple<std::string, clang::Stmt *>> &Ls)
      : mLiterals(Ls) {}
  bool VisitStringLiteral(clang::StringLiteral *SL) {
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "visit string literal RedarrayClauseVisitor, string: " << SL->getString() << "\n");
    if (SL->getString() != getName(ClauseId::RemoveRedarray))
      mLiterals.emplace_back(SL->getString(), mClause);
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr* DE) {
    LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "visit decl ref expr RedarrayClauseVisitor, found: " << DE->getNameInfo().getAsString() << "\n");
    mLiterals.emplace_back(DE->getNameInfo().getAsString(), DE);
    return true;
  }
  bool VisitIntegerLiteral(clang::IntegerLiteral* DE) {
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "visit IntegerLiteral RedarrayClauseVisitor\n");
      mLiterals.emplace_back("", DE);
      return true;
  }

  void setClause(clang::Stmt *C) noexcept { mClause = C; }

private:
  SmallVectorImpl<std::tuple<std::string, clang::Stmt *>> &mLiterals;
  clang::Stmt *mClause{nullptr};
};

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class ClangRemoveRedarrayVisitor
    : public clang::RecursiveASTVisitor<ClangRemoveRedarrayVisitor> {
  enum LoopKind : uint8_t {
    Ok = 0,
    NotCanonical = 1u << 0,
    NotPerfect = 1u << 1,
    NotAnalyzed = 1u << 2,
    HasDependency = 1u << 3,
    LLVM_MARK_AS_BITMASK_ENUM(HasDependency)
  };

  using VarList = SmallVector<clang::VarDecl *, 4>;

  using LoopNest = SmallVector<
      std::tuple<clang::VarDecl *, const DIMemory *, clang::ForStmt *, LoopKind,
                 VarList, const CanonicalLoopInfo *>,
      4>;

public:
  ClangRemoveRedarrayVisitor(ClangRemoveRedarray &P, llvm::Function &F,
                              ClangTransformationContext *TfmCtx,
                              const ASTImportInfo &ImportInfo)
      : mImportInfo(ImportInfo), mRewriter(TfmCtx->getRewriter()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()),
        mRawInfo(P.getAnalysis<ClangGlobalInfoPass>().getGlobalInfo(TfmCtx)->RI),
        mGlobalOpts(
            P.getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()),
        mMemMatcher(P.getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher),
        mDIMemMatcher(P.getAnalysis<ClangDIMemoryMatcherPass>().getMatcher()),
        mPerfectLoopInfo(
            P.getAnalysis<ClangPerfectLoopPass>().getPerfectLoopInfo()),
        mCanonicalLoopInfo(
            P.getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo()),
        mAT(P.getAnalysis<EstimateMemoryPass>().getAliasTree()),
        mTLI(P.getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F)),
        mDT(P.getAnalysis<DominatorTreeWrapperPass>().getDomTree()),
        mDIMInfo(P.getAnalysis<DIEstimateMemoryPass>().getAliasTree(), P, F)
        {
    if (mDIMInfo.isValid())
      mSTR = SpanningTreeRelation<const DIAliasTree *>{mDIMInfo.DIAT};
  }


  bool TraverseVarDecl(clang::VarDecl *VD) {
    if (mStatus == FIND_INDEX) {
      mIndex = VD;
    }
    return RecursiveASTVisitor::TraverseVarDecl(VD);
  }

  bool TraverseDecl(clang::Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             tsar::diag::warn_interchange_not_for_loop); // TODO: change this warning
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }

  bool TraverseBinaryOperator(clang::BinaryOperator * B) {
    if (mStatus != GET_ALL_ARRAY_SUBSCRIPTS) {
      return RecursiveASTVisitor::TraverseBinaryOperator(B);
    }
    return RecursiveASTVisitor::TraverseBinaryOperator(B);
  }

  bool TraverseUnaryOperator(clang::UnaryOperator *U) {
    if (mStatus != GET_ALL_ARRAY_SUBSCRIPTS) {
      return RecursiveASTVisitor::TraverseUnaryOperator(U);
    }
    return RecursiveASTVisitor::TraverseUnaryOperator(U);
  }

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case SEARCH_PRAGMA: {
      Pragma P{*S};
      llvm::SmallVector<clang::Stmt *, 2> Clauses;
      if (!findClause(P, ClauseId::RemoveRedarray, Clauses))
        return RecursiveASTVisitor::TraverseStmt(S);
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "found remove_redarray clause\n");
      RedarrayClauseVisitor SCV{mSwaps};
      for (auto *C : Clauses) {
        SCV.setClause(C);
        SCV.TraverseStmt(C);
      }
      mIsStrict = !findClause(P, ClauseId::NoStrict, Clauses);
      LLVM_DEBUG(if (!mIsStrict) dbgs()
                  << DEBUG_PREFIX << "found 'nostrict' clause\n");
      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
      auto IsPossible{pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
                                          mImportInfo, ToRemove)};
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
                  tsar::diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
                  tsar::diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
                  tsar::diag::warn_remove_directive);
      clang::Rewriter::RewriteOptions RemoveEmptyLine;
      /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
      /// set to true then removing (in RewriterBuffer) works incorrect.
      RemoveEmptyLine.RemoveLineIfEmpty = false;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine);
      Clauses.clear();
      // TODO: insert variable declaration here
      mStatus = TRAVERSE_STMT;
      return true;
    }
    case TRAVERSE_STMT: {
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "enter traverse_stmt stage\n");
      if (!isa<clang::ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::warn_interchange_not_for_loop); // TODO: change this warn
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      bool HasMacro{false};
      for_each_macro(S, mSrcMgr, mLangOpts, mRawInfo.Macros,
                     [&HasMacro, this](clang::SourceLocation Loc) {
                       if (!HasMacro) {
                         toDiag(mSrcMgr.getDiagnostics(), Loc,
                                tsar::diag::note_assert_no_macro);
                         HasMacro = true;
                       }
                     });
      if (HasMacro) {
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      auto [ArrName, ArrayStmt] = mSwaps[0];
      auto [SizeLiteral, SizeStmt] = mSwaps[1];
      ArrName += "_subscr_";
      std::string ToInsert = cast<clang::DeclRefExpr>(ArrayStmt)->getType().getAsString();
      ToInsert.erase(ToInsert.find('['), ToInsert.find(']'));
      for (int i = 0; i < cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue(); i++) {
        if (i > 0) {
          ToInsert += ",";
        }
        ToInsert += (" " + ArrName + std::to_string(i));
      }
      ToInsert += (";\n");
      mRewriter.InsertTextBefore(S->getBeginLoc(),
                                ToInsert); // insert array variables
                                                      // definitions here
      mStatus = FIND_INDEX;
      auto Res = RecursiveASTVisitor::TraverseStmt(S);
      mStatus = FIND_OP;
      Res = RecursiveASTVisitor::TraverseStmt(S);
      return true;

    }
    case FIND_INDEX: {
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    case FIND_OP: {
      if (!isa<clang::BinaryOperator>(S) && !isa<clang::UnaryOperator>(S)) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      mStatus = GET_ALL_ARRAY_SUBSCRIPTS;
      if (isa<clang::BinaryOperator>(S)) {
        auto Res = RecursiveASTVisitor::TraverseBinaryOperator(cast<clang::BinaryOperator>(S));
      } else {
        auto Res = RecursiveASTVisitor::TraverseUnaryOperator(cast<clang::UnaryOperator>(S));
      }
      mStatus = FIND_OP;
      if (mArraySubscriptExpr.size() == 0) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      auto [SizeLiteral, SizeStmt] = mSwaps[1];
      auto IndexName = mIndex->getName();
      std::string switchText = "switch (" + IndexName.str() + ") {\n";
      for (int i = 0; i < cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue(); i++) {
        ExternalRewriter Canvas(clang::SourceRange(S->getBeginLoc(), S->getEndLoc()), mSrcMgr, mLangOpts);
        auto ArrSize = cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue();
        auto [ArrName, ArrayStmt] = mSwaps[0];
        ArrName += "_subscr_";
        for (auto Subscr: mArraySubscriptExpr) {
          Canvas.ReplaceText(clang::SourceRange(Subscr->getBeginLoc(), Subscr->getEndLoc()), ArrName + std::to_string(i));
        }
        std::string caseBody = Canvas.getRewrittenText(clang::SourceRange(S->getBeginLoc(), S->getEndLoc())).str();
        switchText += "case " + std::to_string(i) + ":\n" + caseBody + ";\nbreak;\n";
      }
      switchText += "}";
      mRewriter.ReplaceText(clang::SourceRange(S->getBeginLoc(), S->getEndLoc()), switchText);
      clearArraySubscr();
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    case GET_ALL_ARRAY_SUBSCRIPTS: {
      if (!isa<clang::ArraySubscriptExpr>(S)) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      mIsSubscriptUseful = false;
      mStatus = CHECK_SUBSCRIPT;
      auto Res = RecursiveASTVisitor::TraverseStmt(S);
      mStatus = GET_ALL_ARRAY_SUBSCRIPTS;
      if (mIsSubscriptUseful) {
        mArraySubscriptExpr.push_back(cast<clang::ArraySubscriptExpr>(S));
      }
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    case CHECK_SUBSCRIPT: {
      if (!isa<clang::DeclRefExpr>(S)) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      auto Arr = cast<clang::DeclRefExpr>(S);
      auto [ArrName, ArrayStmt] = mSwaps[0];
      if (ArrName == Arr->getNameInfo().getName().getAsString()) {
        mIsSubscriptUseful = true;
        return true;
      }
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }


private:
  void resetVisitor() {
    mStatus = SEARCH_PRAGMA;
    mSwaps.clear();
    mInductions.clear();
  }

  void clearArraySubscr() {
      mArraySubscriptExpr.clear();
  }



  const ASTImportInfo mImportInfo;
  clang::Rewriter &mRewriter;
  clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;
  ClangGlobalInfo::RawInfo &mRawInfo;
  const GlobalOptions &mGlobalOpts;
  MemoryMatchInfo::MemoryMatcher &mMemMatcher;
  const ClangDIMemoryMatcherPass::DIMemoryMatcher &mDIMemMatcher;
  const CanonicalLoopSet &mCanonicalLoopInfo;
  PerfectLoopInfo &mPerfectLoopInfo;
  AliasTree &mAT;
  TargetLibraryInfo &mTLI;
  DominatorTree &mDT;
  DIMemoryClientServerInfo mDIMInfo;
  std::optional<SpanningTreeRelation<const DIAliasTree *>> mSTR;
  bool mIsStrict{true};
  enum Status {
    SEARCH_PRAGMA,
    TRAVERSE_STMT,
    FIND_INDEX,
    FIND_OP,
    GET_ALL_ARRAY_SUBSCRIPTS,
    CHECK_SUBSCRIPT,
  } mStatus{SEARCH_PRAGMA};
  SmallVector<std::tuple<std::string, clang::Stmt *>, 4> mSwaps;
  std::vector<clang::ArraySubscriptExpr *> mArraySubscriptExpr;
  bool mIsSubscriptUseful;
  clang::VarDecl* mIndex;

  LoopNest mInductions;
};
} // namespace

bool ClangRemoveRedarray::runOnFunction(Function &F) {

	dbgs() << DEBUG_PREFIX << "launch runOnFunction remove_redarray\n";

  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  dbgs() << DEBUG_PREFIX << "DISub initialized runOnFunction remove_redarray\n";
  auto *CU{DISub->getUnit()};
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  dbgs() << DEBUG_PREFIX << "getSourceLanguage runOnFunction remove_redarray\n";
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  dbgs() << DEBUG_PREFIX << "getAnalysis runOnFunction remove_redarray\n";
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  dbgs() << DEBUG_PREFIX << "TfmInfo ClangTransformationContext runOnFunction remove_redarray\n";
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError("can not transform sources"
                              ": transformation context is not available");
    return false;
  }
  dbgs() << DEBUG_PREFIX << "TfmCtx runOnFunction remove_redarray\n";
  auto *FD{TfmCtx->getDeclForMangledName(F.getName())};
  if (!FD)
    return false;
  dbgs() << DEBUG_PREFIX << "FD runOnFunction remove_redarray\n";
  ASTImportInfo ImportStub;
  const auto *ImportInfo{&ImportStub};
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  dbgs() << DEBUG_PREFIX << "end of runOnFunction remove_redarray\n";
  ClangRemoveRedarrayVisitor(*this, F, TfmCtx, *ImportInfo).TraverseDecl(FD);
  return false;
}
