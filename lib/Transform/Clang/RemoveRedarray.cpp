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
      SmallVectorImpl<std::tuple<std::string, clang::Stmt *>> &Ls,
      std::vector<std::tuple<std::string, clang::DeclRefExpr*>> &decls,
      std::vector<std::tuple<std::string, clang::IntegerLiteral*>> &sizes)
      : mLiterals(Ls), mDecls(decls), mSizes(sizes) {}
  bool VisitStringLiteral(clang::StringLiteral *SL) {
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "visit string literal RedarrayClauseVisitor, string: " << SL->getString() << "\n");
    if (SL->getString() != getName(ClauseId::RemoveRedarray))
      mLiterals.emplace_back(SL->getString(), mClause);
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr* DE) {
    LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "visit decl ref expr RedarrayClauseVisitor, found: " << DE->getNameInfo().getAsString() << "\n");
    mLiterals.emplace_back(DE->getNameInfo().getAsString(), DE);
    mDecls.emplace_back(DE->getNameInfo().getAsString(), DE);
    return true;
  }
  bool VisitIntegerLiteral(clang::IntegerLiteral* DE) {
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "visit IntegerLiteral RedarrayClauseVisitor\n");
      mLiterals.emplace_back("", DE);
      mSizes.emplace_back("", DE);
      return true;
  }

  void setClause(clang::Stmt *C) noexcept { mClause = C; }

