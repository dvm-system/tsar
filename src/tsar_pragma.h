//===--- tsar_pragma.h - SAPFOR Specific Pragma Parser ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines SAPFOR specific #pragma handlers. These handlers
// check syntax of a pragma and converts it to a sequence of tokens
// which can be parsed and evaluated later. For example,
// `#pragma spf induction(I)` will be converted to
// `{ "spf"; { "analysis"; { "induction"; (void)(sizeof((void)(I))); }}}`.
// This sequence of tokens is going to be represented as a compound statement
// in AST. However, it will not be placed in LLVM IR.
// The Pragam class allows to check whether some compound statement represents
// a pragma.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRAGMA_H
#define TSAR_PRAGMA_H

#include "Directives.h"
#include <clang/AST/Stmt.h>
#include <clang/Lex/Pragma.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/SmallVector.h>

namespace tsar {
/// Description of a directive.
class Pragma {
public:
  using clause_iterator = clang::CompoundStmt::body_iterator;
  using clause_range = clang::CompoundStmt::body_range;

  class Clause : public clang::CompoundStmt::body_range {
  public:
    using body_iterator = clang::CompoundStmt::body_iterator;
    Clause(llvm::StringRef Name, body_iterator Begin, body_iterator End) :
      mName(Name), clang::CompoundStmt::body_range(Begin, End) {}
    operator bool() const { return !mName.empty(); }
    llvm::StringRef getName() const noexcept { return mName; }
  private:
    llvm::StringRef mName;
  };

  /// Returns description of a specified clause.
  static Clause clause(clause_iterator I);

  /// Creates description of a prgama if `S` represents known directive.
  explicit Pragma(clang::Stmt &S);

  /// Returns true if a specified statement (in constructor) represent a
  /// known directive.
  operator bool() const noexcept {
    return mDirectiveId != DirectiveId::NotDirective;
  }

  clang::Stmt * getNamespace() const noexcept { return mStmt; }
  clang::Stmt * getDirective() const noexcept { return mDirective; }

  DirectiveNamespaceId getNamespaceId() const noexcept { return mNamespaceId; }
  DirectiveId getDirectiveId() const noexcept { return mDirectiveId; }

  /// Returns true if there is no clauses in a pragma.
  bool clause_empty() const noexcept { return clause_size() == 0; }

  /// Returns number of clauses.
  unsigned clause_size() const {
    assert(*this && "This must be a pragma");
    return mDirective->size() - 1;
  }

  /// Returns clauses.
  clause_range clause() {
    assert(*this && "This must be a pragma");
    return clause_range(mClauseBegin, mDirective->body_end());
  }
  clause_iterator clause_begin() { return mClauseBegin; }
  clause_iterator clause_end() { return mDirective->body_end(); }

private:
  DirectiveNamespaceId mNamespaceId = DirectiveNamespaceId::NotNamespace;
  DirectiveId mDirectiveId = DirectiveId::NotDirective;
  clang::Stmt *mStmt = nullptr;
  clang::CompoundStmt *mDirective = nullptr;
  clause_iterator mClauseBegin;
};

/// \brief Replaces directive namespace with string literal.
///
/// This handler replaces name of `#pragma <name> ...` with `{ "<name>"; ... }`
/// and forwards processing to children. So,  this class allows hierarchical
/// pragmas to be defined. Child handlers may append tokens to a token
/// queue. This queue is called replacement and this handler writes it to the
/// token stream after the last pragma token (tok::eod). Note, that children
/// should evaluate all tokens in pragma. Otherwise, replacement will not be
/// written to the token stream.
class PragmaNamespaceReplacer : public clang::PragmaNamespace {
public:
  using ReplacementT = llvm::SmallVector<clang::Token, 32>;

  /// Creates handler for a specified namespace.
  explicit PragmaNamespaceReplacer(DirectiveNamespaceId Id) :
    clang::PragmaNamespace(tsar::getName(Id)), mNamespaceId(Id) {}

  /// Handles namespace name and forwards processing of subsequent tokens
  /// to child handlers of directives.
  void HandlePragma(clang::Preprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstToken) override;

  /// Returns common replacement for all handlers in hierarchy.
  ReplacementT & getReplacement() noexcept { return mTokenQueue; }

  /// Returns common replacement for all handlers in hierarchy.
  const ReplacementT & getReplacement() const noexcept { return mTokenQueue; }

