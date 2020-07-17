//===--- ClauseVisitor.h - SAPFOR Specific Clause Parser --------*- C++ -*-===//
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
// This file provides general visitor to parse clause body.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLAUSE_PARSER_H
#define TSAR_CLAUSE_PARSER_H

#include "tsar/Support/Directives.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include <clang/Lex/LexDiagnostic.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "pragma-handler"

namespace tsar {
/// \brief This is a base class for visitors which process body of a clause.
///
/// To define new visitor:
/// - inherit this class (set VisitorT to the derived class).
/// - implement some of methods visitDefault(), visitEK_..., visitSingleExpr().
template<class PreprocessorT, class ReplacementT, class VisitorT>
class ClauseVisitor {
  /// Type of indexes and sizes of token containers.
  using size_type = typename ReplacementT::size_type;

  /// This type used to iterate over clause prototype.
  using iterator = ClausePrototype::const_iterator;

  /// Each complex expression (for example EK_ZeroOrMore produces a new level.
  /// Description of each level is pushed on the top of a stack of levels.
  struct ExprLevel {
    /// This pair contains:
    /// - start position of complex expression,
    /// - `true` if a body of a complex expression has been successfully
    /// visited at least once. For example, body of EK_ZeroOrMore may be visited
    /// zero or more times.
    llvm::PointerIntPair<iterator, 1, bool> Occurrence;

    /// Position of the first token in the representation of the next occurrence
    /// of the body of a complex expression after processing.
    ///
    /// Let us consider an example. Prototype is `EK_ZeroOrMore(EK_Identifier)`.
    /// Tokens are `A B`. Replacement is `<replacement of A> <replacement of B>`.
    /// At first, before processing of `A`:
    /// - ReplacementIdx is a position of the first token in <replacement of A>.
    /// Then, after processing of `A` (this is the first occurrence of the
    /// body of the complex expression `EK_ZeroOrMore`):
    /// - ReplacementIdx is a position of the first token in <replacement of B>.
    size_type ReplacementIdx;

    /// The last read token.
    clang::Token Tok;
  };

  /// Stack of levels.
  using ExprLevelStack = llvm::SmallVector<ExprLevel, 4>;

public:
  /// Creates visitor.
  ClauseVisitor(PreprocessorT &PP, ReplacementT &Replacement) :
    mPP(PP), mReplacement(Replacement) {}

  ReplacementT & getReplacement() noexcept { return mReplacement; }
  const ReplacementT & getReplacement() const noexcept { return mReplacement; }

  PreprocessorT & getPreprocessor() noexcept { return mPP; }
  const PreprocessorT & getPreprocessor() const noexcept { return mPP; }

  /// Returns complex expression at the current level or ClauseExpr::NotExpr.
  ClauseExpr getLevelKind() const {
    return mExprStack.empty() ? ClauseExpr::NotExpr :
      *mExprStack.back().Occurrence.getPointer();
  }

  /// Returns true if the body of the current complex expression has been
  /// successfully visited at least once.
  bool isLevelOccured() const {
    return mExprStack.empty() ? false : mExprStack.back().Occurrence.getInt();
  }

