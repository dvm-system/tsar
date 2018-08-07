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

namespace llvm {
template <typename PtrType> class SmallPtrSetImpl;
}

namespace clang {
class CFG;
class CFGBlock;
class SourceManager;
}

namespace tsar {
/// Finds unreachable basic blocks for a specified CFG.
void unreachableBlocks(clang::CFG &Cfg,
  llvm::SmallPtrSetImpl<clang::CFGBlock *> &Blocks);

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
