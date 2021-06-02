//===--- LoopReversal.cpp - Loop Reversal (Clang) ---------*- C++ -*-===//
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
// This file implements a pass to perform loop reversal in C programs.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include <bcl/utility.h>
#include <llvm/ADT/SmallBitVector.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <string>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-loop-reverse"
#define DEBUG_PREFIX "[LOOP REVERSAL]: "

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

namespace {
class ClangLoopReverse : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  ClangLoopReverse() : FunctionPass(ID) {
    initializeClangLoopReversePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class ClangLoopReversalInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
}

void ClangLoopReversalInfo::addBeforePass(legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(createDIMemoryAnalysisServer());
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createAnalysisWaitServerPass());
}

void ClangLoopReversalInfo::addAfterPass(legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}

void ClangLoopReverse::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.addRequired<ClangPerfectLoopPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

char ClangLoopReverse::ID = '0';
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopReverse, "clang-loop-reverse",
                               "Loop Reversal (Clang)", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangLoopReversalInfo)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(ClangLoopReverse, "clang-loop-reverse",
                             "Loop Reversal (Clang)", false, false,
                             TransformationQueryManager::getPassRegistry())

namespace {
class ClauseVisitor : public clang::RecursiveASTVisitor<ClauseVisitor> {
public:
  ClauseVisitor(SmallVectorImpl<clang::StringLiteral *> &SLs,
                SmallVectorImpl<clang::IntegerLiteral *> &ILs)
      : mSLs(SLs), mILs(ILs) {}

  bool VisitStringLiteral(clang::StringLiteral *SL) {
    mSLs.push_back(SL);
    return true;
  }

  bool VisitIntegerLiteral(clang::IntegerLiteral *IL) {
    mILs.push_back(IL);
    return true;
  }

private:
  SmallVectorImpl<clang::StringLiteral *> &mSLs;
  SmallVectorImpl<clang::IntegerLiteral *> &mILs;
};

class MiniVisitor : public clang::RecursiveASTVisitor<MiniVisitor> {
public:
  MiniVisitor() : mDRE(NULL) {}
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
    if (!DRE)
      return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
    mDRE = DRE;
    return true;
  }
  clang::DeclRefExpr *getDRE() { return mDRE; }

private:
  clang::DeclRefExpr *mDRE;
};

