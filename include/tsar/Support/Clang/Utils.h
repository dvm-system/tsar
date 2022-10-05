//===--- Utils.h -------- Utilities To Examine Clang AST  -------*- C++ -*-===//
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
// This file provides utilities to examine Clang AST.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_UTILS_H
#define TSAR_CLANG_UTILS_H

#include "tsar/Support/Directives.h"
#include "tsar/Support/RewriterBase.h"
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/Error.h>
#include <vector>

namespace llvm {
template <typename PtrType> class SmallPtrSetImpl;
}

namespace clang {
class CFG;
class DiagnosticEngine;
class CFGBlock;
class FunctionDecl;
class LangOptions;
class MemoryBuffer;
class SourceManager;
class Stmt;
}

namespace tsar {
class OutputFile;

/// Returns kind of Clang token for a specified clause expression
// or tok::unknown.
clang::tok::TokenKind getTokenKind(ClauseExpr EK) noexcept;

/// Finds unreachable basic blocks for a specified CFG.
void unreachableBlocks(clang::CFG &Cfg,
  llvm::SmallPtrSetImpl<clang::CFGBlock *> &Blocks);

/// Relex tokens in a specified range.
class LocalLexer {
public:
  using LexedTokens = std::vector<clang::Token>;

  /// Initializes lexer to relex tokens in a specified range `SR`.
  LocalLexer(clang::SourceRange SR,
    const clang::SourceManager &SM, const clang::LangOptions &LangOpts);

  /// Initializes lexer to relex tokens in a specified range `SR`.
  LocalLexer(clang::CharSourceRange SR,
    const clang::SourceManager &SM, const clang::LangOptions &LangOpts);

  /// Lex a token, returns `false` on success and `true` otherwise
  bool LexFromRawLexer(clang::Token &Tok);

  const clang::SourceManager & getSourceManager() const noexcept { return mSM; }
  const clang::LangOptions & getLangOpts() const noexcept { return mLangOpts; }
  clang::CharSourceRange getSourceRange() const { return mSR; }

