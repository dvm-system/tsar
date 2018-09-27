//===--- NoMacroAssert.cpp - No Macro Assert (Clang) -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass which checks absence of macro in a specified
// source range marked with `#pragma spf assert nomacro`. Note, that all
// preprocessor directives (except #pragma) are also treated as a macros.
//
//===----------------------------------------------------------------------===//

#include "NoMacroAssert.h"
#include "ClangUtils.h"
#include "Diagnostic.h"
#include "GlobalInfoExtractor.h"
#include "tsar_query.h"
#include "PassGroupRegistry.h"
#include "tsar_pragma.h"
#include "SourceLocationTraverse.h"
#include "tsar_transformation.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-nomacro-assert"

char ClangNoMacroAssert::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangNoMacroAssert, "clang-nomacro-assert",
  "No Macro Assert (Clang)", false, false, CheckQueryManager::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangNoMacroAssert, "clang-nomacro-assert",
  "No Macro Assert (Clang)", false, false, CheckQueryManager::getPassRegistry())

FunctionPass * llvm::createClangNoMacroAssert(bool *IsInvalid) {
  return new ClangNoMacroAssert(IsInvalid);
}

void ClangNoMacroAssert::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

namespace {
class NoMacroChecker : public RecursiveASTVisitor<NoMacroChecker> {
public:
  NoMacroChecker(const SourceManager &SrcMgr, const LangOptions &LangOpts,
    const StringMap<SourceLocation> &RawMacros) :
    mSrcMgr(SrcMgr), mLangOpts(LangOpts), mRawMacros(RawMacros) {}

  bool isValid() const noexcept { return !mIsInvalid; }
  bool isInvalid() const noexcept { return mIsInvalid; }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    SmallVector<Stmt *, 1> Clauses;
    Pragma P(*S);
    if (findClause(P, ClauseId::AssertNoMacro, Clauses)) {
      mActiveClause = !mActiveClause ? Clauses.front() : mActiveClause;
      return true;
    }
    if (P)
      return true;
    if (mActiveClause) {
      mRootToCheck = !mRootToCheck ? S : mRootToCheck;
      traverseSourceLocation(S, [this](SourceLocation Loc) { checkLoc(Loc); });
    }
    bool Res = RecursiveASTVisitor::TraverseStmt(S);
    if (mRootToCheck == S) {
      auto ExpRange =
        mSrcMgr.getExpansionRange(S->getSourceRange());
      if (!mSrcMgr.isWrittenInSameFile(ExpRange.getBegin(), ExpRange.getEnd())) {
        toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getLocStart(),
          diag::err_assert);
        toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(),
          diag::note_source_range_not_single_file);
        toDiag(mSrcMgr.getDiagnostics(), S->getLocEnd(),
          diag::note_end_location);
      }
      LocalLexer Lex(ExpRange, mSrcMgr, mLangOpts);
      Token Tok;
      while (!Lex.LexFromRawLexer(Tok)) {
        if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
          auto MacroLoc = Tok.getLocation();
          Lex.LexFromRawLexer(Tok);
          StringRef Id = Tok.getRawIdentifier();
          if (Id == "pragma")
            continue;
          if (Id == "define" || Id == "undef" ||
              Id == "ifdef" || Id == "ifndef")
            Lex.LexFromRawLexer(Tok);
          toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getLocStart(),
            diag::err_assert);
          toDiag(mSrcMgr.getDiagnostics(), MacroLoc,
            diag::note_assert_no_macro);
        } else if (Tok.is(tok::raw_identifier) &&
            !mVisitedLocs.count(Tok.getLocation().getRawEncoding()) &&
            mRawMacros.count(Tok.getRawIdentifier())) {
          toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getLocStart(),
            diag::err_assert);
          toDiag(mSrcMgr.getDiagnostics(), Tok.getLocation(),
            diag::note_assert_no_macro);
        }
      }
      mRootToCheck = nullptr;
      mActiveClause = nullptr;
    }
    return Res;
  }

  bool VisitDecl(Decl *D) {
    if (mActiveClause)
      traverseSourceLocation(D, [this](SourceLocation Loc) { checkLoc(Loc); });
    return true;
  }

  bool VisitiTypeLoc(TypeLoc T) {
    if (mActiveClause)
      traverseSourceLocation(T, [this](SourceLocation Loc) { checkLoc(Loc); });
    return true;
  }

private:
  void checkLoc(SourceLocation Loc) {
    if (!mVisitedLocs.insert(
          mSrcMgr.getExpansionLoc(Loc).getRawEncoding()).second)
      return;
    if (Loc.isValid() && Loc.isMacroID()) {
      toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getLocStart(),
        diag::err_assert);
      toDiag(mSrcMgr.getDiagnostics(), Loc, diag::note_assert_no_macro);
    }
  }

  const SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  const StringMap<SourceLocation> &mRawMacros;
  clang::Stmt *mActiveClause = nullptr;
  clang::Stmt *mRootToCheck = nullptr;
  bool mIsInvalid = false;
  GlobalInfoExtractor::RawLocationSet mVisitedLocs;
};
}

bool ClangNoMacroAssert::runOnFunction(Function &F) {
  auto *M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M->getContext().emitError("can not check sources"
        ": transformation context is not available");
    if (mIsInvalid)
      *mIsInvalid = true;
    return false;
  }
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  auto &LangOpts = TfmCtx->getContext().getLangOpts();
  auto *Unit = TfmCtx->getContext().getTranslationUnitDecl();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  NoMacroChecker Checker(SrcMgr, LangOpts, GIP.getRawInfo().Macros);
  Checker.TraverseDecl(Unit);
  if (mIsInvalid)
    *mIsInvalid = Checker.isInvalid();
  return false;
}