  /// \brief Processes body of a clause according to a prototype [I, EI).
  ///
  /// \pre
  ///   - The next lexed token is a first token of the clause body.
  ///   - On success, `Tok` is a last token in clause body.
  void visitBody(iterator I, iterator EI, clang::Token &Tok) {
    assert(mExprStack.empty() && "Expression level stack must be empty!");
#ifdef LLVM_DEBUG
    auto StartI = I;
#endif
    mPP.EnableBacktrackAtThisPos();
    for (; I != EI; ++I) {
      LLVM_DEBUG(processPrototypeLog(StartI, I));
      switch (*I) {
      case ClauseExpr::EK_One:
        push(I, Tok);
        static_cast<VisitorT *>(this)->visitEK_One(Tok);
        break;
      case ClauseExpr::EK_ZeroOrMore:
        push(I, Tok);
        static_cast<VisitorT *>(this)->visitEK_ZeroOrMore(Tok);
        break;
      case ClauseExpr::EK_ZeroOrOne:
        push(I, Tok);
        static_cast<VisitorT *>(this)->visitEK_ZeroOrOne(Tok);
        break;
      case ClauseExpr::EK_OneOrMore:
        static_cast<VisitorT *>(this)->visitEK_OneOrMore(Tok);
        push(I, Tok);
        break;
      case ClauseExpr::EK_OneOf:
        push(I, Tok);
        static_cast<VisitorT *>(this)->visitEK_OneOf(Tok);
        break;
      case ClauseExpr::EK_Anchor:
        static_cast<VisitorT *>(this)->visitEK_Anchor(Tok);
        I = visitAnchor(Tok, I, EI);
        break;
      default:
        mPP.Lex(Tok);
        LLVM_DEBUG(visitTokenLog(*I, Tok.getKind()));
        if (Tok.is(getTokenKind(*I))) {
          static_cast<VisitorT *>(this)->visitSingleExpr(*I, Tok);
          if (!mExprStack.empty() &&
              *mExprStack.back().Occurrence.getPointer() == ClauseExpr::EK_OneOf)
            I = findAnchor(I, EI) - 1;
        } else {
          auto Backup = EI;
          if (mExprStack.empty() && Tok.is(clang::tok::eod)) {
            // Do not consume tok::eod to enable further processing of input.
            mPP.RevertCachedTokens(1);
            LLVM_DEBUG(llvm::dbgs() << "[PRAGMA HANDLER]: revert 'tok::eod'\n");
          } else {
            Backup = backtrack(I, EI, Tok);
          }
          if (Backup == EI) {
            assert(mExprStack.empty() &&
              "Seems that clause prototype is invalid and some anchors are lost!");
            mPP.CommitBacktrackedTokens();
            diagExpected(Tok, I);
            return;
          }
          I = Backup;
          LLVM_DEBUG(backtrackLog(StartI, I, EI));
        }
        break;
      }
    }
    mPP.CommitBacktrackedTokens();
    assert(mExprStack.empty() &&
      "Seems that clause prototype is invalid and some anchors are lost!");
  }

  /// This visitor redirect calls to concrete visitors.
  void visitSingleExpr(ClauseExpr EK, clang::Token &Tok) {
    switch (EK) {
    default:
      llvm_unreachable("There is no appropriate token for clause expression!");
#define KIND(EK, IsSingle, ClangTok) \
     case ClauseExpr::EK: \
       assert(IsSingle && "Complex expression must be unreachable here!"); \
       static_cast<VisitorT *>(this)->visit##EK(Tok); \
       break;
#define GET_CLAUSE_EXPR_KINDS
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_EXPR_KINDS
#undef KIND
    }
  }

#define KIND(EK, IsSingle, ClangTok) \
  void visit##EK(clang::Token &Tok) { \
    static_cast<VisitorT *>(this)->visitDefault(Tok); \
  }
#define GET_CLAUSE_EXPR_KINDS
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_EXPR_KINDS
#undef KIND

  /// This default visitor is called if concrete visitors are not defined.
  void visitDefault(clang::Token &Tok) {}

private:
  /// Pushes expression level on the top of stack.
  void push(iterator I, clang::Token &Tok) {
    assert(!isSingle(*I) && "Expression must be complex!");
    mExprStack.push_back({ {I, false}, mReplacement.size(), Tok });
    mPP.EnableBacktrackAtThisPos();
  }

  /// \brief Processes EK_Anchor expression which ends some complex expression.
  ///
  /// \param [in] Loc Currently processed position. This will be used in
  /// crated tokens.
  /// \param [in] I Iterator which points to EK_Anchor.
  ///
  /// 1. This method updates the top of ExprLevelStack (for example updates
  /// and ReplacementIdx) or pops some items from stack (if a complex expression
  /// has been fully processed).
  /// 2. It also commit backtracked tokens which correspond to the processed
  /// part of complex expression.
  /// 3. If it is necessary, it pushes some tokens at the end of Replacement.
  /// For example, `}` should be pushed after a body of EK_One.
  ///
  /// \return Iterator which points to the EK_Anchor which
  /// precedes an expression that must be processed further.
  iterator visitAnchor(clang::Token &Tok, iterator I, iterator EI) {
    assert(*I == ClauseExpr::EK_Anchor &&
      "Prototype expression must be EK_Anchor!");
    assert(!mExprStack.empty() && "Unexpected anchor!");
    assert(mPP.isBacktrackEnabled() && "Backtrack must be enabled!");
    auto &Level = mExprStack.back();
    Level.Occurrence.setInt(true);
    switch (*Level.Occurrence.getPointer()) {
    default:
      llvm_unreachable("Expression level is incompatible with EK_Anchor!");
    case ClauseExpr::EK_ZeroOrMore:
    case ClauseExpr::EK_OneOrMore:
      mPP.CommitBacktrackedTokens();
      mPP.EnableBacktrackAtThisPos();
      Level.ReplacementIdx = mReplacement.size();
      return Level.Occurrence.getPointer();
    case ClauseExpr::EK_One:
    case ClauseExpr::EK_ZeroOrOne:
    case ClauseExpr::EK_OneOf:
      mPP.CommitBacktrackedTokens();
      mExprStack.pop_back();
      break;
    }
    while (!mExprStack.empty() &&
           *mExprStack.back().Occurrence.getPointer() == ClauseExpr::EK_OneOf) {
      I = findAnchor(mExprStack.back().Occurrence.getPointer(), EI);
      static_cast<VisitorT *>(this)->visitEK_Anchor(Tok);
      mPP.CommitBacktrackedTokens();
      mExprStack.pop_back();
    }
    return I;
  };

