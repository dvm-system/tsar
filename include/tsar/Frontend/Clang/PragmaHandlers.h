//===-- PrgmaHandlers.h -------- Pragma Handlers ----------------*- C++ -*-===//
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

#ifndef TSAR_PRAGMA_HANDLERS_H
#define TSAR_PRAGMA_HANDLERS_H

#include "tsar/Support/Directives.h"
#include <clang/Lex/Pragma.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>

namespace tsar {
class ExternalPreprocessor;
class PragmaNamespaceReplacer;
class PragmaReplacer;
class ClauseReplacer;

/// Base class to represent hierarchy of replacers.
template<class T> class PragmaReplacerNode {
public:
  bool IsEmpty() const { return mHandlers.empty(); }

  /// Check to see if there is already a handler for the specified name.
  ///
  ///  If not, return the handler for the null identifier if it
  /// exists, otherwise return null.  If IgnoreNull is true (the default) then
  /// the null handler isn't returned on failure to match.
  T *FindHandler(llvm::StringRef Name, bool IgnoreNull) const {
    auto Itr = mHandlers.find(Name);
    if (Itr != mHandlers.end())
      return Itr->getValue().get();
    if (!IgnoreNull)
      Itr = mHandlers.find(llvm::StringRef());
    return Itr == mHandlers.end() ? nullptr : Itr->getValue().get();
  }

  void AddPragma(T *Handler) {
    assert(Handler && "Handler must not be null!");
    assert(!mHandlers.count(Handler->getName()) &&
           "A handler with this name is already registered in this namespace");
    mHandlers[Handler->getName()].reset(Handler);
  }

  void RemovePragmaHandler(T *Handler) {
    assert(Handler && "Handler must not be null!");
    assert(mHandlers.count(Handler->getName()) &&
           "Handler not registered in this namespace");
    mHandlers.erase(Handler->getName());
  }
private:
  llvm::StringMap<std::unique_ptr<T>> mHandlers;
};


/// \brief Replaces clause with a sequence of tokens.
///
/// A parent handler PragmaReplacer calls this handler.
class ClauseReplacer {
public:
  using ReplacementT = llvm::SmallVector<clang::Token, 32>;

  /// Creates handler for a specified clause `Id` from a specified
  /// directive`Parent`.
  ClauseReplacer(ClauseId Id, PragmaReplacer &Parent);

  llvm::StringRef getName() const { return mName; }

  /// \brief Handles a clause.
  ///
  /// This checks syntax of a clause and converts it to a sequence of tokens
  /// which will be parsed and evaluated later. For example,
  /// `... induction(I) ...` will be converted to
  /// `{ "induction"; (void)(sizeof((void)(I))); }`.
  void HandleClause(ExternalPreprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstToken);

  /// Returns clause ID being processed.
  ClauseId getClauseId() const noexcept { return mClauseId; }

  /// Returns parent directive ID being processed.
  DirectiveId getDirectiveId() const noexcept;

  /// Returns parent namespace ID being processed.
  DirectiveNamespaceId getNamespaceId() const noexcept;

  /// Returns common replacement for all handlers in hierarchy.
  ReplacementT &getReplacement() noexcept;

  /// Returns common replacement for all handlers in hierarchy.
  const ReplacementT &getReplacement() const noexcept;

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
  virtual void HandleBody(ExternalPreprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstTok);

private:
  std::string mName;
  ClauseId mClauseId;
  PragmaReplacer *mParent;
};

/// \brief Replaces directive names with string literal.
///
/// A parent handler PragmaNamespaceReplacer calls this handler.
class PragmaReplacer : public PragmaReplacerNode<ClauseReplacer> {
public:
  using ReplacementT = ClauseReplacer::ReplacementT;

  /// Creates handler for a specified directive `Id` from a specified
  /// namespace `Parent`.
  PragmaReplacer(DirectiveId Id, PragmaNamespaceReplacer &Parent);

  llvm::StringRef getName() const { return mName; }

  /// Handles directive name and forwards processing of subsequent tokens
  /// to child handlers of clauses.
  ///
  /// \post `FirstToken` is a some token inside pragma or tok::eod.
  void HandlePragma(ExternalPreprocessor &PP,
    clang::PragmaIntroducerKind Introducer, clang::Token &FirstToken);

  /// Returns directive ID being processed.
  DirectiveId getDirectiveId() const noexcept { return mDirectiveId; }

  /// Returns parent namespace ID being processed.
  DirectiveNamespaceId getNamespaceId() const noexcept;

  /// Returns common replacement for all handlers in hierarchy.
  ReplacementT &getReplacement() noexcept;

  /// Returns common replacement for all handlers in hierarchy.
  const ReplacementT &getReplacement() const noexcept;

  /// Returns parent handler.
  PragmaNamespaceReplacer & getParent() noexcept { return *mParent; }

  /// Returns parent handler.
  const PragmaNamespaceReplacer & getParent() const noexcept {return *mParent;}
private:
  std::string mName;
  PragmaNamespaceReplacer *mParent;
  DirectiveId mDirectiveId;
};

/// This handler replaces directive namespace with string literal.
///
/// This handler replaces name of `#pragma <name> ...` with `{ "<name>"; ... }`
/// and forwards processing to children. So,  this class allows hierarchical
/// pragmas to be defined. Child handlers may append tokens to a token
/// queue. This queue is called replacement and this handler writes it to the
/// token stream after the last pragma token (tok::eod). Note, that children
/// should evaluate all tokens in pragma. Otherwise, replacement will not be
/// written to the token stream.
class PragmaNamespaceReplacer :
  public clang::PragmaHandler, public PragmaReplacerNode<PragmaReplacer> {
public:
  using ReplacementT = ClauseReplacer::ReplacementT;

  /// Creates handler for a specified namespace.
  explicit PragmaNamespaceReplacer(DirectiveNamespaceId Id) :
    clang::PragmaHandler(tsar::getName(Id)), mNamespaceId(Id) {}

  /// Handles namespace name and forwards processing of subsequent tokens
  /// to child handlers of directives.
  void HandlePragma(clang::Preprocessor &PP,
    clang::PragmaIntroducer Introducer, clang::Token &FirstToken) override;

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

inline DirectiveNamespaceId PragmaReplacer::getNamespaceId() const noexcept {
  return getParent().getNamespaceId();
}

inline PragmaReplacer::ReplacementT &PragmaReplacer::getReplacement() noexcept {
  return mParent->getReplacement();
}

inline const PragmaReplacer::ReplacementT &
PragmaReplacer::getReplacement() const noexcept {
  return mParent->getReplacement();
}


inline DirectiveId ClauseReplacer::getDirectiveId() const noexcept {
  return getParent().getDirectiveId();
}

inline DirectiveNamespaceId ClauseReplacer::getNamespaceId() const noexcept {
  return getParent().getNamespaceId();
}

inline ClauseReplacer::ReplacementT &ClauseReplacer::getReplacement() noexcept {
  return mParent->getReplacement();
}

inline const ClauseReplacer::ReplacementT &
ClauseReplacer::getReplacement() const noexcept {
  return mParent->getReplacement();
}
}
#endif//TSAR_PRAGMA_HANDLERS_H
