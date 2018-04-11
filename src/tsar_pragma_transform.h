//===-- tsar_pragma_transform.h - SAPFOR Specific Pragma Parser -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines SAPFOR specific #pragma transform handlers.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRAGMA_TRANSFORM_H
#define TSAR_PRAGMA_TRANSFORM_H

#include <clang/AST/Stmt.h>
#include <clang/Lex/Pragma.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/raw_ostream.h>
#include <set>

namespace clang {
class Preprocessor;

class SPFPragmaHandler : public PragmaHandler {
protected:
  /// This set contains locations of each handled pragma.
  typedef std::set<unsigned> PragmaLocSet;
  PragmaLocSet mPragmaLocSet;
public:
  explicit SPFPragmaHandler(llvm::StringRef name) : PragmaHandler(name) {}

  /// Returns true if a specified statement represent a #pragma spf ...
  bool isPragma(clang::SourceLocation Loc) const {
    //assert(S && "Statement must not be null!");
    // seems that inserted tokens start at -2 position
    // #pragma and space(s) tokens parsed as two 1-char tokens
    return Loc.isValid() && mPragmaLocSet.count(Loc.getRawEncoding() + 2) > 0;
  }
};

/// This preliminary evaluates #pragma spf transform ... , note that
/// subsequent evaluation should be performed after generation of AST.
class TransformPragmaHandler : public SPFPragmaHandler {
  std::set<std::string> mClauses = {
    "inline"
  };
public:
  /// Creates handler.
  TransformPragmaHandler() : SPFPragmaHandler("transform") {}

  /// \brief Handles a #pragma sapfor ... .
  /// similar to #pragma spf analysis handler
  /// should be generalized for all clauses and its formats
  void HandlePragma(Preprocessor& PP,
    PragmaIntroducerKind Introducer, Token& FirstToken) override;

private:
  /// Inserts new token in the list of tokens.
  void AddToken(tok::TokenKind K, SourceLocation Loc, unsigned Len);

  /// Inserts an identifier in the list of tokens as a string literal.
  void AddClauseName(Preprocessor& PP, Token& Tok);

  /// Assumes that a current token is a clause name, otherwise produces
  /// an error diagnostic.
  bool ConsumeClause(Preprocessor& PP, Token& Tok);

  llvm::SmallVector<Token, 32> mTokenList;
};

}

#endif//TSAR_PRAGMA_TRANSFORM_H
