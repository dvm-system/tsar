//===- tsar_pragma.cpp - SAPFOR Specific Pragma Parser ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements SAPFOR specific #pragma handlers.
//
//===----------------------------------------------------------------------===//

#include "tsar_pragma.h"
#include "ClangUtils.h"
#include "ClauseVisitor.h"
#include <clang/AST/Expr.h>
#include <clang/Rewrite/Core/Rewriter.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "pragma-handler"

namespace {
std::pair<StringRef, CompoundStmt::body_iterator>
traversePragmaName(Stmt &S) {
  auto CS = dyn_cast<CompoundStmt>(&S);
  if (!CS)
    return std::make_pair(StringRef(), nullptr);
  auto CurrStmt = CS->body_begin();
  if (CurrStmt == CS->body_end())
    return std::make_pair(StringRef(), nullptr);
  auto Cast = dyn_cast<ImplicitCastExpr>(*CurrStmt);
  // In case of C there will be ImplicitCastExpr, however in case of C++ it
  // will be omitted.
  auto LiteralStmt = Cast ? *Cast->child_begin() : *CurrStmt;
  auto Literal = dyn_cast<clang::StringLiteral>(LiteralStmt);
  if (!Literal)
    return std::make_pair(StringRef(), nullptr);
  ++CurrStmt;
  return std::make_pair(Literal->getString(), CurrStmt);
}

inline void AddToken(tok::TokenKind K, SourceLocation Loc, unsigned Len,
    SmallVectorImpl<Token> &TokenList) {
  Token Tok;
  Tok.startToken();
  Tok.setKind(K);
  Tok.setLocation(Loc);
  Tok.setLength(Len);
  TokenList.push_back(Tok);
}

inline void AddStringToken(StringRef Str, SourceLocation Loc, Preprocessor &PP,
    SmallVectorImpl<Token> &TokenList) {
  Token Tok;
  Tok.startToken();
  Tok.setKind(tok::string_literal);
  PP.CreateString(("\"" + Str + "\"").str(), Tok, Loc, Loc);
  TokenList.push_back(Tok);
}

template<class ReplacementT>
class DefaultClauseVisitor :
  public ClauseVisitor<ReplacementT, DefaultClauseVisitor<ReplacementT>> {
  using BaseT = ClauseVisitor<ReplacementT, DefaultClauseVisitor<ReplacementT>>;
public:
  /// Creates visitor.
  DefaultClauseVisitor(clang::Preprocessor &PP, ReplacementT &Replacement) :
    BaseT(PP, Replacement) {}

  using BaseT::getReplacement;
  using BaseT::getLevelKind;

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
  /// something similar to `(void)(sizeof((void)(A)))` (for identifier `A`).
  void visitEK_Identifier(Token &Tok) {
    assert(Tok.is(tok::identifier) && "Token must be an identifier!");
    // Each identifier 'I' will be replace by (void)(sizeof((void)(I))).
    // This construction is necessary to disable warnings for unused expressions
    // (cast to void) and to disable generation of LLVM IR for it (sizeof).
    // Cast to void inside 'sizeof' operator is necessary in case of variable
    // length array:
    // int N;
    // double A[N];
    // (void)(sizeof(A)) // This produces LLVM IR which computes size in dynamic.
    // (void)(sizeof((void)(A))) // This does not produce LLVM IR.
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::kw_void, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::kw_sizeof, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::kw_void, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::l_paren, Tok.getLocation(), 1, getReplacement());
    getReplacement().push_back(Tok);
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::r_paren, Tok.getLocation(), 1, getReplacement());
    AddToken(tok::semi, Tok.getLocation(), 1, getReplacement());
  }
};
}