class ClangLoopReverseVisitor
    : public clang::RecursiveASTVisitor<ClangLoopReverseVisitor> {
public:
  ClangLoopReverseVisitor(ClangLoopReverse &P, Function &F,
                          TransformationContext *TfmCtx,
                          const ASTImportInfo &ImportInfo)
      : mImportInfo(ImportInfo), mRewriter(TfmCtx->getRewriter()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()),
        mGlobalOpts(
            P.getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()),
        mRawInfo(P.getAnalysis<ClangGlobalInfoPass>().getRawInfo()),
        mMemMatcher(P.getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher),
        mDIMemMatcher(P.getAnalysis<ClangDIMemoryMatcherPass>().getMatcher()),
        mPerfectLoopInfo(
            P.getAnalysis<ClangPerfectLoopPass>().getPerfectLoopInfo()),
        mCanonicalLoopInfo(
            P.getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo()),
        mDIMInfo(P.getAnalysis<DIEstimateMemoryPass>().getAliasTree(), P, F) {}

  bool TraverseForStmt(clang::ForStmt *FS) {
    if (mStatus != TRAVERSE_LOOPS)
      return RecursiveASTVisitor::VisitForStmt(FS);
    auto LI{find_if(mCanonicalLoopInfo,
                    [FS](auto *Info) { return Info->getASTLoop() == FS; })};
    if (LI == mCanonicalLoopInfo.end())
      return RecursiveASTVisitor::VisitForStmt(FS);
    auto *Induct{mMemMatcher.find<IR>((*LI)->getInduction())->get<AST>()};
    mLoops.emplace_back(*LI, Induct);
    if (!mPerfectLoopInfo.count((*LI)->getLoop()))
      return true;
    return RecursiveASTVisitor::TraverseForStmt(FS);
  }

  bool TraverseDecl(clang::Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             tsar::diag::err_reverse_not_forstmt);
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case Status::SEARCH_PRAGMA: {
      Pragma P{*S};
      llvm::SmallVector<clang::Stmt *, 2> Clauses;
      if (!findClause(P, ClauseId::LoopReverse, Clauses))
        return RecursiveASTVisitor::TraverseStmt(S);
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "found '"
                        << getName(ClauseId::LoopReverse) << "' clause\n");
      ClauseVisitor CV{mStringLiterals, mIntegerLiterals};
      for (auto *C : Clauses)
        for (auto *S: Pragma::clause(&C))
          CV.TraverseStmt(S);
      mIsStrict = !findClause(P, ClauseId::NoStrict, Clauses);
      LLVM_DEBUG(if (!mIsStrict) dbgs()
                 << DEBUG_PREFIX << "found '" << getName(ClauseId::NoStrict)
                 << "' clause\n");
      llvm::SmallVector<clang::CharSourceRange, 2> ToRemove;
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
      /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty
      /// is set to true then removing (in RewriterBuffer) works
      /// incorrect.
      RemoveEmptyLine.RemoveLineIfEmpty = false;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine);
      mStatus = Status::TRAVERSE_STMT;
      return true;
    }
    case Status::TRAVERSE_STMT: {
      if (!isa<clang::ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::err_reverse_not_forstmt);
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
      mStatus = TRAVERSE_LOOPS;
      if (!TraverseForStmt(cast<clang::ForStmt>(S)))
        return false;
      llvm::SmallBitVector ToTransform;
      for (auto *Literal : mStringLiterals) {
        auto LoopItr{find_if(mLoops, [Literal](auto &Info) {
          return std::get<clang::VarDecl *>(Info)->getName() ==
                 Literal->getString();
        })};
        if (LoopItr == mLoops.end()) {
          toDiag(mSrcMgr.getDiagnostics(), Literal->getBeginLoc(),
                 tsar::diag::warn_reverse_cant_match);
        } else {
          ToTransform.resize(std::distance(mLoops.begin(), LoopItr), false);
          ToTransform.push_back(true);
        }
      }
      for (auto *Literal : mIntegerLiterals) {
        auto LoopIdx{Literal->getValue().getZExtValue() - 1};
        if (LoopIdx < mLoops.size()) {
          ToTransform.resize(LoopIdx);
          ToTransform.push_back(true);
          LLVM_DEBUG(dbgs()
                     << DEBUG_PREFIX << "match " << LoopIdx + 1 << " as "
                     << std::get<clang::VarDecl *>(mLoops[LoopIdx])->getName()
                     << "\n");
        } else {
          toDiag(mSrcMgr.getDiagnostics(), Literal->getBeginLoc(),
                 tsar::diag::warn_reverse_cant_match);
        }
      }
      for (auto Num : ToTransform.set_bits()) {
        auto *CanonicalInfo{std::get<const CanonicalLoopInfo *>(mLoops[Num])};
        assert(CanonicalInfo && "CanonicalLoopInfo must not be null!");
        if (!CanonicalInfo->isCanonical()) {
          toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::err_reverse_not_canonical);
          continue;
        }
        Optional<APSInt> Start, End, Step;
        if (mIsStrict) {
          auto *Loop{CanonicalInfo->getLoop()->getLoop()};
          if (!mDIMInfo.isValid()) {
            toDiag(mSrcMgr.getDiagnostics(),
                   CanonicalInfo->getASTLoop()->getBeginLoc(),
                   tsar::diag::err_reverse_no_analysis);
            continue;
          }
          auto *DIDepSet{mDIMInfo.findFromClient(*Loop)};
          if (!DIDepSet) {
            toDiag(mSrcMgr.getDiagnostics(),
                   CanonicalInfo->getASTLoop()->getBeginLoc(),
                   tsar::diag::err_reverse_no_analysis);
            continue;
          }
          DenseSet<const DIAliasNode *> Coverage;
          accessCoverage<bcl::SimpleInserter>(
              *DIDepSet, *mDIMInfo.DIAT, Coverage,
              mGlobalOpts.IgnoreRedundantMemory);
          if (!Coverage.empty()) {
            bool HasDependency{false};
            for (auto &T : *DIDepSet) {
              if (!Coverage.count(T.getNode()) || hasNoDep(T) ||
                  T.is_any<trait::Private, trait::Reduction>())
                continue;
              if (T.is<trait::Induction>() && T.size() == 1)
                if (auto DIEM{dyn_cast<DIEstimateMemory>(
                        (*T.begin())->getMemory())};
                    DIEM && DIEM->getExpression()->getNumElements() == 0) {
                  auto *ClientDIM{mDIMInfo.getClientMemory(
                      const_cast<DIEstimateMemory *>(DIEM))};
                  assert(ClientDIM && "Origin memory must exist!");
                  auto VarToDI{mDIMemMatcher.find<MD>(
                      cast<DIEstimateMemory>(ClientDIM)->getVariable())};
                  if (VarToDI->template get<AST>() ==
                      std::get<clang::VarDecl *>(mLoops[Num])) {
                    if (auto *Info{(*T.begin())->get<trait::Induction>()}) {
                      Start = Info->getStart();
                      Step = Info->getStep();
                      End = Info->getEnd();
                    }
                    continue;
                  }
                }
              HasDependency = true;
              break;
            }
            if (HasDependency) {
              toDiag(mSrcMgr.getDiagnostics(),
                     CanonicalInfo->getASTLoop()->getBeginLoc(),
                     tsar::diag::err_reverse_dependency);
              continue;
            }
          }
        }
        if (transformLoop(*CanonicalInfo,
                          *std::get<clang::VarDecl *>(mLoops[Num]), Start, Step,
                          End)) {
          LLVM_DEBUG(dbgs()
                     << DEBUG_PREFIX << " transformed "
                     << std::get<clang::VarDecl *>(mLoops[Num])->getName()
                     << "\n");
        } else
          LLVM_DEBUG(dbgs()
                     << DEBUG_PREFIX << " not transformed "
                     << std::get<clang::VarDecl *>(mLoops[Num])->getName()
                     << "\n");
      }
      resetVisitor();
      return true;
    }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  void resetVisitor() {
    mStatus = SEARCH_PRAGMA;
    mIsStrict = true;
    mStringLiterals.clear();
    mIntegerLiterals.clear();
    mLoops.clear();
  }

