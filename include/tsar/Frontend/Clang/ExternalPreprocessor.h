//===- ExternalPreprocessor.h - External Preprocessor -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements ExternalPreprocessor which is similar to preprocessor
// but allows us reprocess already lexed tokens in a simple way. Note, that
// since a LLVM 9.0.0 it is not possible to enable backtracking inside a call of
// the Preprocessor::Lex().
//===----------------------------------------------------------------------===//

#ifndef TSAR_EXTERNAL_PREPROCESSOR_H
#define TSAR_EXTERNAL_PREPROCESSOR_H

#include <clang/Basic/Diagnostic.h>
#include <llvm/ADT/ArrayRef.h>
#include <clang/Lex/Preprocessor.h>

namespace tsar {
/// This is similar to preprocessor but allows us reprocess already lexed tokens
/// in a simple way. Note, that since a LLVM 9.0.0 it is not possible to enable
/// backtracking inside a call of the Preprocessor::Lex().
class ExternalPreprocessor {
  using TokenStream = llvm::SmallVector<clang::Token, 64>;
public:
  ExternalPreprocessor(clang::Preprocessor &PP,
      llvm::ArrayRef<clang::Token> Toks)
    : mPP(&PP)
    , mTokens(Toks.begin(), Toks.end())
    , mNextPtr(mTokens.begin())
    , mEndOfStream(mTokens.end()) {}

  /// Lex the next token for this preprocessor.
  bool Lex(clang::Token &Tok) {
    if (mNextPtr == mEndOfStream)
      return false;
    Tok = *mNextPtr;
    ++mNextPtr;
    return true;
  }

  /// Insert tokens at the current position and start lexing tokens from
  /// the first inserted tokens. The current position is relexing when
  /// processing of knew tokens is finished.
  void EnterTokenStream(llvm::ArrayRef<clang::Token> Toks) {
    if (mNextPtr == mTokens.begin())
      mNextPtr = mTokens.insert(mNextPtr, Toks.begin(), Toks.end());
    else
      mNextPtr = mTokens.insert(mNextPtr - 1, Toks.begin(), Toks.end());
    mEndOfStream = mTokens.end();
  }

  /// True if EnableBacktrackAtThisPos() was called and
  /// caching of tokens is on.
  bool isBacktrackEnabled() const { return !mBacktrackPositions.empty(); }

  void EnableBacktrackAtThisPos() {
    mBacktrackPositions.push_back(mNextPtr);
  }

  void CommitBacktrackedTokens() {
    assert(!mBacktrackPositions.empty() &&
           "EnableBacktrackAtThisPos was not called!");
    mBacktrackPositions.pop_back();
  }

  /// Make Preprocessor re-lex the tokens that were lexed since
  /// EnableBacktrackAtThisPos() was previously called.
  void Backtrack() {
    assert(!mBacktrackPositions.empty() &&
           "EnableBacktrackAtThisPos was not called!");
    mNextPtr = mBacktrackPositions.pop_back_val();
  }

  /// When backtracking is enabled and tokens are cached,
  /// this allows to revert a specific number of tokens.
  ///
  /// Note that the number of tokens being reverted should be up to the last
  /// backtrack position, not more.
  void RevertCachedTokens(unsigned N) {
    assert(isBacktrackEnabled() &&
           "Should only be called when tokens are cached for backtracking!");
    assert(unsigned(mNextPtr - mBacktrackPositions.back()) >= N
         && "Should revert tokens up to the last backtrack position, not more!");
    mNextPtr -= N;
  }

  /// CreateString - Plop the specified string into a scratch buffer and return a
  /// location for it.  If specified, the source location provides a source
  /// location for the token.
  void CreateString(llvm::StringRef Str, clang::Token &Tok,
                    clang::SourceLocation ExpansionLocStart,
                    clang::SourceLocation ExpansionLocEnd) {
    mPP->CreateString(Str, Tok, ExpansionLocStart, ExpansionLocStart);
  }

  clang::DiagnosticBuilder Diag(clang::SourceLocation Loc,
      unsigned DiagID) const {
    return mPP->Diag(Loc, DiagID);
  }

  clang::DiagnosticBuilder Diag(const clang::Token &Tok,
      unsigned DiagID) const {
    return mPP->Diag(Tok.getLocation(), DiagID);
  }

  clang::DiagnosticsEngine &getDiagnostics() const {
    return mPP->getDiagnostics();
  }
  const clang::LangOptions &getLangOpts() const { return mPP->getLangOpts(); }
private:
  clang::Preprocessor *mPP;
  TokenStream mTokens;
  clang::Token *mNextPtr;
  clang::Token *mEndOfStream;
  llvm::SmallVector<clang::Token *, 8> mBacktrackPositions;
};
}
#endif//TSAR_EXTERNAL_PREPROCESSOR_H