private:
  SmallVectorImpl<std::tuple<std::string, clang::Stmt *>> &mLiterals;
  std::vector<std::tuple<std::string, clang::DeclRefExpr*>> &mDecls;
  std::vector<std::tuple<std::string, clang::IntegerLiteral*>> &mSizes;
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

  std::string addSuffix(std::string prefix) {
      int i = 0;
      while (mRawInfo.Identifiers.count(prefix + std::to_string(i)) > 0) { i++; }
      return prefix + std::to_string(i);
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
      RedarrayClauseVisitor SCV{mSwaps, mDecls, mSizes};
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
      if (mDecls.size() != mSizes.size()) {
          LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "number of arrays does not match number of their sizes\n");
          return false;
      }
      std::string variableDeclaration = "";
      for (int i = 0; i < mDecls.size(); i++) {
          auto [ArrName, ArrayStmt] = mDecls[i];
          auto [SizeLiteral, SizeStmt] = mSizes[i];
          const clang::Type* ArrayType = cast<clang::DeclRefExpr>(ArrayStmt)->getType().getTypePtr();
          if (!isa<clang::ArrayType>(ArrayType) && !isa<clang::PointerType>(ArrayType)) {
              LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "statement in the pragma is not array or pointer\n");
              return false;
          }
          std::string ToInsert;
          if (isa<clang::ArrayType>(ArrayType)) {
              ToInsert = cast<clang::ArrayType>(ArrayType)->getElementType().getAsString();
          }
          else if (isa<clang::PointerType>(ArrayType)) {
              ToInsert = cast<clang::PointerType>(ArrayType)->getPointeeType().getAsString();
          }
          arrayVarNames[ArrName] = std::vector<std::string>();
          for (int i = 0; i < cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue(); i++) {
              if (i > 0) {
                  ToInsert += ",";
              }
              std::string newVarName = addSuffix(ArrName + "_subscr_" + std::to_string(i) + "_");
              arrayVarNames[ArrName].push_back(newVarName);
              ToInsert += (" " + newVarName + " = " + ArrName + "[" + std::to_string(i) + "]");
          }
          ToInsert += (";\n");
          variableDeclaration += ToInsert;
      }
      mStatus = FIND_INDEX;
      auto Res = RecursiveASTVisitor::TraverseStmt(S);
      mStatus = FIND_OP;
      Res = RecursiveASTVisitor::TraverseStmt(S);
      if (checksPassed) {
          mRewriter.InsertTextBefore(S->getBeginLoc(),
              variableDeclaration); // insert array variables
                                    // definitions here
          for (int i = 0; i < rangeToReplace.size(); i++) {
              mRewriter.ReplaceText(rangeToReplace[i], textToReplace[i]);
          }
          for (int i = 0; i < mDecls.size(); i++) {
              auto [ArrName, ArrayStmt] = mDecls[i];
              auto [SizeLiteral, SizeStmt] = mSizes[i];
              std::string ToInsert = "";
              for (int i = 0; i < cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue(); i++) {
                  ToInsert += ArrName + "[" + std::to_string(i) + "] = " + arrayVarNames[ArrName][i] + ";\n";
              }
              mRewriter.InsertTextAfterToken(S->getEndLoc(), ToInsert);
          }
      }
      else {
          return false;
      }
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
      } else if (isa<clang::UnaryOperator>(S)) {
        auto Res = RecursiveASTVisitor::TraverseUnaryOperator(cast<clang::UnaryOperator>(S));
      }
      else {
          LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "unsupported operator\n");
          return false;
      }
      mStatus = FIND_OP;
      if (mArraySubscriptExpr.size() == 0) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      int declIndex = -1;
      for (int i = 0; i < mDecls.size(); i++) {
          auto [SizeLiteral, SizeStmt] = mSizes[i];
          auto [ArrName, ArrayStmt] = mDecls[i];
          if (mRewriter.getRewrittenText(mArraySubscriptExpr[0]->getLHS()->getSourceRange()) == ArrName) {
              declIndex = i;
          }
      }
      if (declIndex == -1) {
          return RecursiveASTVisitor::TraverseStmt(S);
      }
      for (int j = 1; j < mArraySubscriptExpr.size(); j++) {
          if (mRewriter.getRewrittenText(mArraySubscriptExpr[j]->getLHS()->getSourceRange()) !=
              mRewriter.getRewrittenText(mArraySubscriptExpr[0]->getLHS()->getSourceRange())) {
              bool arrayJInPragma = false;
              bool array0InPragma = false;
              for (int i = 0; i < mDecls.size(); i++) {
                  auto [ArrName, ArrayStmt] = mDecls[i];
                  if (ArrName == mRewriter.getRewrittenText(mArraySubscriptExpr[0]->getLHS()->getSourceRange())) {
                      array0InPragma = true;
                  }
                  if (ArrName == mRewriter.getRewrittenText(mArraySubscriptExpr[j]->getLHS()->getSourceRange())) {
                      arrayJInPragma = true;
                  }
              }
              if (arrayJInPragma && array0InPragma) {
                  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "different arrays in operator are not yet supported\n");
                  checksPassed = false;
                  return false;
              }
          }
      }
      auto [ArrName, ArrayStmt] = mDecls[declIndex];
      auto [SizeLiteral, SizeStmt] = mSizes[declIndex];
      std::string switchText = "switch (" + mRewriter.getRewrittenText(mArraySubscriptExpr[0]->getRHS()->getSourceRange()) + ") {\n";
      for (int i = 0; i < cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue(); i++) {
        ExternalRewriter Canvas(clang::SourceRange(S->getBeginLoc(), S->getEndLoc()), mSrcMgr, mLangOpts);
        auto ArrSize = cast<clang::IntegerLiteral>(SizeStmt)->getValue().getSExtValue();
        for (auto Subscr: mArraySubscriptExpr) {
          Canvas.ReplaceText(clang::SourceRange(Subscr->getBeginLoc(), Subscr->getEndLoc()), arrayVarNames[ArrName][i]);
        }
        std::string caseBody = Canvas.getRewrittenText(clang::SourceRange(S->getBeginLoc(), S->getEndLoc())).str();
        switchText += "case " + std::to_string(i) + ":\n" + caseBody + ";\nbreak;\n";
      }
      switchText += "}";
      bool sourceRangeInArray = false;
      for (auto range : rangeToReplace) {
          if (range.fullyContains(S->getSourceRange())) {
              sourceRangeInArray = true;
          }
      }
      if (!sourceRangeInArray) {
          rangeToReplace.push_back(S->getSourceRange());
          textToReplace.push_back(switchText);
      }
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
      for (int i = 0; i < mDecls.size(); i++) {
          auto [ArrName, ArrayStmt] = mDecls[i];
          if (ArrName == Arr->getNameInfo().getName().getAsString()) {
              mIsSubscriptUseful = true;
              return true;
          }
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
    mDecls.clear();
    mSizes.clear();
    mInductions.clear();
    arrayVarNames.clear();
    rangeToReplace.clear();
    textToReplace.clear();
    checksPassed = true;
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
  std::vector<std::tuple<std::string, clang::DeclRefExpr*>> mDecls;
  std::vector<std::tuple<std::string, clang::IntegerLiteral*>> mSizes;
  std::map<std::string, std::vector<std::string> > arrayVarNames;
  std::vector<clang::ArraySubscriptExpr *> mArraySubscriptExpr;
  std::vector<clang::SourceRange> rangeToReplace;
  std::vector<std::string> textToReplace;
  bool mIsSubscriptUseful;
  bool checksPassed = true;
  clang::VarDecl* mIndex;

  LoopNest mInductions;
};
} // namespace

bool ClangRemoveRedarray::runOnFunction(Function &F) {

  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError("can not transform sources"
                              ": transformation context is not available");
    return false;
  }
  auto *FD{TfmCtx->getDeclForMangledName(F.getName())};
  if (!FD)
    return false;
  ASTImportInfo ImportStub;
  const auto *ImportInfo{&ImportStub};
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  ClangRemoveRedarrayVisitor(*this, F, TfmCtx, *ImportInfo).TraverseDecl(FD);
  return false;
}
