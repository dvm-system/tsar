//=== DeadDeclsElimination.cpp - Dead Decls Elimination (Clang) --*- C++ -*===//
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
// This file implements a pass to initialize firstprivate variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/RemoveFirstPrivate.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <stack>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-rfp"

char ClangRemoveFirstPrivate::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangRemoveFirstPrivate, "remove-firstprivate",
  "Initialize variables in for", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangRemoveFirstPrivate, "remove-firstprivate",
  "Initialize variables in for", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {

class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {
  struct DeclarationInfo {
    DeclarationInfo(Stmt *S) : Scope(S) {}

    Stmt *Scope;
    SmallVector<Stmt *, 16> DeadAccesses;
  };
public:
  explicit DeclVisitor(TransformationContext &TfmCtx, const ASTImportInfo &ImportInfo,
      ClangGlobalInfoPass::RawInfo &RawInfo) :
    mTfmCtx(&TfmCtx), mImportInfo(ImportInfo),
    mRawInfo(&RawInfo), mRewriter(TfmCtx.getRewriter()),
    mContext(TfmCtx.getContext()), mSrcMgr(mRewriter.getSourceMgr()),
    mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;

    bool ast = false;
    Pragma P(*S);

    if (findClause(P, ClauseId::RemoveFirstPrivate, mClauses)) {

      auto locationForInits = S -> getEndLoc();

      isInPragma = true;
      ast = RecursiveASTVisitor::TraverseStmt(S);
      isInPragma = false;


      std::string txtStr;
      std::vector<std::string> inits;
      while (starts.size()) {
        SourceRange toInsert(starts.top(), ends.top());
        CharSourceRange txtToInsert(toInsert, true);
        starts.pop();
        ends.pop();

        txtStr = mRewriter.getRewrittenText(txtToInsert);
        txtStr += ";\n";
        inits.push_back(txtStr);
      }

      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
      auto IsPossible = pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts,
                                            mImportInfo, ToRemove);
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive);
      Rewriter::RewriteOptions RemoveEmptyLine;
      /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
      /// set to true then removing (in RewriterBuffer) works incorrect.
      RemoveEmptyLine.RemoveLineIfEmpty = false;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine); // delete each range

      for (std::vector<std::string>::iterator it = inits.begin(); it != inits.end(); ++it) {
        mRewriter.InsertTextAfterToken(locationForInits, *it);
      }
      return ast;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseDeclRefExpr(clang::DeclRefExpr *Ex) {
    NamedDecl *named = nullptr;
    if (isInPragma) {

      if (waitingForVar) {
        starts.push(Ex -> getLocation());
      } else {
        ends.push(Ex -> getLocation());
      }
      waitingForVar = !waitingForVar;
    }
    return RecursiveASTVisitor::TraverseDeclRefExpr(Ex);
  }

#ifdef LLVM_DEBUG
// debug info
#endif

private:
  /// Return current scope.
  Stmt *getScope() {
    for (auto I = mScopes.rbegin(), EI = mScopes.rend(); I != EI; ++I)
      if (isa<ForStmt>(*I) || isa<CompoundStmt>(*I))
        return *I;
    return nullptr;
  }

  bool isInPragma = false;
  bool waitingForVar = true;
  std::map<NamedDecl *, DeclarationInfo> mDeadDecls;
  std::vector<Stmt*> mScopes;
  // clang::Rewriter *mRewriter;
  DenseSet<DeclStmt*> mMultipleDecls;
  DenseMap<const clang::Type *, decltype(mDeadDecls)::const_iterator> mTypeDecls;
  DeclRefExpr *mSimpleAssignLHS = nullptr;

  TransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  ClangGlobalInfoPass::RawInfo *mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;

  std::stack<SourceLocation> starts;
  std::stack<SourceLocation> ends;

};
}

bool ClangRemoveFirstPrivate::runOnFunction(Function &F) {
  auto *M = F.getParent();
  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  auto *TfmCtx{TfmInfo ? TfmInfo->getContext(*M) : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M->getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;

  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();

  DeclVisitor Visitor(*TfmCtx, *ImportInfo, GIP.getRawInfo());
  Visitor.TraverseDecl(FuncDecl);
  return false;
}

void ClangRemoveFirstPrivate::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangRemoveFirstPrivate() {
  return new ClangRemoveFirstPrivate();
}