private:
  struct IncrementInfo {
    enum : uint8_t {
      Default = 0,
      Negative = 1u << 0,
      Unary = 1u << 1,
      Swap = 1u << 2,
      LLVM_MARK_AS_BITMASK_ENUM(Swap)
    } Flags{Default};
    StringRef NewOp;
    clang::SourceRange OpRange;
    clang::SourceRange IncrRange;
    clang::SourceRange VarRange;
  };

  struct ConditionInfo {
    enum : uint8_t {
      Default = 0,
      Exclude = 1u << 0,
      LLVM_MARK_AS_BITMASK_ENUM(Exclude)
    } Flags{Default};
    StringRef NewOp;
    clang::SourceRange OpRange;
    clang::SourceRange BoundRange;
    clang::SourceRange VarRange;
  };

  bool transformLoop(const CanonicalLoopInfo &CanonicalInfo,
                     const clang::VarDecl &Induction, Optional<APSInt> &Start,
                     Optional<APSInt> &Step, Optional<APSInt> &End) {
    IncrementInfo IE;
    if (auto *BO{dyn_cast<clang::BinaryOperator>(
            CanonicalInfo.getASTLoop()->getInc())}) {
      switch (BO->getOpcode()) {
      default:
        return false;
      case clang::BinaryOperator::Opcode::BO_AddAssign:
        IE.IncrRange = BO->getRHS()->getSourceRange();
        IE.NewOp = "-=";
        IE.Flags |= IncrementInfo::Negative;
        IE.OpRange = clang::SourceRange{
            BO->getOperatorLoc(), BO->getOperatorLoc().getLocWithOffset(1)};
        break;
      case clang::BinaryOperator::Opcode::BO_SubAssign:
        IE.IncrRange = BO->getRHS()->getSourceRange();
        IE.NewOp = "+=";
        IE.OpRange = clang::SourceRange{
            BO->getOperatorLoc(), BO->getOperatorLoc().getLocWithOffset(1)};
        break;
      case clang::BinaryOperator::Opcode::BO_Assign:
        if (auto *Op{dyn_cast<clang::BinaryOperator>(BO->getRHS())})
          switch (Op->getOpcode()) {
          default:
            return false;
          case clang::BinaryOperator::Opcode::BO_Add: {
            auto *LHS{Op->getLHS()}, *RHS{Op->getRHS()};
            IE.VarRange = LHS->getSourceRange();
            IE.IncrRange = RHS->getSourceRange();
            MiniVisitor MV;
            MV.TraverseStmt(RHS);
            if (auto *DRE{MV.getDRE()}) {
              if (DRE->getDecl() == &Induction) {
                IE.Flags |= IncrementInfo::Swap;
                IE.VarRange = RHS->getSourceRange();
                IE.IncrRange = LHS->getSourceRange();
              }
            }
            IE.Flags |= IncrementInfo::Negative;
            IE.NewOp = "-";
            IE.OpRange = clang::SourceRange{Op->getOperatorLoc()};
            break;
          }
          case clang::BinaryOperator::Opcode::BO_Sub:
            IE.IncrRange = Op->getRHS()->getSourceRange();
            IE.OpRange = clang::SourceRange{Op->getOperatorLoc()};
            IE.NewOp = "+";
            break;
          }
        else
          return false;
        break;
      }
    } else if (auto *UO{dyn_cast<clang::UnaryOperator>(
                   CanonicalInfo.getASTLoop()->getInc())}) {
      IE.Flags |= IncrementInfo::Unary;
      switch (UO->getOpcode()) {
      default:
        return false;
      case clang::UnaryOperator::Opcode::UO_PostInc:
      case clang::UnaryOperator::Opcode::UO_PreInc:
        IE.NewOp = "--";
        IE.OpRange = clang::SourceRange(
            UO->getOperatorLoc(), UO->getOperatorLoc().getLocWithOffset(1));
        IE.Flags |= IncrementInfo::Negative;
        break;
      case clang::UnaryOperator::Opcode::UO_PostDec:
      case clang::UnaryOperator::Opcode::UO_PreDec:
        IE.NewOp = "++";
        IE.OpRange = clang::SourceRange(
            UO->getOperatorLoc(), UO->getOperatorLoc().getLocWithOffset(1));
        break;
      }
    } else {
      return false;
    }
    ConditionInfo CE;
    if (auto *BO{dyn_cast<clang::BinaryOperator>(
            CanonicalInfo.getASTLoop()->getCond())}) {
      auto *LHS{ BO->getLHS() }, *RHS{ BO->getRHS() };
      CE.BoundRange = RHS->getSourceRange();
      CE.VarRange = LHS->getSourceRange();
      MiniVisitor MV;
      MV.TraverseStmt(RHS);
      bool Flag = false;
      if (auto *DRE{MV.getDRE()}; DRE && DRE->getDecl() == &Induction)
        std::swap(CE.BoundRange, CE.VarRange);
      switch (BO->getOpcode()) {
      default:
        return false;
      case clang::BinaryOperator::Opcode::BO_LE:
        CE.NewOp = ">=";
        CE.OpRange = clang::SourceRange{
            BO->getOperatorLoc(), BO->getOperatorLoc().getLocWithOffset(1)};
        break;
      case clang::BinaryOperator::Opcode::BO_LT:
        CE.NewOp = ">=";
        CE.OpRange = clang::SourceRange{BO->getOperatorLoc()};
        CE.Flags |= ConditionInfo::Exclude;
        break;
      case clang::BinaryOperator::Opcode::BO_GE:
        CE.NewOp = "<=";
        CE.OpRange = clang::SourceRange{
            BO->getOperatorLoc(), BO->getOperatorLoc().getLocWithOffset(1)};
        break;
      case clang::BinaryOperator::Opcode::BO_GT:
        CE.NewOp = "<=";
        CE.OpRange = clang::SourceRange{BO->getOperatorLoc()};
        CE.Flags |= ConditionInfo::Exclude;
        break;
      }
    } else {
      return false;
    }
    clang::SourceRange InitRange;
    if (auto *BO{dyn_cast<clang::BinaryOperator>(
            CanonicalInfo.getASTLoop()->getInit())})
      InitRange = BO->getRHS()->getSourceRange();
    else if (auto *DS{dyn_cast<clang::DeclStmt>(
                 CanonicalInfo.getASTLoop()->getInit())})
      InitRange = DS->child_begin()->getSourceRange();
    else
      return false;
    auto InitStr{mRewriter.getRewrittenText(InitRange)};
    if (Start)
      InitStr = std::to_string(Start->getSExtValue()); // TODO check sign
    auto CondStr{mRewriter.getRewrittenText(CE.BoundRange)};
    std::string NewInitStr;
    if (End && Step) { // TODO check sign
      NewInitStr = std::to_string(End->getSExtValue() - Step->getSExtValue());
    } else if (IE.Flags & IncrementInfo::Unary) {
      if (CE.Flags & ConditionInfo::Exclude)
        if (IE.Flags & IncrementInfo::Negative)
          NewInitStr = "((" + CondStr + ")-1)";
        else
          NewInitStr = "((" + CondStr + ")+1)";
      else
        NewInitStr = "(" + CondStr + ")";
    } else {
      auto IncrStr{mRewriter.getRewrittenText(IE.IncrRange)};
      if (CE.Flags & ConditionInfo::Exclude) {
        if (IE.Flags & IncrementInfo::Negative)
          NewInitStr = "((" + InitStr + ")+(((" + CondStr + ")-1)-(" + InitStr +
                       "))/(" + IncrStr + ")*(" + IncrStr + "))";
        else
          NewInitStr = "((" + InitStr + ")-((" + InitStr + ")-((" + CondStr +
                       ")+1))/(" + IncrStr + ")*(" + IncrStr + "))";

      } else {
        if (IE.Flags & IncrementInfo::Negative)
          NewInitStr = "((" + InitStr + ")+((" + CondStr + ")-(" + InitStr +
                       "))/(" + IncrStr + ")*(" + IncrStr + "))";
        else
          NewInitStr = "((" + InitStr + ")-((" + InitStr + ")-(" + CondStr +
                       "))/(" + IncrStr + ")*(" + IncrStr + "))";
      }
    }
    if (IE.Flags & IncrementInfo::Swap) {
      auto VarStr{mRewriter.getRewrittenText(IE.VarRange)};
      auto IncrStr{mRewriter.getRewrittenText(IE.IncrRange)};
      mRewriter.ReplaceText(IE.VarRange, IncrStr);
      mRewriter.ReplaceText(IE.IncrRange, VarStr);
    }
    mRewriter.ReplaceText(IE.OpRange, IE.NewOp);
    mRewriter.ReplaceText(CE.OpRange, CE.NewOp);
    mRewriter.ReplaceText(InitRange, NewInitStr);
    mRewriter.ReplaceText(CE.BoundRange, InitStr);
    return true;
  }

  const ASTImportInfo &mImportInfo;
  clang::Rewriter &mRewriter;
  clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;
  ClangGlobalInfoPass::RawInfo &mRawInfo;
  const GlobalOptions &mGlobalOpts;
  MemoryMatchInfo::MemoryMatcher &mMemMatcher;
  const ClangDIMemoryMatcherPass::DIMemoryMatcher &mDIMemMatcher;
  PerfectLoopInfo &mPerfectLoopInfo;
  const CanonicalLoopSet &mCanonicalLoopInfo;
  DIMemoryClientServerInfo mDIMInfo;
  bool mIsStrict{true};
  enum Status {
    SEARCH_PRAGMA,
    TRAVERSE_STMT,
    TRAVERSE_LOOPS
  } mStatus{SEARCH_PRAGMA};
  SmallVector<clang::StringLiteral *, 1> mStringLiterals;
  SmallVector<clang::IntegerLiteral *, 1> mIntegerLiterals;
  SmallVector<std::tuple<const tsar::CanonicalLoopInfo *, clang::VarDecl *>, 4>
      mLoops;
};
} // namespace

bool ClangLoopReverse::runOnFunction(Function &F) {
  releaseMemory();
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
  ClangLoopReverseVisitor{*this, F, TfmCtx, *ImportInfo}.TraverseDecl(FD);
  return false;
}
