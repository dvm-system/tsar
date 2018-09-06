//===--- ClangUtils.h --- Utilities To Examine Clang AST  -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities to examine Clang AST.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_UTILS_H
#define TSAR_CLANG_UTILS_H

#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <vector>

namespace llvm {
template <typename PtrType> class SmallPtrSetImpl;
}

namespace clang {
class CFG;
class CFGBlock;
class LangOptions;
class MemoryBuffer;
class SourceManager;
}

namespace tsar {
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

  /// Lex a token, returns `false` on success and `true` otherwise
  bool LexFromRawLexer(clang::Token &Tok);

  const clang::SourceManager & getSourceManager() const noexcept { return mSM; }
  const clang::LangOptions & getLangOpts() const noexcept { return mLangOpts; }
  clang::SourceRange getSourceRange() const { return mSR; }

  /// Returns list of already lexed tokens.
  const LexedTokens & getLexedTokens() const noexcept { return mTokens; }

private:
  clang::SourceRange mSR;
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
  clang::FileID FID, const llvm::MemoryBuffer *InputBuffer,
  const clang::SourceManager &SM, const clang::LangOptions &LangOpts,
  llvm::StringMap<clang::SourceLocation> &Macros,
  llvm::StringMap<clang::SourceLocation> &Includes,
  llvm::StringSet<> &Ids);

/// Returns range of expansion locations.
inline clang::SourceRange getExpansionRange(const clang::SourceManager &SM,
    clang::SourceRange Range) {
  return SM.getExpansionRange(Range);
}

/// Returns range of spelling locations.
inline clang::SourceRange getSpellingRange(const clang::SourceManager &SM,
    clang::SourceRange Range) {
  return clang::SourceRange(
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

/// Compares locations.
inline bool operator<=(const clang::SourceLocation& LHS,
    const clang::SourceLocation& RHS) {
  return LHS < RHS || LHS == RHS;
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
}
#endif//TSAR_CLANG_UTILS_H
