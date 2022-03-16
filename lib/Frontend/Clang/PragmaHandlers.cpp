//=== Passes.cpp - Create and Initialize Parse Passes (Clang) - -*- C++ -*-===//
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
// This file implements SAPFOR specific #pragma handlers.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/PragmaHandlers.h"
#include "tsar/Frontend/Clang/ClauseVisitor.h"
#include "tsar/Frontend/Clang/ExternalPreprocessor.h"

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "pragma-handler"

namespace {
inline void AddToken(tok::TokenKind K, SourceLocation Loc, unsigned Len,
    SmallVectorImpl<Token> &TokenList) {
  Token Tok;
  Tok.startToken();
  Tok.setKind(K);
  Tok.setLocation(Loc);
  Tok.setLength(Len);
  TokenList.push_back(Tok);
}

template<class PreprocessorT >
inline void AddStringToken(StringRef Str, SourceLocation Loc, PreprocessorT &PP,
    SmallVectorImpl<Token> &TokenList) {
  Token Tok;
  Tok.startToken();
  Tok.setKind(tok::string_literal);
  PP.CreateString(("\"" + Str + "\"").str(), Tok, Loc, Loc);
  TokenList.push_back(Tok);
}

template<class PreprocessorT, class ReplacementT>
class DefaultClauseVisitor :
  public ClauseVisitor<PreprocessorT, ReplacementT,
      DefaultClauseVisitor<PreprocessorT, ReplacementT>> {
  using BaseT = ClauseVisitor<PreprocessorT, ReplacementT,
      DefaultClauseVisitor<PreprocessorT, ReplacementT>>;
public:
  /// Creates visitor.
  DefaultClauseVisitor(PreprocessorT &PP, ReplacementT &Replacement) :
    BaseT(PP, Replacement), mLangOpts(PP.getLangOpts()) {}

  using BaseT::getReplacement;
  using BaseT::getLevelKind;
  using BaseT::getPreprocessor;

  void visitEK_Anchor(Token &Tok) {
    if (getLevelKind() == ClauseExpr::EK_One) {
      auto End = Tok.getLocation();
      if (Tok.getLength() > 0)
        End = End.getLocWithOffset(Tok.getLength() - 1);
      AddToken(tok::r_brace, Tok.getLocation(), 1, getReplacement());
    }
  }

  void visitEK_One(Token &Tok) {
    auto End = Tok.getLocation().getLocWithOffset(Tok.getLength());
    AddToken(tok::l_brace, End, 1, getReplacement());
  }

  /// Assumes that a current token is an identifier and append to replacement
  /// something similar to `(void)(sizeof((long long)(A)))`
  /// (for identifier `A`).
  void visitEK_Identifier(Token &Tok) {
    assert(Tok.is(tok::identifier) && "Token must be an identifier!");
    // Each identifier 'I' will be replace by (void)(sizeof(&I)).
    // This construction is necessary to disable warnings for unused expressions
    // (cast to void) and to disable generation of LLVM IR for it (sizeof).
    // Cast to void inside 'sizeof' operator is necessary in case of variable
    // length array:
    // int N;
    // double A[N];
    // (void)(sizeof(A)) // This produces LLVM IR which computes size in dynamic.
    // (void)(sizeof((void)(A))) // This does not produce LLVM IR.
    // However it is forbidden to apply 'sizeof' to the void type in C++,
    // it is also forbidden to apply 'sizeof' to a function type in C++.
    // It is also forbidden to cast aggregate types to void and arithmetic
    // types, so the use of ampersand instead of a cast operation allows
    // using identifier of aggregate types.
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::kw_void, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::kw_sizeof, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::amp, Tok.getLocation(), 1, getReplacement());
    getReplacement().push_back(Tok);
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::semi, Tok.getLocation(), 1, getReplacement());
  }

  /// Assume that a current token is an preprocessor-level identifier and append
  /// to replacement something similar to `"name";`.
  ///
  /// Preprocessor-level identifiers are used to mark some common information
  /// for different directives (for example name of a region or interval).
  void visitEK_PPIdentifier(Token &Tok) {
    assert(Tok.is(tok::identifier) && "Token must be an identifier!");
    AddStringToken(Tok.getIdentifierInfo()->getName(), Tok.getLocation(),
                   getPreprocessor(), getReplacement());
    AddToken(tok::semi, Tok.getLocation(), 1, getReplacement());
  }

  /// Assumes that a current token is a numeric constant and append to
  /// replacement something similar to `(void)(5);` (for constant `5`).
  void visitEK_NumericConstant(Token &Tok) {
    assert(Tok.is(tok::numeric_constant) &&
           "Token must be a numeric constant!");
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::kw_void, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    getReplacement().push_back(Tok);
    AddToken(tok::semi, Tok.getLocation(), 1, getReplacement());
  }

private:
  const LangOptions &mLangOpts;
};
}

