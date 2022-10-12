//=== UnreachableCodeElimination.cpp - Unreachable Code (Clang) --*- C++ -*===//
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
//===---------------------------------------------------------------------===//
//
// This file implements a pass to eliminate unreachable code in a source code
// using code coverage.
//
//===---------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Token.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/Debug.h>
#include <llvm/Pass.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-unreachable-code"

namespace {
class ClangUnreachableCodeEliminationPass : public FunctionPass,
                                         private bcl::Uncopyable {
public:
  static char ID;

  ClangUnreachableCodeEliminationPass() : FunctionPass(ID) {
    initializeClangUnreachableCodeEliminationPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class UnreachableCodeVisitor
    : public clang::RecursiveASTVisitor<UnreachableCodeVisitor> {
  using WarningDescription =
      std::pair<decltype(tsar::diag::NUM_BUILTIN_TSAR_DIAGNOSTICS),
                decltype(tsar::diag::NUM_BUILTIN_TSAR_DIAGNOSTICS)>;

public:
  explicit UnreachableCodeVisitor(clang::Rewriter &Rewriter,
                                  const coverage::CoverageData &Coverage,
                                  const ASTImportInfo &ImportInfo)
      : mRewriter(&Rewriter), mCoverage(Coverage), mImportInfo(ImportInfo),
        mSourceManager(Rewriter.getSourceMgr()),
        mLangOpts(Rewriter.getLangOpts()) {}

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    Pragma P(*S);
    if (P) {
      if (findClause(P, ClauseId::RemoveUnreachable, mClauses)) {
        llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
        auto IsPossible = pragmaRangeToRemove(P, mClauses, mSourceManager,
                                              mLangOpts, mImportInfo, ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSourceManager.getDiagnostics(),
                   mClauses.front()->getBeginLoc(),
                   tsar::diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSourceManager.getDiagnostics(),
                   mClauses.front()->getBeginLoc(),
                   tsar::diag::warn_remove_directive_in_include);
          else
            toDiag(mSourceManager.getDiagnostics(),
                   mClauses.front()->getBeginLoc(),
                   tsar::diag::warn_remove_directive);
        Rewriter::RewriteOptions RemoveEmptyLine;
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
        /// set to true then removing (in RewriterBuffer) works incorrect.
        RemoveEmptyLine.RemoveLineIfEmpty = false;
        for (auto SR : ToRemove)
          mRewriter->RemoveText(SR, RemoveEmptyLine);
      }
      return true;
    }
    if (!mClauses.empty() && !isa<CompoundStmt>(S)) {
      toDiag(mSourceManager.getDiagnostics(), mClauses.front()->getBeginLoc(),
        tsar::diag::warn_unexpected_directive);
      mClauses.clear();
    }
    if (mIsActive && isUnreachable(*S) &&
        !isMacroPrevent(S->getBeginLoc(), S->getBeginLoc(), S->getEndLoc())) {
      toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      mRewriter->RemoveText(getRemoveRangeWithSemi(S->getSourceRange()));
      return true;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseCompoundStmt(CompoundStmt *S) {
    if (mClauses.empty())
      return RecursiveASTVisitor::TraverseCompoundStmt(S);
    mClauses.clear();
    bool StashIsActive = mIsActive;
    mIsActive = true;
    auto Res{RecursiveASTVisitor::TraverseCompoundStmt(S)};
    mIsActive = StashIsActive;
    return true;
  }

  bool TraverseSwitchStmt(clang::SwitchStmt *Switch) {
    if (!mIsActive)
      return RecursiveASTVisitor::TraverseSwitchStmt(Switch);
    bool HasReachable{false};
    for (SwitchCase *Case{Switch->getSwitchCaseList()}, *Prev{nullptr}; Case;
         Prev = Case, Case = Case->getNextSwitchCase()) {
      if (isUnreachable(*Case) &&
          !isMacroPrevent(Case->getBeginLoc(), Case->getKeywordLoc(),
                          Case->getEndLoc())) {
        toDiag(mSourceManager.getDiagnostics(), Case->getBeginLoc(),
               tsar::diag::remark_remove_unreachable);
        mRewriter->RemoveText(
            getRemoveRangeWithSemi({Case->getKeywordLoc(), Case->getEndLoc()}));
      } else if (!TraverseStmt(Case)) {
        return false;
      } else {
        HasReachable = true;
      }
    }
    if (HasReachable)
      return TraverseStmt(Switch->getCond());
    if (auto *S{findSideEffect(*Switch->getCond())}) {
      toDiag(mSourceManager.getDiagnostics(), Switch->getCond()->getBeginLoc(),
             tsar::diag::warn_disable_de);
      toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
             tsar::diag::note_de_side_effect_prevent);
      return TraverseStmt(Switch->getCond());
    }
    if (isMacroPrevent(
            Switch->getBeginLoc(), Switch->getBeginLoc(), Switch->getEndLoc(),
            {tsar::diag::warn_disable_de, tsar::diag::note_de_macro_prevent}))
      return TraverseStmt(Switch->getCond());
    mRewriter->RemoveText(
        mSourceManager.getExpansionRange(Switch->getSourceRange()));
    return true;
  }

  bool TraverseBinaryOperator(clang::BinaryOperator *BO) {
    if (!mIsActive)
      return RecursiveASTVisitor::TraverseBinaryOperator(BO);
    if (BO->isLogicalOp())
      if (auto *RHS{BO->getRHS()};
          isUnreachable(*RHS) &&
          !isMacroPrevent(RHS->getBeginLoc(), BO->getOperatorLoc(),
                          RHS->getEndLoc())) {
        toDiag(mSourceManager.getDiagnostics(), RHS->getBeginLoc(),
               tsar::diag::remark_remove_unreachable);
        mRewriter->RemoveText(mSourceManager.getExpansionRange(
            SourceRange{BO->getOperatorLoc(), RHS->getEndLoc()}));
        return RecursiveASTVisitor::TraverseStmt(BO->getLHS());
      }
    return RecursiveASTVisitor::TraverseBinaryOperator(BO);
  }

  bool TraverseConditionalOperator(clang::ConditionalOperator *CO) {
    if (!mIsActive)
      return RecursiveASTVisitor::TraverseConditionalOperator(CO);
    if (auto *True{CO->getTrueExpr()}; isUnreachable(*True)) {
      if (auto *S{findSideEffect(*CO->getCond())}) {
        toDiag(mSourceManager.getDiagnostics(), CO->getCond()->getBeginLoc(),
               tsar::diag::warn_disable_de);
        toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::note_de_side_effect_prevent);
        if (isMacroPreventFront(True->getBeginLoc(), CO->getBeginLoc()) ||
            isMacroPrevent(True->getBeginLoc(), CO->getQuestionLoc(),
                           CO->getQuestionLoc()) ||
            isMacroPrevent(True->getBeginLoc(), True->getBeginLoc(),
                           CO->getColonLoc()) ||
            isMacroPreventBack(True->getBeginLoc(), CO->getEndLoc()))
          return RecursiveASTVisitor::TraverseConditionalOperator(CO);
        auto EndLoc{
            getEndLocWithSemi(mSourceManager.getExpansionLoc(CO->getEndLoc()))};
        if (isMacroPreventBack(True->getBeginLoc(), EndLoc))
          return RecursiveASTVisitor::TraverseConditionalOperator(CO);
        mRewriter->InsertTextBefore(
            mSourceManager.getExpansionLoc(CO->getBeginLoc()), "(");
        mRewriter->InsertTextAfter(mSourceManager.getExpansionLoc(EndLoc), ")");
        mRewriter->ReplaceText(
            mSourceManager.getExpansionLoc(CO->getQuestionLoc()), ",");
        mRewriter->RemoveText(mSourceManager.getExpansionRange(
            SourceRange{True->getBeginLoc(), CO->getColonLoc()}));
      } else if (!isMacroPrevent(True->getBeginLoc(), CO->getBeginLoc(),
                                 CO->getColonLoc())) {
        mRewriter->RemoveText(mSourceManager.getExpansionRange(
            SourceRange{CO->getBeginLoc(), CO->getColonLoc()}));
      } else {
        // Do not process true branch separately, otherwise the transformation
        // may produce incorrect statement cond ? : expr;
        return TraverseStmt(CO->getCond()) && TraverseStmt(CO->getFalseExpr());
      }
      toDiag(mSourceManager.getDiagnostics(), True->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      return TraverseStmt(CO->getCond()) && TraverseStmt(CO->getFalseExpr());
    }
    if (auto *False{CO->getFalseExpr()};
        isUnreachable(*False) &&
        !isMacroPrevent(False->getBeginLoc(), CO->getColonLoc(),
                        False->getEndLoc())) {
      if (auto *S{findSideEffect(*CO->getCond())}) {
        toDiag(mSourceManager.getDiagnostics(), CO->getCond()->getBeginLoc(),
               tsar::diag::warn_disable_de);
        toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::note_de_side_effect_prevent);
        if (isMacroPreventFront(False->getBeginLoc(), CO->getBeginLoc()) ||
            isMacroPrevent(False->getBeginLoc(), CO->getQuestionLoc(),
                           CO->getQuestionLoc()) ||
            isMacroPreventBack(False->getBeginLoc(), CO->getEndLoc()))
          return RecursiveASTVisitor::TraverseConditionalOperator(CO);
        auto EndLoc{
            getEndLocWithSemi(mSourceManager.getExpansionLoc(CO->getEndLoc()))};
        if (isMacroPreventBack(False->getBeginLoc(), EndLoc))
          return RecursiveASTVisitor::TraverseConditionalOperator(CO);
        mRewriter->InsertTextBefore(
            mSourceManager.getExpansionLoc(CO->getBeginLoc()), "(");
        mRewriter->InsertTextAfter(mSourceManager.getExpansionLoc(EndLoc), ")");
        mRewriter->ReplaceText(
            mSourceManager.getExpansionLoc(CO->getQuestionLoc()), ",");
      } else if (!isMacroPrevent(False->getBeginLoc(), CO->getBeginLoc(),
                                CO->getQuestionLoc())) {
        mRewriter->RemoveText(mSourceManager.getExpansionRange(
            SourceRange{CO->getBeginLoc(), CO->getQuestionLoc()}));
      } else {
        // Do not process false branch separately, otherwise the transformation
        // may produce incorrect statement cond ? expr: ;
        return TraverseStmt(CO->getCond()) && TraverseStmt(CO->getTrueExpr());
      }
      mRewriter->RemoveText(mSourceManager.getExpansionRange(
          SourceRange{CO->getColonLoc(), False->getEndLoc()}));
      toDiag(mSourceManager.getDiagnostics(), False->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      return TraverseStmt(CO->getCond()) && TraverseStmt(CO->getTrueExpr());
    }
    return RecursiveASTVisitor::TraverseConditionalOperator(CO);
  }

  bool TraverseIfStmt(clang::IfStmt *If) {
    if (!mIsActive)
      return RecursiveASTVisitor::TraverseIfStmt(If);
    auto Then{If->getThen()};
    auto Else{If->getElse()};
    bool ThenUnreachable{Then && isUnreachable(*Then) &&
                         !isMacroPrevent(Then->getBeginLoc(),
                                         Then->getBeginLoc(),
                                         Then->getEndLoc())};
    bool ElseUnreachable{Else && isUnreachable(*Else) &&
                         !isMacroPrevent(Else->getBeginLoc(),
                                         Else->getBeginLoc(),
                                         Else->getEndLoc())};
    if (!ThenUnreachable && !ElseUnreachable)
      return RecursiveASTVisitor::TraverseIfStmt(If);
    std::function<void(SourceRange)> removeBranch = [this](SourceRange Range) {
      mRewriter->ReplaceText(Range, ";");
    };
    std::function<void(SourceRange)> removeElseKeyword = [](SourceRange) {};
    if (auto *Init{If->getInit()}) {
      if (!TraverseStmt(Init))
        return false;
    } else if (auto *DS{If->getConditionVariableDeclStmt()}) {
      if (!TraverseStmt(DS))
        return false;
    } else if (!isMacroPreventBack(If->getIfLoc(), If->getIfLoc(),
                                   {tsar::diag::warn_disable_de,
                                    tsar::diag::note_de_macro_prevent})) {
      Token Tok;
      getRawTokenAfter(mSourceManager.getExpansionLoc(If->getIfLoc()),
                       mSourceManager, mLangOpts, Tok);
      auto Reachable{ThenUnreachable ? Else : Then};
      if (Tok.is(tok::l_paren) &&
          (!Reachable ||
           !isMacroPrevent(If->getBeginLoc(), Reachable->getBeginLoc(),
                           Reachable->getBeginLoc(),
                           {tsar::diag::warn_disable_de,
                            tsar::diag::note_de_macro_prevent}) &&
               !isMacroPrevent(If->getBeginLoc(), Reachable->getEndLoc(),
                               Reachable->getEndLoc(),
                               {tsar::diag::warn_disable_de,
                                tsar::diag::note_de_macro_prevent})) &&
          !isMacroPrevent(If->getBeginLoc(), If->getBeginLoc(),
                          If->getRParenLoc(),
                          {tsar::diag::warn_disable_de,
                           tsar::diag::note_de_macro_prevent}) &&
          !isMacroPrevent(If->getBeginLoc(), If->getIfLoc(), Tok.getLocation(),
                          {tsar::diag::warn_disable_de,
                           tsar::diag::note_de_macro_prevent}) &&
          !isMacroPrevent(If->getBeginLoc(), If->getRParenLoc(),
                          If->getRParenLoc(),
                          {tsar::diag::warn_disable_de,
                           tsar::diag::note_de_macro_prevent})) {
        if (Reachable)
          if (!isa<DeclStmt>(Reachable) &&
              none_of(Reachable->children(),
                      [](auto *S) { return isa<DeclStmt>(S); })) {
            if (isa<CompoundStmt>(Reachable)) {
              mRewriter->RemoveText(
                  mSourceManager.getExpansionLoc(Reachable->getBeginLoc()));
              mRewriter->RemoveText(
                  mSourceManager.getExpansionLoc(Reachable->getEndLoc()));
            }
          } else if (!isa<CompoundStmt>(Reachable)) {
            mRewriter->InsertTextBefore(
                mSourceManager.getExpansionLoc(Reachable->getBeginLoc()), "{");
            mRewriter->InsertTextAfter(
                mSourceManager.getExpansionLoc(Reachable->getEndLoc()), "}");
          }
        removeElseKeyword = removeBranch = [this](SourceRange Range) {
          mRewriter->RemoveText(Range);
        };
        if (auto *S{findSideEffect(*If->getCond())}; !S) {
          mRewriter->RemoveText(mSourceManager.getExpansionRange(
              SourceRange{If->getBeginLoc(), If->getRParenLoc()}));
        } else {
          toDiag(mSourceManager.getDiagnostics(), If->getCond()->getBeginLoc(),
                 tsar::diag::warn_disable_de);
          toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::note_de_side_effect_prevent);
          mRewriter->RemoveText(mSourceManager.getExpansionRange(
              SourceRange{If->getIfLoc(), Tok.getLocation()}));
          mRewriter->ReplaceText(
              mSourceManager.getExpansionLoc(If->getRParenLoc()), ";");
          if (!TraverseStmt(If->getCond()))
            return false;
        }
      } else if (!TraverseStmt(If->getCond())) {
        return false;
      }
    }
    if (ThenUnreachable) {
      toDiag(mSourceManager.getDiagnostics(), Then->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      if (isa<CompoundStmt>(Then))
        removeBranch(mSourceManager.getExpansionRange(Then->getSourceRange())
                         .getAsRange());
      else
        removeBranch(getRemoveRangeWithSemi(Then->getSourceRange()));
      if (Else)
        removeElseKeyword(mSourceManager.getExpansionLoc(If->getElseLoc()));
      Then = nullptr;
    }
    if (ElseUnreachable) {
      toDiag(mSourceManager.getDiagnostics(), Else->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      if (isa<CompoundStmt>(Else))
        removeBranch(mSourceManager.getExpansionRange(Else->getSourceRange())
                         .getAsRange());
      else
        removeBranch(getRemoveRangeWithSemi(Else->getSourceRange()));
      removeElseKeyword(mSourceManager.getExpansionLoc(If->getElseLoc()));
      Else = nullptr;
    }
    auto Res{true};
    if (Then)
      Res &= TraverseStmt(Then);
    if (Else)
      Res &= TraverseStmt(Else);
    return Res;
  }

  bool TraverseWhileStmt(WhileStmt *While) {
    if (!mIsActive)
      return RecursiveASTVisitor::TraverseWhileStmt(While);
    auto *Body{While->getBody()};
    if (Body && isUnreachable(*Body) &&
        !isMacroPrevent(Body->getBeginLoc(), Body->getBeginLoc(),
                        Body->getEndLoc())) {
      if (isa<CompoundStmt>(Body))
        mRewriter->RemoveText(
            mSourceManager.getExpansionRange(Body->getSourceRange()));
      else
        mRewriter->RemoveText(getRemoveRangeWithSemi(Body->getSourceRange()));
      toDiag(mSourceManager.getDiagnostics(), Body->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      if (auto *S{findSideEffect(*While->getCond())}) {
        toDiag(mSourceManager.getDiagnostics(), While->getCond()->getBeginLoc(),
               tsar::diag::warn_disable_de);
        toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::note_de_side_effect_prevent);
        if (isMacroPrevent(Body->getBeginLoc(), While->getWhileLoc(),
                           While->getLParenLoc(),
                           {tsar::diag::warn_disable_de,
                            tsar::diag::note_de_macro_prevent}) ||
            isMacroPrevent(Body->getBeginLoc(), While->getRParenLoc(),
                           While->getRParenLoc(),
                           {tsar::diag::warn_disable_de,
                            tsar::diag::note_de_macro_prevent}))
          return TraverseStmt(While->getCond());
        mRewriter->RemoveText(mSourceManager.getExpansionRange(
            SourceRange{While->getWhileLoc(), While->getLParenLoc()}));
        mRewriter->ReplaceText(
            mSourceManager.getExpansionLoc(While->getRParenLoc()), ";");
        return TraverseStmt(While->getCond());
      }
      if (isMacroPrevent(
              While->getBeginLoc(), While->getBeginLoc(), While->getRParenLoc(),
              {tsar::diag::warn_disable_de, tsar::diag::note_de_macro_prevent}))
        return true;
      mRewriter->RemoveText(mSourceManager.getExpansionRange(
          SourceRange{While->getBeginLoc(), While->getRParenLoc()}));
      return true;
    }
    return RecursiveASTVisitor::TraverseWhileStmt(While);
  }

  bool TraverseForStmt(clang::ForStmt *For) {
    if (!mIsActive)
      return RecursiveASTVisitor::TraverseForStmt(For);
    auto *Body{For->getBody()};
    if (Body && isUnreachable(*Body) &&
        !isMacroPrevent(Body->getBeginLoc(), Body->getBeginLoc(),
                        Body->getEndLoc())) {
      toDiag(mSourceManager.getDiagnostics(), Body->getBeginLoc(),
             tsar::diag::remark_remove_unreachable);
      auto *Init{For->getInit()};
      auto *Cond{For->getCond()};
      auto *Inc{For->getInc()};
      if (isa_and_nonnull<DeclStmt>(Init) ||
          (Init && (isMacroPrevent(Init->getBeginLoc(), Init->getBeginLoc(),
                                   Init->getEndLoc(),
                                   {tsar::diag::warn_disable_de,
                                    tsar::diag::note_de_macro_prevent}) ||
                    isMacroPrevent(Init->getBeginLoc(), Init->getEndLoc(),
                                   Init->getEndLoc(),
                                   {tsar::diag::warn_disable_de,
                                    tsar::diag::note_de_macro_prevent}))) ||
          (Cond && isMacroPrevent(Cond->getBeginLoc(), Cond->getBeginLoc(),
                                  Cond->getEndLoc(),
                                  {tsar::diag::warn_disable_de,
                                   tsar::diag::note_de_macro_prevent})) ||
          isMacroPrevent(For->getBeginLoc(), For->getBeginLoc(),
                         For->getRParenLoc(),
                         {tsar::diag::warn_disable_de,
                          tsar::diag::note_de_macro_prevent}) ||
          isMacroPrevent(For->getBeginLoc(), For->getRParenLoc(),
                         For->getRParenLoc(),
                         {tsar::diag::warn_disable_de,
                          tsar::diag::note_de_macro_prevent}) ||
          isMacroPrevent(For->getBeginLoc(), For->getLParenLoc(),
                         For->getLParenLoc(),
                         {tsar::diag::warn_disable_de,
                          tsar::diag::note_de_macro_prevent}) ||
          (Inc && isMacroPrevent(For->getBeginLoc(), Inc->getBeginLoc(),
                                 For->getLParenLoc(),
                                 {tsar::diag::warn_disable_de,
                                  tsar::diag::note_de_macro_prevent}))) {
        if (isa<CompoundStmt>(Body))
          mRewriter->ReplaceText(
              mSourceManager.getExpansionRange(Body->getSourceRange()), ";");
        else
          mRewriter->RemoveText(
              mSourceManager.getExpansionRange(Body->getSourceRange()));
        if (Inc && !isMacroPrevent(Inc->getBeginLoc(), Inc->getBeginLoc(),
                                   Inc->getEndLoc()))
          mRewriter->RemoveText(
              mSourceManager.getExpansionRange(Inc->getSourceRange()));
        return TraverseStmt(Init) && TraverseStmt(For->getCond());
      }
      if (isa<CompoundStmt>(Body))
        mRewriter->RemoveText(
            mSourceManager.getExpansionRange(Body->getSourceRange()));
      else
        mRewriter->RemoveText(getRemoveRangeWithSemi(Body->getSourceRange()));
      if (Init)
        if (auto *S{findSideEffect(*Init)}) {
          toDiag(mSourceManager.getDiagnostics(), Init->getBeginLoc(),
                 tsar::diag::warn_disable_de);
          toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::note_de_side_effect_prevent);
        } else {
          mRewriter->RemoveText(getRemoveRangeWithSemi(Init->getSourceRange()));
          Init = nullptr;
        }
      if (Cond)
        if (auto *S{findSideEffect(*Cond)}) {
          toDiag(mSourceManager.getDiagnostics(), Cond->getBeginLoc(),
                 tsar::diag::warn_disable_de);
          toDiag(mSourceManager.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::note_de_side_effect_prevent);
        } else {
          mRewriter->RemoveText(
              mSourceManager.getExpansionRange(Cond->getSourceRange()));
          Cond = nullptr;
        }
      if (!Init && !Cond) {
        mRewriter->RemoveText(mSourceManager.getExpansionRange(
            SourceRange{For->getBeginLoc(), For->getRParenLoc()}));
        return true;
      }
      if (!Init)
        mRewriter->RemoveText(getRemoveRangeWithSemi(For->getLParenLoc()));
      else if (!Cond)
        mRewriter->RemoveText(getRemoveRangeWithSemi(Init->getEndLoc()));
      mRewriter->RemoveText(
          getRemoveRangeWithSemi({For->getBeginLoc(), For->getLParenLoc()}));
      if (Inc)
        mRewriter->RemoveText(
            getRemoveRangeWithSemi((Inc->getBeginLoc(), For->getRParenLoc())));
      else
        mRewriter->RemoveText(
            mSourceManager.getExpansionLoc(For->getRParenLoc()));
      return TraverseStmt(Init) && TraverseStmt(Cond);
    }
    return RecursiveASTVisitor::TraverseForStmt(For);
  }

private:
  template<typename T>
  bool isUnreachable(const T &S) const {
    auto Range{mSourceManager.getExpansionRange(S.getSourceRange())};
    auto BeginLoc{mSourceManager.getPresumedLoc(Range.getBegin())};
    auto EndLoc{mSourceManager.getPresumedLoc(Range.getEnd())};
    LLVM_DEBUG(dbgs() << "[UNREACHABLE CODE]: check range: [";
               Range.getBegin().print(dbgs(), mSourceManager); dbgs() << ",";
               Range.getEnd().print(dbgs(), mSourceManager); dbgs() << "]\n");
    auto BI{mCoverage.begin()}, EI{mCoverage.end()};
    if (BI->Line > BeginLoc.getLine() ||
        BI->Line == BeginLoc.getLine() && BI->Col > BeginLoc.getColumn())
      return true;
    for (;;) {
      auto Size{std::distance(BI, EI)};
      if (Size == 1)
        break;
      auto Half{Size / 2};
      auto HI{BI + Half};
      if (HI->Line < BeginLoc.getLine() ||
          HI->Line == BeginLoc.getLine() && HI->Col <= BeginLoc.getColumn())
        BI = HI;
      else
        EI = HI;
    }
    for (EI = mCoverage.end();
         BI != EI && BI->Line < EndLoc.getLine() ||
         BI->Line == EndLoc.getLine() && BI->Col <= EndLoc.getColumn();
         ++BI) {
      LLVM_DEBUG(dbgs() << "[UNREACHABLE CODE]: count for " << BI->Line << ":"
                        << BI->Col << " is " << BI->Count << "\n");
      if (!BI->HasCount || BI->Count != 0)
        return false;
    }
    LLVM_DEBUG(dbgs() << "[UNREACHABLE CODE]: unreachable range found\n");
    return true;
  }

  SourceLocation getEndLocWithSemi(SourceLocation Loc) const {
    assert(Loc.isValid() && Loc.isFileID() &&
           "Expected a valid file location!");
    clang::Token NextToken;
    return (!getRawTokenAfter(Loc, mSourceManager, mRewriter->getLangOpts(),
                              NextToken) &&
            NextToken.is(tok::semi))
               ? NextToken.getLocation()
               : Loc;
  }

  SourceRange getRemoveRangeWithSemi(SourceRange Range) const {
    auto RemoveRange{mSourceManager.getExpansionRange(Range)};
    auto SemiLoc{getEndLocWithSemi(RemoveRange.getEnd())};
    if (!isMacroPreventBack(SemiLoc, SemiLoc))
      RemoveRange.setEnd(mSourceManager.getExpansionLoc(SemiLoc));
    return RemoveRange.getAsRange();
  };

  bool
  isMacroPrevent(SourceLocation Target, SourceLocation Begin,
                 SourceLocation End,
                 WarningDescription Diags = WarningDescription{
                     tsar::diag::warn_disable_remove_unreachable,
                     tsar::diag::note_remove_unreachable_macro_prevent}) const {
    return isMacroPreventFront(Target, Begin) ||
           isMacroPreventBack(Target, End);
  }

  bool isMacroPreventFront(
      SourceLocation Target, SourceLocation Loc,
      WarningDescription Diags = WarningDescription{
          tsar::diag::warn_disable_remove_unreachable,
          tsar::diag::note_remove_unreachable_macro_prevent}) const {
    if (Loc.isFileID() ||
        Lexer::isAtStartOfMacroExpansion(Loc, mSourceManager, mLangOpts))
      return false;
    toDiag(mSourceManager.getDiagnostics(), Target, Diags.first);
    toDiag(mSourceManager.getDiagnostics(), Loc, Diags.second);
    return true;
  }

  bool isMacroPreventBack(SourceLocation Target, SourceLocation Loc,
      WarningDescription Diags = WarningDescription{
          tsar::diag::warn_disable_remove_unreachable,
          tsar::diag::note_remove_unreachable_macro_prevent}) const {
    if (Loc.isFileID() ||
        Lexer::isAtEndOfMacroExpansion(Loc, mSourceManager, mLangOpts))
      return false;
    toDiag(mSourceManager.getDiagnostics(), Target, Diags.first);
    toDiag(mSourceManager.getDiagnostics(), Loc, Diags.second);
    return true;
  }

  clang::Rewriter *mRewriter;
  const coverage::CoverageData &mCoverage;
  const ASTImportInfo &mImportInfo;
  clang::SourceManager &mSourceManager;
  const clang::LangOptions &mLangOpts;
  bool mIsActive{false};
  SmallVector<Stmt *, 1> mClauses;
};
}

bool ClangUnreachableCodeEliminationPass::runOnFunction(Function &F) {
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
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  if (TfmCtx->getContext().getSourceManager().getFileCharacteristic(
          FuncDecl->getLocation()) != clang::SrcMgr::C_User)
    return false;
  auto GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  if (GO.ProfileUse.empty()) {
    toDiag(TfmCtx->getContext().getDiagnostics(), FuncDecl->getLocation(),
           tsar::diag::err_no_coverage_file);
    return false;
  }
  std::vector<StringRef> ObjectFilenames{GO.ObjectFilenames.size()};
  transform(GO.ObjectFilenames, ObjectFilenames.begin(),
            [](auto &F) { return StringRef{F}; });
  auto CoverageOrErr{
      coverage::CoverageMapping::load(ObjectFilenames, GO.ProfileUse)};
  if (auto E{CoverageOrErr.takeError()}) {
    toDiag(TfmCtx->getContext().getDiagnostics(),
           FuncDecl->getLocation(),
           tsar::diag::warn_no_coverage_data)
        << GO.ProfileUse << toString(std::move(E));
    return false;
  }
  auto FRI{find_if((**CoverageOrErr).getCoveredFunctions(),
                   [&F](const coverage::FunctionRecord &FR) {
                     return FR.Name == F.getName();
                   })};
  if (FRI == (**CoverageOrErr).getCoveredFunctions().end()) {
    toDiag(TfmCtx->getContext().getDiagnostics(),
           FuncDecl->getLocation(),
           tsar::diag ::warn_no_coverage_data_for)
        << GO.ProfileUse << F.getName();
    return false;
  }
  auto Coverage{(**CoverageOrErr).getCoverageForFunction(*FRI)};
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  UnreachableCodeVisitor{TfmCtx->getRewriter(), Coverage, *ImportInfo}
      .TraverseDecl(FuncDecl);
  return false;
}

void ClangUnreachableCodeEliminationPass::getAnalysisUsage(
    AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangUnreachableCodeEliminationPass() {
  return new ClangUnreachableCodeEliminationPass();
}

char ClangUnreachableCodeEliminationPass ::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangUnreachableCodeEliminationPass,
                               "clang-unreachable-code",
                               "Unreachable Code Elimination (Clang)", false,
                               false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangUnreachableCodeEliminationPass,
                             "clang-unreachable-code",
                             "Unreachable Code Elimination (Clang)", false,
                             false,
                             TransformationQueryManager::getPassRegistry())
