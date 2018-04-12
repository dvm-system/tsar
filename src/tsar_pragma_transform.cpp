//===- tsar_pragma.cpp - SAPFOR Specific Pragma Parser ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements SAPFOR specific #pragma handlers.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/Decl.h>
#include <clang/AST/Stmt.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Instructions.h>
#include "tsar_pragma_transform.h"

using namespace llvm;

namespace clang {

void TransformPragmaHandler::HandlePragma(
  Preprocessor& PP, PragmaIntroducerKind, Token& FirstToken) {
  mTokenList.clear();
  AddToken(tok::l_brace, FirstToken.getLocation(), 1);
  mPragmaLocSet.insert(FirstToken.getLocation().getRawEncoding());
  Token Tok;
  PP.LexNonComment(Tok);
  do {
    if (!ConsumeClause(PP, Tok))
      return;
  } while (Tok.isNot(tok::eod));
  AddToken(tok::r_brace, Tok.getLocation(), 1);
#if (LLVM_VERSION_MAJOR < 4)
  Token *TokenArray = new Token[mTokenList.size()];
  std::copy(mTokenList.begin(), mTokenList.end(), TokenArray);
  PP.EnterTokenStream(TokenArray, mTOkenList.size(), false, true);
#else
  PP.EnterTokenStream(mTokenList, false);
#endif
}

inline void TransformPragmaHandler::AddToken(
  tok::TokenKind K, SourceLocation Loc, unsigned Len) {
  Token Tok;
  Tok.startToken();
  Tok.setKind(K);
  Tok.setLocation(Loc);
  Tok.setLength(Len);
  mTokenList.push_back(Tok);
}

void TransformPragmaHandler::AddClauseName(Preprocessor &PP, Token &Tok) {
  //assert(Tok.is(tok::identifier) && "Token must be an identifier!");
  Token ClauseTok;
  ClauseTok.startToken();
  ClauseTok.setKind(tok::string_literal);
  SmallString<16> Name;
  raw_svector_ostream OS(Name);
  OS << '\"' << Tok.getIdentifierInfo()->getName() << '\"';
  PP.CreateString(
    OS.str(), ClauseTok, Tok.getLocation(), Tok.getLocation());
  mTokenList.push_back(ClauseTok);
}

bool TransformPragmaHandler::ConsumeClause(Preprocessor &PP, Token &Tok) {
  // now can use reserved identifiers
  /*if (Tok.isNot(tok::identifier)) {
    PP.Diag(Tok.getLocation(), diag::err_expected) << "clause name";
    return false;
  }*/
  if (mClauses.find(Tok.getIdentifierInfo()->getName()) == std::end(mClauses)) {
    unsigned DiagId = PP.getDiagnostics().getCustomDiagID(
      DiagnosticsEngine::Error, "unknown clause '%0'");
    PP.Diag(Tok.getLocation(), DiagId) << Tok.getIdentifierInfo()->getName();
    return false;
  }
  AddClauseName(PP, Tok);
  AddToken(tok::semi, Tok.getLocation(), 1);
  PP.LexNonComment(Tok);
  return true;
}

}