  /// Returns list of already lexed tokens.
  const LexedTokens & getLexedTokens() const noexcept { return mTokens; }

private:
  clang::CharSourceRange mSR;
  const clang::LangOptions &mLangOpts;
  const clang::SourceManager &mSM;
  std::size_t mLength;
  unsigned mCurrentPos;
  LexedTokens mTokens;
};

/// Returns list of clang::tok::raw_identifier tokens inside a specified range.
std::vector<clang::Token> getRawIdentifiers(clang::SourceRange SR,
  const clang::SourceManager &SM, const clang::LangOptions &LangOpts);

/// \brief Searches #define and #include directives in a specified file
/// and collects all identifiers.
///
/// The results are two maps from a macro/file name to the location of
/// this name in an appropriate #define/#include directive and a set of all
/// lexed identifiers.
///
/// Note, that if there are several macro definitions with the same name
/// (or includes of the same file), then only the first one will be remembered.
void getRawMacrosAndIncludes(
  clang::FileID FID, const llvm::MemoryBufferRef &InputBuffer,
  const clang::SourceManager &SM, const clang::LangOptions &LangOpts,
  llvm::StringMap<clang::SourceLocation> &Macros,
  llvm::StringMap<clang::SourceLocation> &Includes,
  llvm::StringSet<> &Ids);

/// Relex token after the token at a specified location, returns true in case
/// of errors.
bool getRawTokenAfter(clang::SourceLocation Loc, const clang::SourceManager &SM,
  const clang::LangOptions &LangOpts, clang::Token &Tok);

struct ClangExternalRewriterInfo {
  using SourceLocationT = clang::SourceLocation;
  using SourceRangeT = clang::SourceRange;
};

/// This is similar to clang::Rewriter, however this class enables to rewrite
/// some copy of input buffer.
class ExternalRewriter :
  public RewriterBase<ExternalRewriter, ClangExternalRewriterInfo> {
public:
  /// Creates rewriter to update copy of source text in a specified range.
  ExternalRewriter(clang::SourceRange SR, const clang::SourceManager &SM,
    const clang::LangOptions &LangOpts);

  /// Replaces a range of characters in the buffer with a new string, return
  /// `false` on success and `true` in case of errors.
  bool ReplaceText(clang::SourceRange SR, clang::StringRef NewStr);

  /// Inserts a specified string at a specified location in the buffer,
  /// return `false` on success and `true` in case of errors.
  bool InsertText(clang::SourceLocation Loc, clang::StringRef NewStr,
    bool InsertAfter = true);

  /// Insert the specified string after the token in the specified location.
  bool InsertTextAfterToken(clang::SourceLocation Loc, clang::StringRef NewStr);

  /// \brief Removes a rang of characters in the buffer, return `false`
  /// on success.
  ///
  /// If `RemoveLineIfEmpty` is `true` and removing of a specified range leads
  /// to an empty line, then this line will be removed.
  bool RemoveText(clang::SourceRange SR, bool RemoveLineIfEmpty = false);

  /// \brief Removes a rang of characters in the buffer, return `false`
  /// on success.
  ///
  /// If `RemoveLineIfEmpty` is `true` and removing of a specified range leads
  /// to an empty line, then this line will be removed.
  bool RemoveText(clang::CharSourceRange SR, bool RemoveLineIfEmpty = false);

  /// \brief Returns the rewritten form of the text in the specified range
  /// inside the initial one (getSourceRange()).
  ///
  //  If the start of a specified range and the star of the initial range are
  /// in different buffers, this returns an empty string.
  clang::StringRef getRewrittenText(clang::SourceRange SR);

  /// Returns initial range which is rewritten by this rewriter.
  clang::SourceRange getSourceRange() const { return mSR; }

  const clang::SourceManager & getSourceMgr() const noexcept { return mSM; }
  const clang::LangOptions & getLangOpts() const noexcept { return mLangOpts; }

private:
  using RewriterBaseImpl::getRewrittenText;
  using RewriterBaseImpl::InsertText;
  using RewriterBaseImpl::ReplaceText;

  unsigned ComputeOrigOffset(clang::SourceLocation Loc) const {
    unsigned Base = mSR.getBegin().getRawEncoding();
    unsigned OrigBegin = Loc.getRawEncoding() - Base;
    return OrigBegin;
  }

  clang::SourceRange mSR;
  const clang::SourceManager &mSM;
  const clang::LangOptions &mLangOpts;
};

