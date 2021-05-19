//=== DeadDeclsElimination.cpp - Dead Decls Elimination (Clang) --*- C++ -*===//
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
//===---------------------------------------------------------------------===//
//
// This file defines a pass to eliminate unreachable calls in a source code.
//
//===---------------------------------------------------------------------===//

#include "tsar/Transform/Clang/UnreachableCallsElimination.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/UnreachableCountedRegions.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>

#include <iostream>
#include <sstream>


using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-unreachable-calls"

class ClangCopyPropagationInfo final : public PassGroupInfo {
  void addBeforePass(llvm::legacy::PassManager &PM) const override {
    PM.add(createClangUnreachableCountedRegions());
  }
};

char ClangUnreachableCallsElimination::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangUnreachableCallsElimination, "clang-unreachable-calls",
  "Unreachable Calls Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangCopyPropagationInfo);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangUnreachableCountedRegions)
INITIALIZE_PASS_IN_GROUP_END(ClangUnreachableCallsElimination, "clang-unreachable-calls",
  "Unreachable Calls Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {
class CallExprVisitor : public clang::RecursiveASTVisitor<CallExprVisitor> {
public:
  explicit CallExprVisitor(clang::Rewriter &Rewriter,
      const std::vector<llvm::coverage::CountedRegion> &Unreachable)
    : mRewriter(&Rewriter),
    mUnreachable(Unreachable),
    mSourceManager(Rewriter.getSourceMgr()) {}

  bool TraverseStmt(clang::Stmt *S) {
    if (!S) {
      return true;
    }

    if (clang::CompoundStmt *CS = dyn_cast<clang::CompoundStmt>(S)) {
      return RecursiveASTVisitor::TraverseCompoundStmt(CS);
    }
    if (clang::ReturnStmt *RS = dyn_cast<clang::ReturnStmt>(S)) {
      return TraverseReturnStmt(RS);
    }

    if (isUnreachable(S)) {
      makeEliminableFromTo(S->getBeginLoc(),
          shiftTokenIfEqualsGiven(S->getEndLoc()));  // semicolon
      return true;
    }

    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseReturnStmt(clang::ReturnStmt *RS) {
    if (!RS) {
      return true;
    }

    if (isUnreachable(RS)) {
      mUnreachableReturnStmts.push_back(RS);
      return true;
    } else {
      mHasReachableReturnStmt = true;
      return RecursiveASTVisitor::TraverseReturnStmt(RS);
    }
  }

  bool TraverseSwitchStmt(clang::SwitchStmt *SS) {
    if (!SS) {
      return true;
    }
 
    bool Res = true;
    SwitchCase *Iter = SS->getSwitchCaseList();
    SwitchCase *Prev = nullptr;
 
    // clang visits cases (including default) in reversed order (???)
    while (Iter) {
      if (isUnreachable(Iter)) {
        if (Prev) {
          makeEliminableFromTo(Iter->getKeywordLoc(),
              Prev->getKeywordLoc().getLocWithOffset(-1));
        } else {
          makeEliminableFromTo(Iter->getKeywordLoc(),
              SS->getEndLoc().getLocWithOffset(-1));
        }
      } else {
        if (clang::CaseStmt *CS = dyn_cast<clang::CaseStmt>(Iter)) {
          Res = Res && RecursiveASTVisitor::TraverseCaseStmt(CS);
        } else if (clang::DefaultStmt *DS = dyn_cast<clang::DefaultStmt>(Iter)) {
          Res = Res && RecursiveASTVisitor::TraverseDefaultStmt(DS);
        }
      }
      Prev = Iter;
      Iter = Iter->getNextSwitchCase();;
    }
 
    return Res;
  }

  bool TraverseBinaryOperator(clang::BinaryOperator *BO) {
    if (!BO) {
      return true;
    }
    if (!BO->isLogicalOp()) {
      return RecursiveASTVisitor::TraverseBinaryOperator(BO);
    }

    clang::Expr *RHS = BO->getRHS();

    if (isUnreachable(RHS)) {
      makeEliminableFromTo(BO->getOperatorLoc(), RHS->getEndLoc());
      clang::Expr *LHS = BO->getLHS();
      return RecursiveASTVisitor::TraverseStmt(LHS);
    }

    return RecursiveASTVisitor::TraverseBinaryOperator(BO);
  }

  bool TraverseConditionalOperator(clang::ConditionalOperator *CO) {
    if (!CO) {
      return true;
    }

    clang::Expr *Cond = CO->getCond();
    clang::Expr *TrueExpr = CO->getTrueExpr();
    clang::Expr *FalseExpr = CO->getFalseExpr();

    if (isUnreachable(TrueExpr)) {
      makeEliminableFromTo(CO->getBeginLoc(), CO->getColonLoc());
      return true;
    }

    if (isUnreachable(FalseExpr)) {
      makeEliminableFromTo(CO->getBeginLoc(), CO->getQuestionLoc());
      makeEliminableFromTo(CO->getColonLoc(), CO->getEndLoc());
      return true;
    }

    return RecursiveASTVisitor::TraverseConditionalOperator(CO);    
  }

  bool TraverseIfStmt(clang::IfStmt *IS) {
    if (!IS) {
      return true;
    }

    clang::Stmt *Then = IS->getThen();
    clang::Stmt *Else = IS->getElse();
    bool HasElse = IS->hasElseStorage();

    if (isUnreachable(Then)) {
      if (isa<clang::CompoundStmt>(Then)) {
        makeEliminableFromTo(IS->getIfLoc(), Then->getEndLoc());
      } else {
        makeEliminableFromTo(IS->getIfLoc(),
            shiftTokenIfEqualsGiven(Then->getEndLoc()));  // semicolon
      }
      if (HasElse) {
        if (isa<clang::CompoundStmt>(Else)) {
          makeEliminableFromTo(IS->getElseLoc(), Else->getBeginLoc());
          makeEliminableLoc(Else->getEndLoc());
        } else {
          makeEliminableLoc(IS->getElseLoc());  // this eliminates the entire 'else' word
        }
        return RecursiveASTVisitor::TraverseStmt(Else);
      }
      return true;
    }

    if (HasElse && isUnreachable(Else)) {
      if (isa<clang::CompoundStmt>(Else)) {
        makeEliminableFromTo(IS->getElseLoc(), Else->getEndLoc());
      } else {
        makeEliminableFromTo(IS->getElseLoc(),
            shiftTokenIfEqualsGiven(Else->getEndLoc()));  // semicolon
      }
      if (isa<clang::CompoundStmt>(Then)) {
        makeEliminableFromTo(IS->getIfLoc(), Then->getBeginLoc());
        makeEliminableLoc(Then->getEndLoc());
      } else {
        clang::Stmt *Cond = IS->getCond();
        makeEliminableFromTo(IS->getIfLoc(),
            shiftTokenIfEqualsGiven(Cond->getEndLoc(),
            clang::tok::r_paren));  // right parethesis
      }
      return RecursiveASTVisitor::TraverseStmt(Then);
    }

    return RecursiveASTVisitor::TraverseIfStmt(IS);
  }

  bool TraverseWhileStmt(clang::WhileStmt *WS) {
    if (!WS) {
      return true;
    }

    clang::Stmt *WhileBody = WS->getBody();

    if (isUnreachable(WhileBody)) {
      if (isa<clang::CompoundStmt>(WhileBody)) {
        makeEliminableFromTo(WS->getWhileLoc(), WhileBody->getEndLoc());
      } else {
        makeEliminableFromTo(WS->getWhileLoc(),
            shiftTokenIfEqualsGiven(WhileBody->getEndLoc()));  // semicolon
      }
      return true;
    }

    return RecursiveASTVisitor::TraverseWhileStmt(WS);
  }

  bool TraverseForStmt(clang::ForStmt *FS) {
    if (!FS) {
      return true;
    }

    clang::Stmt *ForBody = FS->getBody();

    if (isUnreachable(ForBody)) {
      if (isa<clang::CompoundStmt>(ForBody)) {
        makeEliminableFromTo(FS->getForLoc(), FS->getEndLoc());
      } else {
        makeEliminableFromTo(FS->getForLoc(),
            shiftTokenIfEqualsGiven(FS->getEndLoc()));  // semicolon
      }
      return true;
    }

    return RecursiveASTVisitor::TraverseForStmt(FS);
  }

  void eliminateUnreachableSourceRanges() {
    if (mHasReachableReturnStmt) {
      for (clang::ReturnStmt *RS : mUnreachableReturnStmts) {
        makeEliminableFromTo(RS->getBeginLoc(),
            shiftTokenIfEqualsGiven(RS->getEndLoc()));  // semicolon
      }
    }

    std::cout << "Starting unreachable code elimination" << std::endl;
    for (const auto &SR : mSourceRangesToEliminate) {
      std::cout << "\t" << sourceRangeToString(SR) << std::endl;
      mRewriter->RemoveText(SR);
    }
    std::cout << std::endl;
  }

private:
  /// Checks if S's source range is nested within any unreachable CountedRegion
  bool isUnreachable(clang::Stmt *S) {
    if (!S) {
      return false;
    }

    clang::SourceLocation BeginLoc = S->getBeginLoc();
    clang::SourceLocation EndLoc = S->getEndLoc();

    unsigned LineStart = mSourceManager.getExpansionLineNumber(BeginLoc);
    unsigned ColumnStart = mSourceManager.getExpansionColumnNumber(BeginLoc);
    unsigned LineEnd = mSourceManager.getExpansionLineNumber(EndLoc);
    unsigned ColumnEnd = mSourceManager.getExpansionColumnNumber(EndLoc);

    for (const auto &CR : mUnreachable) {
      if ((CR.LineStart < LineStart || (CR.LineStart == LineStart && CR.ColumnStart <= ColumnStart))
          && (CR.LineEnd > LineEnd || (CR.LineEnd == LineEnd && CR.ColumnEnd >= ColumnEnd))) {
        return true;
      }
    }

    return false;
  }

  /// Marks source range from BeginLoc to EndLoc as eliminable
  void makeEliminableFromTo(const clang::SourceLocation &BeginLoc,
      const clang::SourceLocation &EndLoc) {
    clang::SourceRange SourceRangeToEliminate{ BeginLoc, EndLoc };
    mSourceRangesToEliminate.push_back(SourceRangeToEliminate);
  }

  /// Marks Loc as eliminable
  void makeEliminableLoc(const clang::SourceLocation &Loc) {
    clang::SourceRange SourceRangeToEliminate{ Loc, Loc };
    mSourceRangesToEliminate.push_back(SourceRangeToEliminate);
  }

  /// If next token equals given one (semicolon by default) returns its location
  /// Else returns given location unchanged
  clang::SourceLocation shiftTokenIfEqualsGiven(clang::SourceLocation Loc,
      clang::tok::TokenKind ShiftTo = clang::tok::semi) {
    clang::Token NextToken;
    return (!getRawTokenAfter(Loc, mSourceManager, mRewriter->getLangOpts(), NextToken)
        && NextToken.is(ShiftTo))
        ? NextToken.getLocation()
        : Loc;
  }

  std::string sourceLocToString(const clang::SourceLocation &Loc) {
    unsigned Line = mSourceManager.getExpansionLineNumber(Loc);
    unsigned Column = mSourceManager.getExpansionColumnNumber(Loc);
    std::ostringstream out;
    out << Line << ":" << Column;
    return out.str();
  }

  std::string sourceRangeToString(const clang::SourceRange &SR) {
    clang::SourceLocation BeginLoc = SR.getBegin();
    clang::SourceLocation EndLoc = SR.getEnd();
    std::ostringstream out;
    out << "from " << sourceLocToString(BeginLoc)
        << " to " << sourceLocToString(EndLoc);
    return out.str();
  }

  clang::Rewriter *mRewriter;
  clang::SourceManager &mSourceManager;
  std::vector<clang::SourceRange> mSourceRangesToEliminate;
  const std::vector<llvm::coverage::CountedRegion> &mUnreachable;

  bool mHasReachableReturnStmt = false;
  std::vector<clang::ReturnStmt *> mUnreachableReturnStmts;
};
}


ClangUnreachableCallsElimination::ClangUnreachableCallsElimination() : FunctionPass(ID) {
  initializeClangUnreachableCallsEliminationPass(*PassRegistry::getPassRegistry());
}

bool ClangUnreachableCallsElimination::runOnFunction(Function &F) {
  errs() << "Run on function ";
  errs().write_escaped(F.getName()) << '\n';

  auto *M = F.getParent();

  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  auto *TfmCtx{TfmInfo ? TfmInfo->getContext(*M) : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M->getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }

  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;

  const auto &Unreachable = getAnalysis<ClangUnreachableCountedRegions>().getUnreachable();
  CallExprVisitor Visitor(TfmCtx->getRewriter(), Unreachable);
  Visitor.TraverseDecl(FuncDecl);
  Visitor.eliminateUnreachableSourceRanges();

  return false;
}

void ClangUnreachableCallsElimination::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<ClangUnreachableCountedRegions>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangUnreachableCallsElimination() {
  return new ClangUnreachableCallsElimination();
}