  /// Returns ID of namespace being processed.
  DirectiveNamespaceId getNamespaceId() const noexcept { return mNamespaceId; }

protected:
  ReplacementT mTokenQueue;
  DirectiveNamespaceId mNamespaceId;
};

/// \brief Replaces directive names with string literal.
///
/// A parent handler PragmaNamespaceReplacer calls this handler.
class PragmaReplacer : public clang::PragmaNamespace {
public:
  using ReplacementT = PragmaNamespaceReplacer::ReplacementT;

  /// Creates handler for a specified directive `Id` from a specified
  /// namespace `Parent`.
  PragmaReplacer(DirectiveId Id, PragmaNamespaceReplacer &Parent) :
      clang::PragmaNamespace(tsar::getName(Id)),
      mDirectiveId(Id), mParent(&Parent) {
    assert(tsar::getParent(Id) == Parent.getNamespaceId() &&
      "Incompatible namespace and directive IDs!");
  }

  /// Handles directive name and forwards processing of subsequent tokens
  /// to child handlers of clauses.
  ///
  /// \post `FirstToken` is a some token inside pragma or tok::eod.
  void HandlePragma(clang::Preprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstToken) override;

  /// Returns `nullptr` because this is not a namespace handler.
  clang::PragmaNamespace * getIfNamespace() override { return nullptr; }

  /// Returns directive ID being processed.
  DirectiveId getDirectiveId() const noexcept { return mDirectiveId; }

  /// Returns parent namespace ID being processed.
  DirectiveNamespaceId getNamespaceId() const noexcept {
    return getParent().getNamespaceId();
  }

  /// Returns common replacement for all handlers in hierarchy.
  ReplacementT & getReplacement() noexcept { return mParent->getReplacement(); }

  /// Returns common replacement for all handlers in hierarchy.
  const ReplacementT & getReplacement() const noexcept {
    return mParent->getReplacement(); }

  /// Returns parent handler.
  PragmaNamespaceReplacer & getParent() noexcept { return *mParent; }

  /// Returns parent handler.
  const PragmaNamespaceReplacer & getParent() const noexcept {return *mParent;}
private:
  PragmaNamespaceReplacer *mParent;
  DirectiveId mDirectiveId;
};

/// \brief Replaces clause with a sequence of tokens.
///
/// A parent handler PragmaReplacer calls this handler.
class ClauseReplacer: public clang::PragmaHandler{
public:
  using ReplacementT = PragmaReplacer::ReplacementT;

  /// Creates handler for a specified clause `Id` from a specified
  /// directive`Parent`.
  ClauseReplacer(ClauseId Id, PragmaReplacer &Parent) :
      clang::PragmaHandler(tsar::getName(Id)), mClauseId(Id), mParent(&Parent) {
    assert(tsar::getParent(Id) == Parent.getDirectiveId() &&
      "Incompatible directive and clause IDs!");
  }

  /// \brief Handles a clause.
  ///
  /// This checks syntax of a clause and converts it to a sequence of tokens
  /// which will be parsed and evaluated later. For example,
  /// `... induction(I) ...` will be converted to
  /// `{ "induction"; (void)(sizeof((void)(I))); }`.
  void HandlePragma(clang::Preprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstToken) override;

  /// Returns clause ID being processed.
  ClauseId getClauseId() const noexcept { return mClauseId; }

  /// Returns parent directive ID being processed.
  DirectiveId getDirectiveId() const noexcept {
    return getParent().getDirectiveId();
  }

  /// Returns parent namespace ID being processed.
  DirectiveNamespaceId getNamespaceId() const noexcept {
    return getParent().getNamespaceId();
  }

  /// Returns common replacement for all handlers in hierarchy.
  ReplacementT & getReplacement() noexcept { return mParent->getReplacement(); }

  /// Returns common replacement for all handlers in hierarchy.
  const ReplacementT & getReplacement() const noexcept {
    return mParent->getReplacement(); }

  /// Returns parent handler.
  PragmaReplacer & getParent() noexcept { return *mParent; }

  /// Returns parent handler.
  const PragmaReplacer & getParent() const noexcept { return *mParent; }

protected:
  /// \brief Handles body of the clause.
  ///
  /// For example, in case of `... inducition(I) ...`  the body will be `(I)`.
  /// \pre `FirstTok` is a clause name, for the mentioned example,
  /// `FirstTok` will be name of clause `induction`.
  /// \post
  /// - Lexer points to the last successfully processed token.
  /// - On success, `FirstTok` is a last token in clause body.
  virtual void HandleBody(clang::Preprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstTok);

private:
  ClauseId mClauseId;
  PragmaReplacer *mParent;
};
}
#endif//TSAR_PRAGMA_H
