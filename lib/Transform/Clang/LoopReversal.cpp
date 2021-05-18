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
//#include "tsar/Analysis/AnalysisServer.h"
//#include "tsar/Support/PassGroupRegistry.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/LegacyPassManager.h>
//#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <string>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-loop-reverse"
#define DEBUG_PREFIX "[LOOP REVERSAL]: ";

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
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
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
          ToTransform.set(std::distance(mLoops.begin(), LoopItr));
        }
      }
      for (auto *Literal : mIntegerLiterals) {
        auto LoopIdx{Literal->getValue().getZExtValue() - 1};
        if (LoopIdx < mLoops.size()) {
          ToTransform.set(LoopIdx);
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
        Optional<llvm::APSInt> Start;
        Optional<llvm::APSInt> End;
        Optional<llvm::APSInt> Step;
        if (mIsStrict) {
          bool HasDependency{false};
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
        if (transformLoop(CanonicalInfo, Start, Step, End)) {
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
  bool transformLoop(const tsar::CanonicalLoopInfo *LI,
                     llvm::Optional<llvm::APSInt> &Start,
                     llvm::Optional<llvm::APSInt> &Step,
                     llvm::Optional<llvm::APSInt> &End) {
    if (!LI)
      return false;
    auto *FS = LI->getASTLoop();
    if (LI->isCanonical()) {
      auto *InitExpr = FS->getInit();
      auto *CondExpr = FS->getCond();
      auto *IncrExpr = FS->getInc();
      auto *IRInd = LI->getInduction();
      auto *ASTInd = mMemMatcher.find<IR>(IRInd)->get<AST>();
      bool IndSign = true;
      clang::SourceLocation IncrLoc;
      const char *NewIncrOp;
      clang::SourceRange IncExprSR;
      // if i = b + i -> i = i - b
      bool IncrSwapReq = false;
      bool UnaryIncr = false;
      clang::SourceRange IncrRightExprSR;
      clang::SourceRange IncrLeftExprSR;
      int Offset;
      // Increment expression transform
      if (auto *BO = dyn_cast<clang::BinaryOperator>(IncrExpr)) {
        switch (BO->getOpcode()) {
        case clang::BinaryOperator::Opcode::BO_AddAssign: {
          IncExprSR = BO->getRHS()->getSourceRange();
          NewIncrOp = "-=";
          Offset = 1;
          IncrLoc = BO->getOperatorLoc();
          IndSign = true;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_SubAssign: {
          IncExprSR = BO->getRHS()->getSourceRange();
          NewIncrOp = "+=";
          Offset = 1;
          IncrLoc = BO->getOperatorLoc();
          IndSign = false;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_Assign: {
          if (auto *OP = dyn_cast<clang::BinaryOperator>(BO->getRHS())) {
            switch (OP->getOpcode()) {
            case clang::BinaryOperator::Opcode::BO_Add: {
              auto *Left = OP->getLHS();
              auto *Right = OP->getRHS();
              MiniVisitor MV;
              MV.TraverseStmt(Right);
              if (auto *DRE = MV.getDRE()) {
                if (DRE->getDecl() == ASTInd) {
                  IncrSwapReq = true;
                }
              }
              IncrRightExprSR = Right->getSourceRange();
              IncrLeftExprSR = Left->getSourceRange();
              if (IncrSwapReq) {
                IncExprSR = IncrLeftExprSR;
              } else {
                IncExprSR = IncrRightExprSR;
              }
              Offset = 0;
              NewIncrOp = "-";
              IndSign = true;
              IncrLoc = OP->getOperatorLoc();
              break;
            }
            case clang::BinaryOperator::Opcode::BO_Sub: {
              IncExprSR = OP->getRHS()->getSourceRange();
              IncrLoc = OP->getOperatorLoc();
              Offset = 0;
              NewIncrOp = "+";
              IndSign = false;
              break;
            }
            default: {
              llvm_unreachable("Not canonical loop. Kind 1\n");
            }
            }
          } else {
            llvm_unreachable("Not canonical loop. Kind 2\n");
          }
          break;
        }
        default: {
          llvm_unreachable("Not canonical loop. Kind 3\n");
        }
        }

      } else if (auto *UO = dyn_cast<clang::UnaryOperator>(IncrExpr)) {
        UnaryIncr = true;
        switch (UO->getOpcode()) {
        case clang::UnaryOperator::Opcode::UO_PostInc:
        case clang::UnaryOperator::Opcode::UO_PreInc: {
          NewIncrOp = "--";
          Offset = 1;
          IncrLoc = UO->getOperatorLoc();
          IndSign = true;
          break;
        }
        case clang::UnaryOperator::Opcode::UO_PostDec:
        case clang::UnaryOperator::Opcode::UO_PreDec: {
          NewIncrOp = "++";
          Offset = 1;
          IncrLoc = UO->getOperatorLoc();
          IndSign = false;
          break;
        }
        default: {
          llvm_unreachable("Not canonical loop. Kind 5\n");
        }
        }
      } else {
        llvm_unreachable("Not canonical loop. Kind 4\n");
      }

      clang::SourceRange CondExprSR;
      clang::SourceRange CondIndSR;
      bool CondExclude = false;

      const char *NewCondOp;
      clang::SourceLocation CondOpLoc;
      if (auto *BO = dyn_cast<clang::BinaryOperator>(CondExpr)) {
        CondOpLoc = BO->getOperatorLoc();
        auto *Left = BO->getLHS();
        auto *Right = BO->getRHS();
        MiniVisitor MV;
        MV.TraverseStmt(Right);
        bool Flag = false;
        if (auto *DRE = MV.getDRE()) {
          if (DRE->getDecl() == ASTInd) {
            CondIndSR = Right->getSourceRange();
            CondExprSR = Left->getSourceRange();
            Flag = true;
          }
        }
        if (!Flag) {
          CondIndSR = Left->getSourceRange();
          CondExprSR = Right->getSourceRange();
        }
        switch (BO->getOpcode()) {
        case clang::BinaryOperator::Opcode::BO_LE: {
          NewCondOp = ">=";
          Offset = 1;
          CondExclude = false;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_LT: {
          NewCondOp = ">=";
          Offset = 0;
          CondExclude = true;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_GE: {
          NewCondOp = "<=";
          Offset = 1;
          CondExclude = false;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_GT: {
          NewCondOp = "<=";
          Offset = 0;
          CondExclude = true;
          break;
        }
        default: {
          llvm_unreachable("Not canonical loop. Kind 7\n");
        }
        }
      } else {
        llvm_unreachable("Not canonical loop. Kind 6\n");
      }
      clang::SourceRange InitExprSR;
      if (auto *BO = dyn_cast<clang::BinaryOperator>(InitExpr)) {
        InitExprSR = BO->getRHS()->getSourceRange();
      } else if (auto *DS = dyn_cast<clang::DeclStmt>(InitExpr)) {
        InitExprSR = DS->child_begin()->getSourceRange();
      } else {
        llvm_unreachable("Not canonical loop. Kind 8\n");
      }

      // source ranges for old operator signs to replace
      clang::SourceRange IncrOperatorSR(IncrLoc,
                                        IncrLoc.getLocWithOffset(Offset));

      clang::SourceRange CondOperatorSR(CondOpLoc,
                                        CondOpLoc.getLocWithOffset(Offset));
      // get strings from none transformed code
      auto TextInitExpr = mRewriter.getRewrittenText(InitExprSR);
      auto TextCondExpr = mRewriter.getRewrittenText(CondExprSR);
      std::string TextIncrExpr;
      std::string TextNewInitExpr;
      // if have start value from dep analysis
      if (Start.hasValue())
        TextInitExpr = std::to_string(Start->getSExtValue());
      // if have end and step value from dep analysis
      if (End.hasValue() && Step.hasValue()) {
        TextNewInitExpr =
            std::to_string(End->getSExtValue() - Step->getSExtValue());
        // no analysis -> calculate
      } else if (UnaryIncr) {
        if (CondExclude) {
          if (IndSign) {
            TextNewInitExpr = "((" + TextCondExpr + ")-1)";
          } else {
            TextNewInitExpr = "((" + TextCondExpr + ")+1)";
          }
        } else {
          TextNewInitExpr = "(" + TextCondExpr + ")";
        }
      } else {
        TextIncrExpr = mRewriter.getRewrittenText(IncExprSR);
        if (CondExclude) {
          if (IndSign) {
            TextNewInitExpr = "((" + TextInitExpr + ")+(((" + TextCondExpr +
                              ")-1)-(" + TextInitExpr + "))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          } else {
            TextNewInitExpr = "((" + TextInitExpr + ")-((" + TextInitExpr +
                              ")-((" + TextCondExpr + ")+1))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          }
        } else {
          if (IndSign) {
            TextNewInitExpr = "((" + TextInitExpr + ")+((" + TextCondExpr +
                              ")-(" + TextInitExpr + "))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          } else {
            TextNewInitExpr = "((" + TextInitExpr + ")-((" + TextInitExpr +
                              ")-(" + TextCondExpr + "))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          }
        }
      }
      // swap incr expr i = b + i -> i = i + b
      if (IncrSwapReq) {
        auto TextRightIncrExpr = mRewriter.getRewrittenText(IncrRightExprSR);
        auto TextLeftIncrExpr = mRewriter.getRewrittenText(IncrLeftExprSR);
        /*       if (Step.hasValue())
                 mRewriter.ReplaceText(
                     IncrRightExprSR,
                     "(" + std::to_string(abs(Step->getSExtValue())) + ")");
               else*/
        mRewriter.ReplaceText(IncrRightExprSR, TextLeftIncrExpr);
        mRewriter.ReplaceText(IncrLeftExprSR, TextRightIncrExpr);
      } /* else {
         if (!UnaryIncr && Step.hasValue())
           mRewriter.ReplaceText(
               IncExprSR, "(" + std::to_string(abs(Step->getSExtValue())) +
       ")");
       }*/
      // replace incr operator i++ -> i-- ; i+=k -> i-=k; etc
      mRewriter.ReplaceText(IncrOperatorSR, NewIncrOp);
      // replace cond operator
      mRewriter.ReplaceText(CondOperatorSR, NewCondOp);
      // replace init expression
      mRewriter.ReplaceText(InitExprSR, TextNewInitExpr);
      // replace cond expression
      mRewriter.ReplaceText(CondExprSR, TextInitExpr);
      return true;
    } else {
      toDiag(mSrcMgr.getDiagnostics(), FS->getBeginLoc(),
             tsar::diag::err_reverse_not_canonical);
      return false;
    }
    return false;
  }
  // written to don't add  math lib
  static int64_t abs(int64_t x) { return x > 0 ? x : -x; }

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