namespace tsar {

PragmaReplacer::PragmaReplacer(DirectiveId Id, PragmaNamespaceReplacer &Parent) :
  mName(tsar::getName(Id).str()),
  mDirectiveId(Id), mParent(&Parent) {
assert(tsar::getParent(Id) == Parent.getNamespaceId() &&
  "Incompatible namespace and directive IDs!");
}

void PragmaNamespaceReplacer::HandlePragma(
  Preprocessor &PP, PragmaIntroducer Introducer, Token &FirstToken) {
  mTokenQueue.clear();
  auto NamespaceLoc = FirstToken.getLocation();
  PP.LexUnexpandedToken(FirstToken);
  StringRef DirectiveName;
  if (FirstToken.is(tok::identifier)) {
    DirectiveName = FirstToken.getIdentifierInfo()->getName();
  } else if (auto *KW = tok::getKeywordSpelling(FirstToken.getKind())) {
    DirectiveName = KW;
  } else {
    PP.Diag(FirstToken, clang::diag::err_expected) << "name of directive";
    return;
  }
  DirectiveId Id;
  if (!getTsarDirective(mNamespaceId, DirectiveName, Id)) {
    toDiag(PP.getDiagnostics(), FirstToken.getLocation(),
      tsar::diag::err_unknown_directive) << getName() << DirectiveName;
    return;
  }
  auto *Handler = FindHandler(DirectiveName, false);
  if (!Handler) {
    PP.Diag(FirstToken, clang::diag::warn_pragma_ignored);
    return;
  }
  AddToken(tok::l_brace, NamespaceLoc, 1, mTokenQueue);
  AddStringToken(getName(), NamespaceLoc, PP, mTokenQueue);
  AddToken(tok::semi, NamespaceLoc, 1, mTokenQueue);
  auto RelexFrom = FirstToken;
  SmallVector<Token, 64> mTokensToRelex;
  do {
    PP.LexUnexpandedToken(FirstToken);
    mTokensToRelex.push_back(FirstToken);
  } while (!FirstToken.is(tok::eod));
  ExternalPreprocessor ExternalPP(PP, mTokensToRelex);
  Handler->HandlePragma(ExternalPP, Introducer.Kind, RelexFrom);
  // Replace pragma only if all tokens have been processed.
  if (RelexFrom.is(tok::eod)) {
    AddToken(tok::r_brace, FirstToken.getLocation(), 1, mTokenQueue);
    PP.EnterTokenStream(mTokenQueue, false, false);
  }
}

void PragmaReplacer::HandlePragma(ExternalPreprocessor &PP,
    PragmaIntroducerKind Introducer, Token &FirstToken) {
  assert(mParent && "Parent handler must not be null!");
  auto DirectiveLoc = FirstToken.getLocation();
  AddToken(tok::l_brace, DirectiveLoc, 1, getReplacement());
  AddStringToken(getName(), DirectiveLoc, PP, getReplacement());
  AddToken(tok::semi, DirectiveLoc, 1, getReplacement());
  // To represent the directive body a clause with empty name is used.
  // So do not read clause name if a directive has a body.
  bool HandleDirectiveBody = hasBody(mDirectiveId);
  if (!HandleDirectiveBody)
    PP.Lex(FirstToken);
  while (FirstToken.isNot(tok::eod)) {
    StringRef ClauseName;
    if (!HandleDirectiveBody) {
      if (FirstToken.is(tok::identifier)) {
        ClauseName = FirstToken.getIdentifierInfo()->getName();
      } else if (auto *KW = tok::getKeywordSpelling(FirstToken.getKind())) {
        ClauseName = KW;
      } else {
        PP.Diag(FirstToken, clang::diag::err_expected) << "name of clause";
        return;
      }
      HandleDirectiveBody = false;
    }
    ClauseId Id;
    if (!getTsarClause(mDirectiveId, ClauseName, Id)) {
      toDiag(PP.getDiagnostics(), FirstToken.getLocation(),
        diag::err_unknown_clause) << getName() << ClauseName;
      return;
    }
    auto *ClauseHandler = FindHandler(ClauseName, false);
    if (!ClauseHandler) {
      PP.Diag(FirstToken, clang::diag::warn_pragma_ignored);
      return;
    }
    ClauseHandler->HandleClause(PP, Introducer, FirstToken);
    assert(!PP.isBacktrackEnabled() &&
      "Did you forget to call CommitBacktrackedTokens() or Backtrack()?");
    PP.Lex(FirstToken);
  }
  AddToken(tok::r_brace, FirstToken.getLocation(), 1, getReplacement());
}

ClauseReplacer::ClauseReplacer(ClauseId Id, PragmaReplacer &Parent)
    : mName(tsar::getName(Id)), mClauseId(Id), mParent(&Parent) {
  assert(tsar::getParent(Id) == Parent.getDirectiveId() &&
         "Incompatible directive and clause IDs!");
}

void ClauseReplacer::HandleClause(
    ExternalPreprocessor &PP, PragmaIntroducerKind Introducer,
    Token &FirstToken) {
  auto ClauseLoc = FirstToken.getLocation();
  AddToken(tok::l_brace, ClauseLoc, 1, getReplacement());
  AddStringToken(getName(), ClauseLoc, PP, getReplacement());
  AddToken(tok::semi, ClauseLoc, 1, getReplacement());
  HandleBody(PP, Introducer, FirstToken);
  auto End = FirstToken.getLocation();
  if (FirstToken.getLength() > 0)
    End = End.getLocWithOffset(FirstToken.getLength() - 1);
  AddToken(tok::r_brace, End, 1, getReplacement());
}

void ClauseReplacer::HandleBody(ExternalPreprocessor &PP,
    PragmaIntroducerKind Introducer, Token &FirstToken) {
  LLVM_DEBUG(dbgs() << "[PRAGMA HANDLER]: process body of '" << getName() << "'\n");
  const auto Prototype = ClausePrototype::get(mClauseId);
  DefaultClauseVisitor<ExternalPreprocessor, ReplacementT> CV(PP,
                                                              getReplacement());
  CV.visitBody(Prototype.begin(), Prototype.end(), FirstToken);
}
}