  /// \brief Constructs correct language declaration of a specified
  /// identifier `Id` with a specified type `Type`.
  ///
  /// Uses bruteforce with linear complexity dependent on number of tokens
  /// in `Type` where token is non-whitespace character or special sequence.
  /// \param [in] Context This is a string containing declarations used in case
  /// of referencing in `Type`.
  /// \param [in] Replacements Map from tokens to their new values. The tokens
  /// will be replaced with the new values before the function constructs a new
  /// declarations.
  /// \return Vector of tokens which can be transformed to text string for
  /// insertion into source code or an empty vector in case of errors.
  /// \attention This method does not allocate memory for strings in the result.
  /// References to substrings of parameters is used. So the result will be
  /// valid while references to parameters is valid.
  ///
  /// SLOW!
std::vector<llvm::StringRef> buildDeclStringRef(llvm::StringRef Type,
  llvm::StringRef Id, llvm::StringRef Context,
  const llvm::StringMap<std::string> &Replacements);

/// \brief Constructs correct language declaration of a specified
/// identifier `Id` with a specified type `Type`.
///
/// \tparam TokenT Type of a token which should be implicitly constructible from
/// llvm::StringRef.
/// \note For details see buildDeclStringRef().
/// \return Vector of tokens which can be transformed to text string for
/// insertion into source code.
template<class TokenT>
std::vector<TokenT> buildDecl(llvm::StringRef Type,
    llvm::StringRef Id, llvm::StringRef Context,
    const llvm::StringMap<std::string> &Replacements) {
  std::vector<TokenT> Out;
  auto Tokens = buildDeclStringRef(Type, Id, Context, Replacements);
  for (auto T : Tokens)
    Out.emplace_back(T);
  return Out;
}

/// Reformat a given range of code from a specified file.
llvm::Expected<std::string> reformat(llvm::StringRef Code,
  llvm::StringRef Fliename);

/// Returns location of the beginning of a line which contains a specified
/// location.
inline clang::SourceLocation getStartOfLine(clang::SourceLocation Loc,
    const clang::SourceManager &SM) {
  bool IsInvalid;
  unsigned Col = SM.getPresumedColumnNumber(Loc, &IsInvalid);
  if (IsInvalid)
    return clang::SourceLocation();
  auto StartOfLine = clang::SourceLocation::getFromRawEncoding(
    Loc.getRawEncoding() + 1 - Col);
  Col = SM.getPresumedColumnNumber(StartOfLine, &IsInvalid);
  if (IsInvalid)
    return clang::SourceLocation();
  if (Col > 1)
    return clang::SourceLocation::getFromRawEncoding(
      StartOfLine.getRawEncoding() - 1);
  return StartOfLine;
}

/// Returns range of expansion locations.
inline clang::CharSourceRange getExpansionRange(const clang::SourceManager &SM,
    clang::SourceRange Range) {
  return SM.getExpansionRange(Range);
}

/// Returns range of spelling locations.
inline clang::CharSourceRange getSpellingRange(const clang::SourceManager &SM,
    clang::SourceRange Range) {
  return clang::CharSourceRange::getCharRange(
    SM.getSpellingLoc(Range.getBegin()), SM.getSpellingLoc(Range.getEnd()));
}

/// If it is a macro location this function returns the expansion
/// location or the spelling location, depending on if it comes from a
/// macro argument or not.
///
/// In case of expansion `MACRO(X)` file location for `X` is a spelling location
/// which points to `X`.
/// \code
///   MACRO(X)
///         ^
/// \endcode
/// In case of macro `#define MACRO x` file location for `X` is an expansion
/// location which points to the beginning of macro expansion.
/// \code
///   MACRO
///   ^
/// \endcode
inline clang::SourceRange getFileRange(const clang::SourceManager &SM,
    clang::SourceRange Range) {
  return clang::SourceRange(
    SM.getFileLoc(Range.getBegin()), SM.getFileLoc(Range.getEnd()));
}

/// \brief Returns true if `Range` contains `SubRange`
/// (or `Range` == `SubRange`).
///
/// If `IsInvalid` is not `nullptr` it will be set to `true` in case of
/// invalid ranges. If one of ranges is invalid the result will be `false`.
inline bool isSubRange(const clang::SourceManager &SM,
    clang::SourceRange SubRange, clang::SourceRange Range,
    bool *IsInvalid = nullptr) {
  if (SubRange.isInvalid() || Range.isInvalid())
    return IsInvalid ? *IsInvalid = true, false : false;
  if (IsInvalid)
    *IsInvalid = false;
  return Range.getBegin() <= SubRange.getBegin() &&
    SubRange.getEnd() <= Range.getEnd();
}

/// Return name of a specified function.
///
/// This returns the name as a single StringRef if it can be
/// represented as such. Otherwise the name is written into the given
/// SmallVector and a StringRef to the SmallVector's data is returned.
llvm::StringRef getFunctionName(clang::FunctionDecl &FD,
    llvm::SmallVectorImpl<char> &Name);

/// Create a new output file.
///
/// Emit diagnostic on error and return nullptr.
/// This function is copied from clang::CompilerInstance.
llvm::Optional<OutputFile> createDefaultOutputFile(
    clang::DiagnosticsEngine &Diags, llvm::StringRef OutputPath = "",
    bool Binary = true, llvm::StringRef BaseInput = "",
    llvm::StringRef Extension = "", bool RemoveFileOnSignal = true,
    bool UseTemporary = true,
    bool CreateMissingDirectories = false);

/// Return a statement which cause a side effect inside a specified statement.
const clang::Stmt *findSideEffect(const clang::Stmt &S);
}
#endif//TSAR_CLANG_UTILS_H