  /// Tries to backtrack in case of errors.
  ///
  /// \return Iterator which points to the expression in prototype which
  /// precedes an expression that must be processed further. If rollback has
  /// been failed this method returns EI.
  /// \post This method removes tail from Replacement and extracts levels from
  /// ExprStack which correspond to erroneously processed token stream.
  /// Position of lexer will be updated using PP.Backtrack().
  iterator backtrack(iterator I, iterator EI, clang::Token &Tok) {
    while (!mExprStack.empty()) {
      auto &Level = mExprStack.back();
      switch (*Level.Occurrence.getPointer()) {
      case ClauseExpr::EK_OneOf: // try next item in the list of variants
        mPP.Backtrack();
        if (*(I + 1) == ClauseExpr::EK_Anchor) {
          I = I + 1;
          Tok = Level.Tok;
          mExprStack.pop_back();
          continue;
        }
        mReplacement.resize(Level.ReplacementIdx);
        mPP.EnableBacktrackAtThisPos();
        return I;
      case ClauseExpr::EK_One:
        I = findAnchor(I, EI);
        Tok = Level.Tok;
        mExprStack.pop_back();
        mPP.Backtrack();
        continue;
      case ClauseExpr::EK_ZeroOrMore:
      case ClauseExpr::EK_ZeroOrOne:
        while (!isSingle(*(I + 1)))
          I = findAnchor(I + 1, EI);
        I = findAnchor(I + 1, EI);
        break;
      case ClauseExpr::EK_OneOrMore:
        while (!isSingle(*(I + 1)))
          I = findAnchor(I + 1, EI);
        I = findAnchor(I + 1, EI);
        if (Level.Occurrence.getInt()) {
          break;
        } else {
          mPP.Backtrack();
          Tok = Level.Tok;
          mExprStack.pop_back();
          continue;
        }
      }
      mReplacement.resize(Level.ReplacementIdx);
      mPP.Backtrack();
      Tok = Level.Tok;
      mExprStack.pop_back();
      return I;
    }
    return EI;
  }

  /// Prints clang::diag_err_expected.
  void diagExpected(clang::Token &Tok, iterator I) {
    std::string Expected;
    if (const char *Punc = clang::tok::getPunctuatorSpelling(getTokenKind(*I)))
      Expected.append("'").append(Punc).append("'");
    else if (*I == ClauseExpr::EK_Identifier)
      Expected.append("identifier");
    else
      Expected.append("keyword");
    mPP.Diag(Tok.getLocation(), clang::diag::err_expected) << Expected;
  }


#ifdef LLVM_DEBUG
void visitTokenLog(ClauseExpr ExpectedId, clang::tok::TokenKind VisitedId) {
  llvm::dbgs()
    << "[PRAGMA HANDLER]: expected token '" << tsar::getName(ExpectedId)
    << "' visited token '" << clang::tok::getTokenName(VisitedId) << "'\n";
}

void processPrototypeLog(iterator StartI, iterator I) {
  llvm::dbgs()
    << "[PRAGMA HANDLER]: process '" << tsar::getName(*I)
    <<"' (offset " << std::distance(StartI, I) << ")\n";
}

void backtrackLog(iterator StartI,  iterator I, iterator EI) {
  llvm::dbgs() << "[PRAGMA HANDLER]: backtrack, skip prototype until ";
  if (I != EI)
    llvm::dbgs() << "'" << tsar::getName(*I) <<"' (offset " <<
       std::distance(StartI  , I) << ")\n";
  else
    llvm::dbgs() << "the end\n";
}
#endif

private:
  PreprocessorT &mPP;
  ReplacementT &mReplacement;
  ExprLevelStack mExprStack;
};
}
#endif//TSAR_CLAUSE_PARSER_H
