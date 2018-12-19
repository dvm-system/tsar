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
#include "NoMacroAssert.h"
#include "tsar_pragma.h"
#include "tsar_query.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/DenseMap.h>
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

char RenameLocalPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(RenameLocalPass,"clang-rename",
  "Source-level Renaming of Local Objects (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass);
INITIALIZE_PASS_IN_GROUP_END(RenameLocalPass,"clang-rename",
  "Source-level Renaming of Local Objects (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry())

void RenameLocalPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createRenameLocalPass() { return new RenameLocalPass(); }

namespace {
class DeclVisitor : public RecursiveASTVisitor <DeclVisitor> {
public:
  explicit DeclVisitor(tsar::TransformationContext *TfmCtx) :
    mTfmCtx(TfmCtx), mRewriter(TfmCtx->getRewriter()) {}

  bool TraverseFunctionDecl(FunctionDecl *S) {
    auto Stash = mNames;
    RecursiveASTVisitor<DeclVisitor>::TraverseFunctionDecl(S);
    mNames = Stash;
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *V) {
    auto ND = V->getFoundDecl();
    auto I = mChange.find(ND);
    if (I != mChange.end()) {
      std::string Name = ((V->getNameInfo()).getName()).getAsString();
      mRewriter.ReplaceText(V->getLocation(), Name.length(), I->second);
    }
    return true;
  }

  bool VisitVarDecl(VarDecl * V) {
    std::string Name = V->getName();
    std::string Buf;
    unsigned Count = 1;
    if (mNames.count(Name)) {
      while (mNames.count(Name + std::to_string(Count))) Count++;
      Buf = Name + std::to_string(Count);
      mNames.insert(Name + std::to_string(Count));
      mChange.try_emplace(V, Buf);
      mRewriter.ReplaceText(V->getLocation(), Name.length(), Buf);
    } else {
      mNames.insert(Name);
    }
    return true;
  }

#ifdef LLVM_DEBUG
  void printChanges() {
    dbgs() << "[RENAME]: original and new names in the scope: \n";
    for (auto &N : mChange)
      dbgs() << N.first->getName() << "->" << N.second << " ";
    dbgs() << "\n";
  }

  void printAllNamesInScope() {
    dbgs() << "[RENAME]: names which are used in the scope:\n";
    for (auto &N : mNames)
      dbgs() << N.first() << " ";
    dbgs() << "\n";
  }
#endif

private:
  tsar::TransformationContext * mTfmCtx;
  clang::Rewriter &mRewriter;

  /// \brief List of all names in a scope.
  ///
  /// TODO (kaniandr@gmail.com): store names of all used objects instead of
  /// variable names only.
  StringSet<> mNames;

  /// List of new names of declarations.
  DenseMap<NamedDecl *, std::string> mChange;
};

/// The visitor searches a pragma `rename` and performs renaming for a scope
/// after it. It also checks absence a macros in this scope and print some
/// other warnings.
class RenameChecker : public RecursiveASTVisitor <RenameChecker> {
public:
  RenameChecker(tsar::TransformationContext *TfmCtx) :
    mTfmCtx(TfmCtx), mRewriter(TfmCtx->getRewriter()),
    mContext(TfmCtx->getContext()), mSrcMgr(mRewriter.getSourceMgr()) {}

  bool TraverseCompoundStmt(clang::CompoundStmt * S) {
    if (mFlag) {
      // There was a pragma rename, so check absence of macros and perform
      // renaming.
      mClauses.pop_back();
      mFlag = false;
      if (mIsMacro) {
        mIsMacro = false;
        StringMap<SourceLocation> mRawMacros;
        for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawMacros,
          [this](clang::SourceLocation Loc) {
            toDiag(mContext.getDiagnostics(), Loc,
              diag::warn_rename_macro_prevent);
            mIsMacro = true;
        });
        if (mIsMacro) return true;
      }
      DeclVisitor Vis(mTfmCtx);
      Vis.TraverseCompoundStmt(S);
      return true;
    }
    Pragma P(*S);
    if (findClause(P, ClauseId::Rename, mClauses)) {
      mFlag = true;
      return true;
    } else
      return RecursiveASTVisitor::TraverseCompoundStmt(S);
  }

  bool VisitStmt(clang::Stmt * S) {
    if (mFlag) {
      mFlag = false;
      toDiag(mContext.getDiagnostics(), mClauses[0]->getLocStart(),
        diag::warn_unexpected_directive);
      mClauses.pop_back();
    }
    return RecursiveASTVisitor::VisitStmt(S);
  }

  bool VisitDecl(clang::Decl * D) {
    if (mFlag) {
      mFlag = false;
      toDiag(mContext.getDiagnostics(), mClauses[0]->getLocStart(),
        diag::warn_unexpected_directive);
      mClauses.pop_back();
    }
    return RecursiveASTVisitor::VisitDecl(D);
  }

private:
  tsar::TransformationContext *mTfmCtx;
  clang::Rewriter &mRewriter;
  clang::ASTContext &mContext;
  clang::SourceManager &mSrcMgr;
  SmallVector<Stmt *, 1> mClauses;
  bool mIsMacro = true;
  bool mFlag = false;
};
}

bool RenameLocalPass::runOnModule(Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  RenameChecker Vis(TfmCtx);
  Vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  return false;
}