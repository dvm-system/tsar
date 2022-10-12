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
#include "tsar/Transform/IR/InterprocAttr.h"
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
  Passes.add(createGlobalsAccessStorage());
  Passes.add(createGlobalsAccessCollector());
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
  AU.addRequired<LoopAttributesDeductionPass>();
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
INITIALIZE_PASS_DEPENDENCY(LoopAttributesDeductionPass)
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

class ClangLoopReverseVisitor
    : public clang::RecursiveASTVisitor<ClangLoopReverseVisitor> {
  struct ExpressionInfo {
    StringRef NewOp;
    clang::SourceRange OpRange;
    clang::SourceRange ExprRange;

    operator bool() {
      return !NewOp.empty() && OpRange.isValid();
    }
  };

  struct IncrementInfo : public ExpressionInfo {
    enum : uint8_t {
      Default = 0,
      Sub = 1u << 0,
      Unary = 1u << 1,
      Assign = 1u << 2,
      LLVM_MARK_AS_BITMASK_ENUM(Assign)
    } Flags{Default};
  };

  struct ConditionInfo : public ExpressionInfo {
    enum : uint8_t {
      Default = 0,
      Exclude = 1u << 0,
      LLVM_MARK_AS_BITMASK_ENUM(Exclude)
    } Flags{Default};
  };

public:
  ClangLoopReverseVisitor(ClangLoopReverse &P, Function &F,
                          ClangTransformationContext *TfmCtx,
                          const ASTImportInfo &ImportInfo)
      : mImportInfo(ImportInfo), mRewriter(TfmCtx->getRewriter()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()),
        mGlobalOpts(
            P.getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()),
        mRawInfo(
            P.getAnalysis<ClangGlobalInfoPass>().getGlobalInfo(TfmCtx)->RI),
        mMemMatcher(P.getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher),
        mDIMemMatcher(P.getAnalysis<ClangDIMemoryMatcherPass>().getMatcher()),
        mPerfectLoopInfo(
            P.getAnalysis<ClangPerfectLoopPass>().getPerfectLoopInfo()),
        mCanonicalLoopInfo(
            P.getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo()),
        mDIMInfo(P.getAnalysis<DIEstimateMemoryPass>().getAliasTree(), P, F),
        mLoopAttr(P.getAnalysis<LoopAttributesDeductionPass>()) {}

  bool TraverseForStmt(clang::ForStmt *FS) {
    if (mStatus != TRAVERSE_LOOPS)
      return RecursiveASTVisitor::VisitForStmt(FS);
    auto LI{find_if(mCanonicalLoopInfo,
                    [FS](auto *Info) { return Info->getASTLoop() == FS; })};
    if (LI == mCanonicalLoopInfo.end())
      return RecursiveASTVisitor::VisitForStmt(FS);
    auto Itr{mMemMatcher.find<IR>((*LI)->getInduction())};
    auto InductItr{find_if(Itr->get<AST>(), [this](const clang::VarDecl *VD) {
      return &mSrcMgr == &VD->getASTContext().getSourceManager();
    })};
    assert(InductItr != Itr->get<AST>().end() &&
           "Matched variable must be presented in a current AST context.");
    auto *Induct{*InductItr};
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
             tsar::diag::warn_reverse_not_for_loop);
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case Status::SEARCH_PRAGMA:
      return searchPragma(S);
    case Status::TRAVERSE_STMT:
      return traverseStmt(S);
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
  bool searchPragma(clang::Stmt *S) {
    assert(S && "Statement must not be null!");
    assert(mStatus == SEARCH_PRAGMA && "Invalid visitor status!");
    Pragma P{*S};
    llvm::SmallVector<clang::Stmt *, 2> Clauses;
    if (!findClause(P, ClauseId::LoopReverse, Clauses))
      return RecursiveASTVisitor::TraverseStmt(S);
    LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "found '"
                      << getName(ClauseId::LoopReverse) << "' clause\n");
    ClauseVisitor CV{mStringLiterals, mIntegerLiterals};
    for (auto *C : Clauses)
      for (auto *S : Pragma::clause(&C))
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

  bool traverseStmt(clang::Stmt *S) {
    assert(S && "Statement must not be null!");
    assert(mStatus == TRAVERSE_STMT && "Invalid visitor status!");
    if (!isa<clang::ForStmt>(S)) {
      toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
             tsar::diag::warn_reverse_not_for_loop);
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
               tsar::diag::warn_reverse_induction_mismatch)
            << Literal->getString();
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
        LLVM_DEBUG(
            dbgs() << DEBUG_PREFIX << "match " << LoopIdx + 1 << " as "
                   << std::get<clang::VarDecl *>(mLoops[LoopIdx])->getName()
                   << "\n");
      } else {
        toDiag(mSrcMgr.getDiagnostics(), Literal->getBeginLoc(),
               tsar::diag::warn_reverse_number_mismatch)
            << static_cast<unsigned>(LoopIdx);
      }
    }
    for (auto Num : ToTransform.set_bits()) {
      auto *CanonicalInfo{std::get<const CanonicalLoopInfo *>(mLoops[Num])};
      assert(CanonicalInfo && "CanonicalLoopInfo must not be null!");
      if (!CanonicalInfo->isCanonical()) {
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::warn_reverse_not_canonical);
        continue;
      }
      auto *L{CanonicalInfo->getLoop()->getLoop()};
      Optional<APSInt> Start, Step, End;
      if (mIsStrict) {
        if (!L->getExitingBlock()) {
          toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::warn_reverse_multiple_exits);
          continue;
        }
        if (!mLoopAttr.hasAttr(*L, AttrKind::AlwaysReturn) ||
            !mLoopAttr.hasAttr(*L, AttrKind::NoIO) ||
            !mLoopAttr.hasAttr(*L, Attribute::NoUnwind) ||
            mLoopAttr.hasAttr(*L, Attribute::ReturnsTwice)) {
          toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::warn_reverse_unsafe_cfg);
          continue;
        }
        if (!checkDependencies(Num, *CanonicalInfo, Start, Step, End))
          continue;
      }
      transformLoop(*CanonicalInfo, *std::get<clang::VarDecl *>(mLoops[Num]),
                    Start, Step, End);
    }
    resetVisitor();
    return true;
  }

  bool checkDependencies(unsigned LoopIdx,
      const CanonicalLoopInfo &CanonicalInfo, Optional<APSInt> &Start,
      Optional<APSInt> &Step, Optional<APSInt> &End) {
    auto *Loop{CanonicalInfo.getLoop()->getLoop()};
    if (!mDIMInfo.isValid()) {
      toDiag(mSrcMgr.getDiagnostics(),
             CanonicalInfo.getASTLoop()->getBeginLoc(),
             tsar::diag::warn_reverse_no_analysis);
      return false;
    }
    auto *DIDepSet{mDIMInfo.findFromClient(*Loop)};
    if (!DIDepSet) {
      toDiag(mSrcMgr.getDiagnostics(),
             CanonicalInfo.getASTLoop()->getBeginLoc(),
             tsar::diag::warn_reverse_no_analysis);
      return false;
    }
    DenseSet<const DIAliasNode *> Coverage;
    accessCoverage<bcl::SimpleInserter>(*DIDepSet, *mDIMInfo.DIAT, Coverage,
                                        mGlobalOpts.IgnoreRedundantMemory);
    if (!Coverage.empty()) {
      bool HasDependency{false};
      for (auto &T : *DIDepSet) {
        if (!Coverage.count(T.getNode()) || hasNoDep(T) ||
            T.is_any<trait::Private, trait::Reduction>())
          continue;
        if (T.is<trait::Induction>() && T.size() == 1)
          if (auto DIEM{dyn_cast<DIEstimateMemory>((*T.begin())->getMemory())};
              DIEM && DIEM->getExpression()->getNumElements() == 0) {
            auto *ClientDIM{
                mDIMInfo.getClientMemory(const_cast<DIEstimateMemory *>(DIEM))};
            assert(ClientDIM && "Origin memory must exist!");
            auto VarToDI{mDIMemMatcher.find<MD>(
                cast<DIEstimateMemory>(ClientDIM)->getVariable())};
            if (VarToDI->template get<AST>() ==
                std::get<clang::VarDecl *>(mLoops[LoopIdx])) {
              if (auto *Info{(*T.begin())->get<trait::Induction>()}) {
                Start = Info->getStart();
                Step = Info->getStep();
                End = Info->getEnd();
              }
              continue;
            }
          }
        toDiag(mSrcMgr.getDiagnostics(),
               CanonicalInfo.getASTLoop()->getBeginLoc(),
               tsar::diag::warn_reverse_dependency);
        return false;
      }
    }
    return true;
  }


  bool isInductionRef(clang::Stmt *S, const clang::VarDecl *Induction) {
    if (auto *DRE{dyn_cast<clang::DeclRefExpr>(S)};
        DRE && DRE->getDecl() == Induction)
      return true;
    while (!S->children().empty()) {
      auto I(S->child_begin());
      if (++I != S->child_end())
        return false;
      S = *S->child_begin();
      if (auto *DRE{dyn_cast<clang::DeclRefExpr>(S)};
          DRE && DRE->getDecl() == Induction)
        return true;
    }
    return false;
  };

  IncrementInfo parseIncrement(const clang::Expr &Inc,
                               const clang::VarDecl &Induction) {
    IncrementInfo IE;
    if (auto *BO{dyn_cast<clang::BinaryOperator>(&Inc)}) {
      switch (BO->getOpcode()) {
      case clang::BinaryOperator::Opcode::BO_AddAssign:
        IE.ExprRange = BO->getRHS()->getSourceRange();
        IE.NewOp = "-=";
        IE.Flags |= IncrementInfo::Sub;
        IE.OpRange = clang::SourceRange{
            BO->getOperatorLoc(), BO->getOperatorLoc().getLocWithOffset(1)};
        break;
      case clang::BinaryOperator::Opcode::BO_SubAssign:
        IE.ExprRange = BO->getRHS()->getSourceRange();
        IE.NewOp = "+=";
        IE.OpRange = clang::SourceRange{
            BO->getOperatorLoc(), BO->getOperatorLoc().getLocWithOffset(1)};
        break;
      case clang::BinaryOperator::Opcode::BO_Assign:
        IE.Flags |= IncrementInfo::Assign;
        if (auto *Op{dyn_cast<clang::BinaryOperator>(BO->getRHS())})
          switch (Op->getOpcode()) {
          case clang::BinaryOperator::Opcode::BO_Add: {
            if (isInductionRef(Op->getRHS(), &Induction))
              IE.ExprRange = Op->getLHS()->getSourceRange();
            else if (isInductionRef(Op->getLHS(), &Induction))
              IE.ExprRange = Op->getRHS()->getSourceRange();
            else
              return IncrementInfo{};
            IE.Flags |= IncrementInfo::Sub;
            IE.NewOp = "-";
            IE.OpRange = clang::SourceRange{Op->getOperatorLoc()};
            break;
          }
          case clang::BinaryOperator::Opcode::BO_Sub:
            IE.ExprRange = Op->getRHS()->getSourceRange();
            IE.OpRange = clang::SourceRange{Op->getOperatorLoc()};
            IE.NewOp = "+";
            break;
          }
        break;
      }
    } else if (auto *UO{dyn_cast<clang::UnaryOperator>(&Inc)}) {
      IE.Flags |= IncrementInfo::Unary;
      switch (UO->getOpcode()) {
      case clang::UnaryOperator::Opcode::UO_PostInc:
      case clang::UnaryOperator::Opcode::UO_PreInc:
        IE.NewOp = "--";
        IE.OpRange = clang::SourceRange(
            UO->getOperatorLoc(), UO->getOperatorLoc().getLocWithOffset(1));
        IE.Flags |= IncrementInfo::Sub;
        break;
      case clang::UnaryOperator::Opcode::UO_PostDec:
      case clang::UnaryOperator::Opcode::UO_PreDec:
        IE.NewOp = "++";
        IE.OpRange = clang::SourceRange(
            UO->getOperatorLoc(), UO->getOperatorLoc().getLocWithOffset(1));
        break;
      }
    }
    return IE;
  }

  ConditionInfo parseCondition(const clang::Expr &Cond,
                               const clang::VarDecl &Induction) {
    ConditionInfo CE;
    if (auto *BO{dyn_cast<clang::BinaryOperator>(&Cond)}) {
      if (isInductionRef(BO->getRHS(), &Induction))
        CE.ExprRange = BO->getLHS()->getSourceRange();
      else if (isInductionRef(BO->getLHS(), &Induction))
        CE.ExprRange = BO->getRHS()->getSourceRange();
      else
        return ConditionInfo{};
      switch (BO->getOpcode()) {
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
    }
    return CE;
  }

  bool transformLoop(const CanonicalLoopInfo &CanonicalInfo,
      const clang::VarDecl &Induction, const Optional<APSInt> &Start,
      const Optional<APSInt> &Step, const Optional<APSInt> &End) {
    IncrementInfo IE{
        parseIncrement(*CanonicalInfo.getASTLoop()->getInc(), Induction)};
    if (!IE) {
        toDiag(mSrcMgr.getDiagnostics(),
               CanonicalInfo.getASTLoop()->getBeginLoc(),
               tsar::diag::warn_reverse_increment_complex);
      return false;
    }
    ConditionInfo CE{
        parseCondition(*CanonicalInfo.getASTLoop()->getCond(), Induction)};
    if (!CE) {
      toDiag(mSrcMgr.getDiagnostics(),
             CanonicalInfo.getASTLoop()->getBeginLoc(),
             tsar::diag::warn_reverse_condition_complex);
      return false;
    }
    clang::SourceRange InitRange;
    if (auto *BO{dyn_cast<clang::BinaryOperator>(
            CanonicalInfo.getASTLoop()->getInit())};
        BO && BO->getOpcode() == clang::BinaryOperator::Opcode::BO_Assign &&
        isInductionRef(BO->getLHS(), &Induction)) {
      InitRange = BO->getRHS()->getSourceRange();
    } else if (auto *DS{dyn_cast<clang::DeclStmt>(
                   CanonicalInfo.getASTLoop()->getInit())}) {
      InitRange = DS->child_begin()->getSourceRange();
    } else {
      toDiag(mSrcMgr.getDiagnostics(),
        CanonicalInfo.getASTLoop()->getBeginLoc(),
        tsar::diag::warn_reverse_initialization_complex);
      return false;
    }
    auto InitStr{mRewriter.getRewrittenText(InitRange)};
    if (Start) {
      SmallString<8> Init;
      Start->toString(Init, 10);
      InitStr = Init.str().str();
    }
    auto CondStr{mRewriter.getRewrittenText(CE.ExprRange)};
    SmallString<128> NewInitStr;
    if (End && Step) {
      (*End - *Step).toString(NewInitStr);
    } else if (IE.Flags & IncrementInfo::Unary) {
      if (CE.Flags & ConditionInfo::Exclude)
        if (IE.Flags & IncrementInfo::Sub)
          NewInitStr = CondStr + "-1";
        else
          NewInitStr = CondStr + "+1";
      else
        NewInitStr = CondStr;
    } else {
      auto IncrStr{mRewriter.getRewrittenText(IE.ExprRange)};
      if (CE.Flags & ConditionInfo::Exclude) {
        if (IE.Flags & IncrementInfo::Sub)
          NewInitStr = InitStr + "+(" + CondStr + " - 1 - (" + InitStr +
                       "))/(" + IncrStr + ")*(" + IncrStr + ")";
        else
          NewInitStr = InitStr + "-(" + InitStr + "-(" + CondStr + "+1))/(" +
                       IncrStr + ")*(" + IncrStr + ")";

      } else {
        if (IE.Flags & IncrementInfo::Sub)
          NewInitStr = InitStr + "+(" + CondStr + "-(" + InitStr + "))/(" +
                       IncrStr + ")*(" + IncrStr + ")";
        else
          NewInitStr = InitStr + "-(" + InitStr + "-(" + CondStr + "))/(" +
                       IncrStr + ")*(" + IncrStr + ")";
      }
    }
    if (IE.Flags & IncrementInfo::Assign) {
      mRewriter.InsertTextAfter(IE.ExprRange.getBegin(), "-(");
      mRewriter.InsertTextAfterToken(IE.ExprRange.getEnd(), ")");
    } else {
      mRewriter.ReplaceText(IE.OpRange, IE.NewOp);
    }
    mRewriter.ReplaceText(CE.OpRange, CE.NewOp);
    mRewriter.ReplaceText(InitRange, NewInitStr);
    mRewriter.ReplaceText(CE.ExprRange, InitStr);
    return true;
  }

  const ASTImportInfo &mImportInfo;
  clang::Rewriter &mRewriter;
  clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;
  ClangGlobalInfo::RawInfo &mRawInfo;
  const GlobalOptions &mGlobalOpts;
  MemoryMatchInfo::MemoryMatcher &mMemMatcher;
  const ClangDIMemoryMatcherPass::DIMemoryMatcher &mDIMemMatcher;
  PerfectLoopInfo &mPerfectLoopInfo;
  const CanonicalLoopSet &mCanonicalLoopInfo;
  DIMemoryClientServerInfo mDIMInfo;
  LoopAttributesDeductionPass &mLoopAttr;
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
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError(
        "cannot transform sources"
        ": transformation context is not available for the '" +
        F.getName() + "' function");
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
