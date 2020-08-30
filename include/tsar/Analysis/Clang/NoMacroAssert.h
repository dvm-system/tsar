//===--- NoMacroAssert.h --- No Macro Assert (Clang) ------------*- C++ -*-===//
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
// This file defines a pass which checks absence of a macro in a specified
// source range marked with `#pragma spf assert nomacro`. Note, that all
// preprocessor directives (except #pragma) are also treated as a macros.
// This file also implements functions to visit all macros in a specified range.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_ASSERT_NO_MACRO_H
#define TSAR_CLANG_ASSERT_NO_MACRO_H

#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Support/Clang/SourceLocationTraverse.h"
#include "tsar/Support/Clang/Utils.h"
#include <bcl/utility.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Pass.h>

namespace tsar {
namespace detail {
/// \brief Helpful class to visit all macros in a specified range.
///
/// Usage:
///   V.Traverse...(Node);
///   V.RawTraverse(CharSourceRange::getTokenRange(Node.getSourceRange()));
template<class FuncT>
class MacroVisitor : public clang::RecursiveASTVisitor<MacroVisitor<FuncT>> {
public:
  explicit MacroVisitor(const clang::SourceManager &SrcMgr,
      const clang::LangOptions &LangOpts,
      const llvm::StringMap<clang::SourceLocation> &RawMacros, FuncT &F) :
    mSrcMgr(SrcMgr), mLangOpts(LangOpts), mRawMacros(RawMacros), mFunc(F) {}

  /// \brief Relex a specified range in a raw mode and searches macros.
  ///
  /// This uses internal list of visited locations to avoid reevaluation
  /// of macro. So, call Travser...() method at first.
  void RawTraverse(clang::CharSourceRange SR) {
    LocalLexer Lex(SR, mSrcMgr, mLangOpts);
    clang::Token Tok;
    while (!Lex.LexFromRawLexer(Tok)) {
      if (Tok.is(clang::tok::hash) && Tok.isAtStartOfLine()) {
        auto MacroLoc = Tok.getLocation();
        Lex.LexFromRawLexer(Tok);
        llvm::StringRef Id = Tok.getRawIdentifier();
        if (Id == "pragma")
          continue;
        if (Id == "define" || Id == "undef" ||
            Id == "ifdef" || Id == "ifndef")
          Lex.LexFromRawLexer(Tok);
        mFunc(MacroLoc);
      } else if (Tok.is(clang::tok::raw_identifier) &&
          !mVisitedLocs.count(Tok.getLocation().getRawEncoding()) &&
          mRawMacros.count(Tok.getRawIdentifier())) {
        mFunc(Tok.getLocation());
      }
    }
  }

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return true;
    Pragma P(*S);
    if (P)
      return true;
    return clang::RecursiveASTVisitor<MacroVisitor<FuncT>>::TraverseStmt(S);
  }

  bool VisitStmt(clang::Stmt *S) {
    traverseSourceLocation(S,
      [this](clang::SourceLocation Loc) { checkLoc(Loc); });
    return true;
  }

  bool VisitDecl(clang::Decl *D) {
    traverseSourceLocation(D,
      [this](clang::SourceLocation Loc) { checkLoc(Loc); });
    return true;
  }

  bool VisitiTypeLoc(clang::TypeLoc T) {
    traverseSourceLocation(T,
      [this](clang::SourceLocation Loc) { checkLoc(Loc); });
    return true;
  }
private:
  void checkLoc(clang::SourceLocation Loc) {
    if (!mVisitedLocs.insert(
          mSrcMgr.getExpansionLoc(Loc).getRawEncoding()).second)
      return;
    if (Loc.isValid() && Loc.isMacroID())
      mFunc(Loc);
  }

  const clang::LangOptions &mLangOpts;
  const clang::SourceManager &mSrcMgr;
  const llvm::StringMap<clang::SourceLocation> &mRawMacros;
  FuncT &mFunc;
  llvm::DenseSet<unsigned> mVisitedLocs;
};
}

///\brief Applies a function to all macros in a specified range.
///
/// \return `false` if bounds of a specified range are located in different
/// files. In this case raw search of macros can not be performed.
template<class FuncT>
bool for_each_macro(clang::Stmt *S,
    const clang::SourceManager &SrcMgr, const clang::LangOptions &LangOpts,
    const llvm::StringMap<clang::SourceLocation> &RawMacros, FuncT F) {
  detail::MacroVisitor<FuncT> Visitor(SrcMgr, LangOpts, RawMacros, F);
  Visitor.TraverseStmt(S);
  auto ExpRange = SrcMgr.getExpansionRange(S->getSourceRange());
  if (!SrcMgr.isWrittenInSameFile(ExpRange.getBegin(), ExpRange.getEnd()))
    return false;
  Visitor.RawTraverse(ExpRange);
  return true;
}

///\brief Applies a function to all macros in a specified range.
///
/// \return `false` if bounds of a specified range are located in different
/// files. In this case raw search of macros can not be performed.
template<class FuncT>
bool for_each_macro(clang::Decl *D,
    const clang::SourceManager &SrcMgr, const clang::LangOptions &LangOpts,
    const llvm::StringMap<clang::SourceLocation> &RawMacros, FuncT F) {
  detail::MacroVisitor<FuncT> Visitor(SrcMgr, LangOpts, RawMacros, F);
  Visitor.TraverseDecl(D);
  auto ExpRange = SrcMgr.getExpansionRange(D->getSourceRange());
  if (!SrcMgr.isWrittenInSameFile(ExpRange.getBegin(), ExpRange.getEnd()))
    return false;
  Visitor.RawTraverse(ExpRange);
  return true;
}

///\brief Applies a function to all macros in a specified range.
///
/// \return `false` if bounds of a specified range are located in different
/// files. In this case raw search of macros can not be performed.
template<class FuncT>
bool for_each_macro(clang::TypeLoc TL,
    const clang::SourceManager &SrcMgr, const clang::LangOptions &LangOpts,
    const llvm::StringMap<clang::SourceLocation> &RawMacros, FuncT F) {
  detail::MacroVisitor<FuncT> Visitor(SrcMgr, LangOpts, RawMacros, F);
  Visitor.TraverseTypeLoc(TL);
  auto ExpRange = SrcMgr.getExpansionRange(TL.getSourceRange());
  if (!SrcMgr.isWrittenInSameFile(ExpRange.getBegin(), ExpRange.getEnd()))
    return false;
  Visitor.RawTraverse(ExpRange);
  return true;
}
}

namespace llvm {
/// Checks absence of a macro in source ranges which are marked with
/// `assert nomacro` directive.
class ClangNoMacroAssert : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  ClangNoMacroAssert(bool *IsInvalid = nullptr) :
      FunctionPass(ID), mIsInvalid(IsInvalid) {
    initializeClangNoMacroAssertPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
private:
  bool *mIsInvalid = nullptr;
};
}

#endif//TSAR_CLANG_ASSERT_NO_MACRO_H
