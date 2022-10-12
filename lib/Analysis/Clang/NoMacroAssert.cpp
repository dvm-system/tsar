//===--- NoMacroAssert.cpp - No Macro Assert (Clang) -----------*- C++ -*-===//
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
// This file implements a pass which checks absence of macro in a specified
// source range marked with `#pragma spf assert nomacro`. Note, that all
// preprocessor directives (except #pragma) are also treated as a macros.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
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
      checkNode(S);
      mActiveClause = nullptr;
      return true;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseDecl(Decl *D) {
    if (mActiveClause) {
      checkNode(D);
      mActiveClause = nullptr;
      return true;
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }

  bool VisitiTypeLoc(TypeLoc T) {
    if (mActiveClause) {
      checkNode(T);
      mActiveClause = nullptr;
      return true;
    }
    return RecursiveASTVisitor::TraverseTypeLoc(T);
  }

private:
  template<class T> void checkNode(T Node) {
    auto MacroVisitor = [this](SourceLocation Loc) {
      mIsInvalid = true;
      toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getBeginLoc(),
        tsar::diag::err_assert);
      toDiag(mSrcMgr.getDiagnostics(), Loc,
        tsar::diag::note_assert_no_macro);
    };
    if (!for_each_macro(Node, mSrcMgr, mLangOpts, mRawMacros, MacroVisitor)) {
      mIsInvalid = true;
      toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getBeginLoc(),
        tsar::diag::err_assert);
      toDiag(mSrcMgr.getDiagnostics(), getPointer(Node)->getBeginLoc(),
        tsar::diag::note_source_range_not_single_file);
      toDiag(mSrcMgr.getDiagnostics(), getPointer(Node)->getEndLoc(),
        tsar::diag::note_end_location);
    }
  }

  template<class T> T * getPointer(T *Ptr) noexcept { return Ptr; }
  TypeLoc * getPointer(TypeLoc &TL) noexcept { return &TL; }

  const SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  const StringMap<SourceLocation> &mRawMacros;
  clang::Stmt *mActiveClause = nullptr;
  bool mIsInvalid = false;
};
}

bool ClangNoMacroAssert::runOnFunction(Function &F) {
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError(
        "cannot check sources"
        ": transformation context is not available for the '" +
        F.getName() + "' function");
    return false;
  }
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  auto &LangOpts = TfmCtx->getContext().getLangOpts();
  auto *Unit = TfmCtx->getContext().getTranslationUnitDecl();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto *GI{ GIP.getGlobalInfo(TfmCtx) };
  assert(GI && "Global information must not be null!");
  NoMacroChecker Checker(SrcMgr, LangOpts, GI->RI.Macros);
  Checker.TraverseDecl(Unit);
  if (mIsInvalid)
    *mIsInvalid = Checker.isInvalid();
  return false;
}
