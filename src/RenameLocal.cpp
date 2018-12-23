//===- RenameLocal.cpp - Source-level Renaming of Local Objects - *- C++ -*===//
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
// The file declares a pass to perform renaming of objects in a specified scope.
//
//===----------------------------------------------------------------------===//

#include "RenameLocal.h"
#include "DFRegionInfo.h"
#include "Diagnostic.h"
#include "GlobalInfoExtractor.h"
#include "NoMacroAssert.h"
#include "tsar_pragma.h"
#include "tsar_query.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <string>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-rename"

char ClangRenameLocalPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangRenameLocalPass,"clang-rename",
  "Source-level Renaming of Local Objects (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass);
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass);
INITIALIZE_PASS_IN_GROUP_END(ClangRenameLocalPass,"clang-rename",
  "Source-level Renaming of Local Objects (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry())

void ClangRenameLocalPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createClangRenameLocalPass() {
  return new ClangRenameLocalPass();
}

namespace {
/// The visitor searches a pragma `rename` and performs renaming for a scope
/// after it. It also checks absence a macros in this scope and print some
/// other warnings.
class ClangRenamer : public RecursiveASTVisitor<ClangRenamer> {
public:
  ClangRenamer(TransformationContext &TfmCtx,
      ClangGlobalInfoPass::RawInfo &RawInfo) :
    mTfmCtx(&TfmCtx), mRawInfo(&RawInfo), mRewriter(TfmCtx.getRewriter()),
    mContext(TfmCtx.getContext()), mSrcMgr(mRewriter.getSourceMgr()),
    mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    Pragma P(*S);
    if (findClause(P, ClauseId::Rename, mClauses)) {
      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
      if (!pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts, ToRemove))
        toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getLocStart(),
          diag::warn_remove_directive_in_macro);
      Rewriter::RewriteOptions RemoveEmptyLine;
      RemoveEmptyLine.RemoveLineIfEmpty = true;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine);
      return true;
    }
    if (mClauses.empty() || !isa<CompoundStmt>(S) &&
        !isa<ForStmt>(S) && !isa<DoStmt>(S) && !isa<WhileStmt>(S))
      return RecursiveASTVisitor::TraverseStmt(S);
    // There was a pragma rename, so check absence of macros and perform
    // renaming.
    mClauses.clear();
    bool StashRenameState = mActiveRename;
    // We do not perform search of macros in case of nested 'rename'
    // directives and active renaming. The search has been already performed.
    if (!mActiveRename) {
      bool HasMacro = false;
      for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawInfo->Macros,
        [&HasMacro, this](clang::SourceLocation Loc) {
          if (!HasMacro) {
            toDiag(mContext.getDiagnostics(), Loc,
              diag::warn_rename_macro_prevent);
            HasMacro = true;
        }
      });
      // We should not stop traverse because some nested rename directives
      // may exist.
      if (HasMacro)
        return RecursiveASTVisitor::TraverseStmt(S);
      mActiveRename = true;
    }
    auto Res = RecursiveASTVisitor::TraverseStmt(S);
    mActiveRename = StashRenameState;
    return Res;
  }

  bool VisitStmt(Stmt *S) {
    if (!mClauses.empty()) {
      toDiag(mContext.getDiagnostics(), mClauses.front()->getLocStart(),
        diag::warn_unexpected_directive);
      mClauses.clear();
    }
    return RecursiveASTVisitor::VisitStmt(S);
  }

  bool VisitDecl(Decl * D) {
    if (!mClauses.empty()) {
      toDiag(mContext.getDiagnostics(), mClauses.front()->getLocStart(),
        diag::warn_unexpected_directive);
      mClauses.clear();
    }
    return RecursiveASTVisitor::VisitDecl(D);
  }

  bool VisitNamedDecl(NamedDecl *ND) {
    if (mActiveRename) {
      auto Name = ND->getName();
      SmallString<32> Buf;
      if (mRawInfo->Identifiers.count(Name)) {
        for (unsigned Count = 0;
          mRawInfo->Identifiers.count((Name + Twine(Count)).toStringRef(Buf));
          ++Count, Buf.clear());
        StringRef NewName(Buf.data(), Buf.size());
        mRawInfo->Identifiers.insert(NewName);
        mChange.try_emplace(ND, NewName);
        mRewriter.ReplaceText(ND->getLocation(), Name.size(), NewName);
      }
    }
    return RecursiveASTVisitor::VisitNamedDecl(ND);
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    if (mActiveRename) {
      auto ND = Expr->getFoundDecl();
      auto I = mChange.find(ND);
      if (I != mChange.end())
        mRewriter.ReplaceText(
          Expr->getLocation(), ND->getName().size(), I->second);
    }
    return RecursiveASTVisitor::VisitDeclRefExpr(Expr);
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    mChange.clear();
    auto Res = RecursiveASTVisitor::TraverseFunctionDecl(FD);
    LLVM_DEBUG(printChanges());
    return Res;
  }

private:
#ifdef LLVM_DEBUG
  void printChanges() {
    dbgs() << "[RENAME]: original and new names in the scope: ";
    for (auto &N : mChange)
      dbgs() << N.first->getName() << "->" << N.second << " ";
    dbgs() << "\n";
  }
#endif

  TransformationContext *mTfmCtx;
  ClangGlobalInfoPass::RawInfo *mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  bool mActiveRename = false;

  /// List of new names of declarations.
  DenseMap<NamedDecl *, std::string> mChange;
};
}

bool ClangRenameLocalPass::runOnModule(Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  ClangRenamer Vis(*TfmCtx, GIP.getRawInfo());
  Vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  return false;
}