namespace tsar {
tok::TokenKind getTokenKind(ClauseExpr EK) noexcept {
  switch (EK) {
  default:
    llvm_unreachable("There is no appropriate token for clause expression!");
    return clang::tok::unknown;
#define KIND(EK, IsSignle, ClangTok) \
  case ClauseExpr::EK: return clang::tok::ClangTok;
#define GET_CLAUSE_EXPR_KINDS
#include "Directives.gen"
#undef GET_CLAUSE_EXPR_KINDS
#undef KIND
  }
}

Pragma::Pragma(Stmt &S) : mStmt(&S) {
  auto PragmaBody = traversePragmaName(S);
  if (!getTsarDirectiveNamespace(PragmaBody.first, mNamespaceId) ||
    PragmaBody.second == cast<CompoundStmt>(S).body_end())
    return;
  auto DirectiveBody = traversePragmaName(**PragmaBody.second);
  if (!getTsarDirective(mNamespaceId, DirectiveBody.first, mDirectiveId))
    return;
  mDirective = cast<CompoundStmt>(*PragmaBody.second);
  mClauseBegin = DirectiveBody.second;
}

Pragma::Clause Pragma::clause(clause_iterator I) {
  auto Tmp = traversePragmaName(**I);
  return Clause(Tmp.first, Tmp.second,
    Tmp.second ? cast<CompoundStmt>(*I)->body_end() : nullptr);
}

bool findClause(Pragma &P, ClauseId Id, SmallVectorImpl<Stmt *> &Clauses) {
  if (!P || P.getDirectiveId() != getParent(Id))
    return false;
  for (auto CI = P.clause_begin(), CE = P.clause_end(); CI != CE; ++CI) {
    ClauseId CId;
    if (!getTsarClause(P.getDirectiveId(), Pragma::clause(CI).getName(), CId))
      continue;
    if (CId == Id)
      Clauses.push_back(*CI);
  }
  return !Clauses.empty();
}

bool pragmaRangeToRemove(const Pragma &P, const SmallVectorImpl<Stmt *> &Clauses,
    const SourceManager &SM, const LangOptions &LangOpts,
    SmallVectorImpl<CharSourceRange> &ToRemove) {
  assert(P && "Pragma must be valid!");
  SourceLocation PStart = P.getNamespace()->getLocStart();
  SourceLocation PEnd = P.getNamespace()->getLocEnd();
  if (PStart.isInvalid())
    return false;
  if (PStart.isFileID()) {
    if (Clauses.size() == P.clause_size())
      ToRemove.push_back(
        CharSourceRange::getTokenRange({getStartOfLine(PStart, SM), PEnd}));
    else
      for (auto C : Clauses)
        ToRemove.push_back(CharSourceRange::getTokenRange(C->getSourceRange()));
    return true;
  }
  Token Tok;
  auto PStartExp = SM.getExpansionLoc(PStart);
  if (Lexer::getRawToken(PStartExp, Tok, SM, LangOpts) ||
      Tok.isNot(tok::raw_identifier) || Tok.getRawIdentifier() != "_Pragma")
    return false;
  if (Clauses.size() == P.clause_size()) {
    ToRemove.push_back(SM.getExpansionRange({ PStart, PEnd }));
  } else {
    // We obtain positions of pragma and clause in a scratch buffer and
    // calculates offset from the beginning of _Pragma(...) directive.
    // Then we add this offset to expansion location to obtain location of
    // a clause in the source code.
    auto PSpelling = SM.getSpellingLoc(PStart).getRawEncoding();
    for (auto C : Clauses) {
      auto CSpellingS = SM.getSpellingLoc(C->getLocStart()).getRawEncoding();
      auto CSpellingE = SM.getSpellingLoc(C->getLocEnd()).getRawEncoding();
      // Offset of clause start from `spf` in _Pragma("spf ...
      auto OffsetS = CSpellingS - PSpelling;
      // Offset of clause end from `spf` in _Pragma("spf ...
      auto OffsetE = CSpellingE - PSpelling;
      ToRemove.emplace_back(CharSourceRange::getTokenRange(
        { PStartExp.getLocWithOffset(OffsetS + 8),     // ' ' before clause name
          PStartExp.getLocWithOffset(OffsetE + 9) })); // end of clause
    }
  }
  return true;
}

llvm::StringRef getPragmaText(ClauseId Id, llvm::SmallVectorImpl<char> &Out,
    clang::PragmaIntroducerKind PIK) {
  assert(ClauseId::NotClause < Id && Id < ClauseId::NumClauses &&
    "Invalid identifier of a clause!");
  DirectiveId DID = getParent(Id);
  DirectiveNamespaceId NID = getParent(DID);
  llvm::raw_svector_ostream OS(Out);
  switch (PIK) {
  case PIK_HashPragma: OS << "#pragma "; break;
  case PIK__Pragma: OS << "_Pragma(\""; break;
  case PIK___pragma: OS << "__pragma(\""; break;
  }
  OS << getName(NID) << " " << getName(DID) << " " << getName(Id);
  switch (PIK) {
  case PIK__Pragma: case PIK___pragma: OS << "\")"; break;
  }
  OS << "\n";
  return StringRef(Out.data(), Out.size());
}

void PragmaNamespaceReplacer::HandlePragma(
  Preprocessor &PP, PragmaIntroducerKind Introducer, Token &FirstToken) {
  mTokenQueue.clear();
  auto NamespaceLoc = FirstToken.getLocation();
  PP.LexUnexpandedToken(FirstToken);
  StringRef DirectiveName = FirstToken.getIdentifierInfo()->getName();
  if (FirstToken.is(tok::identifier)) {
    DirectiveName = FirstToken.getIdentifierInfo()->getName();
  } else if (auto *KW = tok::getKeywordSpelling(FirstToken.getKind())) {
    DirectiveName = KW;
  } else {
    PP.Diag(FirstToken, diag::err_expected) << "name of directive";
    return;
  }
  DirectiveId Id;
  if (!getTsarDirective(mNamespaceId, DirectiveName, Id)) {
    toDiag(PP.getDiagnostics(), FirstToken.getLocation(),
      diag::err_unknown_directive) << getName() << DirectiveName;
    return;
  }
  PragmaHandler *Handler = FindHandler(DirectiveName, false);
  if (!Handler) {
    PP.Diag(FirstToken, diag::warn_pragma_ignored);
    return;
  }
  AddToken(tok::l_brace, NamespaceLoc, 1, mTokenQueue);
  AddStringToken(getName(), NamespaceLoc, PP, mTokenQueue);
  AddToken(tok::semi, NamespaceLoc, 1, mTokenQueue);
  Handler->HandlePragma(PP, Introducer, FirstToken);
  // Replace pragma only if all tokens have been processed.
  if (FirstToken.is(tok::eod)) {
    AddToken(tok::r_brace, FirstToken.getLocation(), 1, mTokenQueue);
    PP.EnterTokenStream(mTokenQueue, false);
  } else {
    // It seems that call of `PP.CommitBacktrackedTokens()` in clause handlers
    // prevents preprocessor from calling of DiscardUntilEndOfDirective().
    // So, in case of errors in pragma syntax the lexer will read tokens
    // inside pragma instead of tokens after tok::eod. Hence, we manually
    // discards all tokens until the end of directive.
    do {
      PP.LexUnexpandedToken(FirstToken);
      assert(FirstToken.isNot(tok::eof) &&
        "EOF seen while discarding directive tokens");
    } while (FirstToken.isNot(tok::eod));
  }
}

void PragmaReplacer::HandlePragma(
    Preprocessor &PP, PragmaIntroducerKind Introducer, Token &FirstToken) {
  assert(mParent && "Parent handler must not be null!");
  auto DirectiveLoc = FirstToken.getLocation();
  AddToken(tok::l_brace, DirectiveLoc, 1, getReplacement());
  AddStringToken(getName(), DirectiveLoc, PP, getReplacement());
  AddToken(tok::semi, DirectiveLoc, 1, getReplacement());
  PP.LexUnexpandedToken(FirstToken);
  while (FirstToken.isNot(tok::eod)) {
    StringRef ClauseName;
    if (FirstToken.is(tok::identifier)) {
      ClauseName = FirstToken.getIdentifierInfo()->getName();
    } else if (auto *KW = tok::getKeywordSpelling(FirstToken.getKind())) {
      ClauseName = KW;
    } else {
      PP.Diag(FirstToken, diag::err_expected) << "name of clause";
      return;
    }
    ClauseId Id;
    if (!getTsarClause(mDirectiveId, ClauseName, Id)) {
      toDiag(PP.getDiagnostics(), FirstToken.getLocation(),
        diag::err_unknown_clause) << getName() << ClauseName;
      return;
    }
    auto *ClauseHandler = FindHandler(ClauseName, false);
    if (!ClauseHandler) {
      PP.Diag(FirstToken, diag::warn_pragma_ignored);
      return;
    }
    ClauseHandler->HandlePragma(PP, Introducer, FirstToken);
    assert(!PP.isBacktrackEnabled() &&
      "Did you forget to call CommitBacktrackedTokens() or Backtrack()?");
    PP.LexUnexpandedToken(FirstToken);
  }
  AddToken(tok::r_brace, FirstToken.getLocation(), 1, getReplacement());
}

void ClauseReplacer::HandlePragma(
    Preprocessor &PP, PragmaIntroducerKind Introducer, Token &FirstToken) {
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

void ClauseReplacer::HandleBody(
    Preprocessor &PP, PragmaIntroducerKind Introducer, Token &FirstToken) {
  LLVM_DEBUG(dbgs() << "[PRAGMA HANDLER]: process body of '" << getName() << "'\n");
  const auto Prototype = ClausePrototype::get(mClauseId);
  DefaultClauseVisitor<ReplacementT> CV(PP, getReplacement());
  CV.visitBody(Prototype.begin(), Prototype.end(), FirstToken);
}
}
