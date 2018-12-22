//=== UselessVariables.cpp - Dead Declaration Elimination (Clang) *- C++ -*===//
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
// This file implements a pass to eliminate dead declarations in a source code.
//
//===----------------------------------------------------------------------===//

#include "UselessVariables.h"
#include "Diagnostic.h"
#include "GlobalInfoExtractor.h"
#include "NoMacroAssert.h"
#include "tsar_query.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
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
#define DEBUG_TYPE "clang-de-decls"

char ClangUselessVariablesPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangUselessVariablesPass, "clang-de-decls",
  "Dead Declarations Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangUselessVariablesPass, "clang-de-decls",
  "Dead Declarations Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {
class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {
public:
  explicit DeclVisitor(clang::Rewriter &Rewriter) : mRewriter(&Rewriter) {}

  bool VisitVarDecl(VarDecl *D) {
    mDeadDecls.emplace(D, mScopes.top());
    return true;
  }

  bool VisitTypeDecl(TypeDecl *D) {
    auto Itr = mDeadDecls.emplace(D, mScopes.top()).first;
    assert(Itr != mDeadDecls.end() &&
      "Unable to insert declaration in the map!");
    mTypeDecls.try_emplace(D->getTypeForDecl(), Itr);
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *D) {
    mDeadDecls.erase(D->getFoundDecl());
    return true;
  }

  bool VisitTypeLoc(TypeLoc TL) {
    auto DeclItr = mTypeDecls.find(TL.getTypePtr());
    if (DeclItr != mTypeDecls.end()) {
      mDeadDecls.erase(DeclItr->second);
      mTypeDecls.erase(DeclItr);
    }
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S) {
    auto Group = S->getDeclGroup();
    if(!S->isSingleDecl())
      mMultipleDecls.insert(S);
    return true;
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    if (isa<ForStmt>(S) || isa<IfStmt>(S) || isa<CompoundStmt>(S) ||
        isa<DoStmt>(S) || isa<WhileStmt>(S)) {
      mScopes.push(S);
      auto Res = RecursiveASTVisitor::TraverseStmt(S);
      mScopes.pop();
      return Res;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  void eliminateDeadDecls(const ClangGlobalInfoPass::RawInfo &RawInfo) {
    // We should check absence of macros in a transformed scope.
    DenseMap<Stmt *, SourceLocation> ScopeWithMacro;
    for (auto I = mDeadDecls.begin(), EI = mDeadDecls.end(); I != EI;) {
      SourceLocation MacroLoc;
      auto MacroItr = ScopeWithMacro.find(I->second);
      if (MacroItr == ScopeWithMacro.end()) {
        for_each_macro(I->second, mRewriter->getSourceMgr(),
          mRewriter->getLangOpts(), RawInfo.Macros,
          [&MacroLoc](SourceLocation Loc) { MacroLoc = Loc; });
        ScopeWithMacro.try_emplace(I->second, MacroLoc);
      } else {
        MacroLoc = MacroItr->second;
      }
      if (MacroLoc.isValid()) {
        toDiag(mRewriter->getSourceMgr().getDiagnostics(),
          I->first->getLocation(), diag::warn_remove_useless_variables);
        I = mDeadDecls.erase(I);
        continue;
      }
      if (auto VD = dyn_cast<VarDecl>(I->first)) {
        if (!VD->isLocalVarDecl() && VD->isLocalVarDeclOrParm()) {
          toDiag(mRewriter->getSourceMgr().getDiagnostics(),
            VD->getLocation(), diag::warn_remove_useless_variables);
          I = mDeadDecls.erase(I);
          continue;
        } else if (VD->hasInit() && findCallExpr(*VD->getInit())) {
          toDiag(mRewriter->getSourceMgr().getDiagnostics(),
            VD->getLocation(), diag::warn_remove_useless_variables);
          I = mDeadDecls.erase(I);
          continue;
        }
      }
      ++I;
    }
    // Check that all declarations in a group are dead.
    for (auto *S : mMultipleDecls) {
      auto Group = S->getDeclGroup();
      unsigned GroupSize = 0;
      SmallVector<NamedDecl *, 8> DeadInGroup;
      for (auto *D : Group) {
        GroupSize++;
        if (auto ND = dyn_cast<NamedDecl>(D))
          if (mDeadDecls.count(ND))
            DeadInGroup.push_back(ND);
      }
      if (!DeadInGroup.empty() && DeadInGroup.size() != GroupSize) {
        for (auto *ND : DeadInGroup) {
          toDiag(mRewriter->getSourceMgr().getDiagnostics(),
            ND->getLocation(), diag::warn_remove_useless_variables);
          mDeadDecls.erase(ND);
        }
      }
    }
    for (auto I = mDeadDecls.begin(), EI = mDeadDecls.end(); I != EI; I++)
      mRewriter->RemoveText(I->first->getSourceRange());
  }

  void printRemovedDecls() {
    for (auto &D : mDeadDecls) {
      dbgs() << D.first->getName() << "(" << (ptrdiff_t)(D.first) <<") ";
    }
    dbgs() << "\n";
  }

private:
  /// Returns true if there is a call expression inside a specified statement.
  bool findCallExpr(const Stmt &S) {
    if (!isa<CallExpr>(S)) {
      for (auto Child : make_range(S.child_begin(), S.child_end()))
        if (Child && findCallExpr(*Child))
          return true;
      return false;
    }
    return true;
  }

  std::map<NamedDecl *, Stmt *> mDeadDecls;
  std::stack<Stmt*> mScopes;
  clang::Rewriter *mRewriter;
  DenseSet<DeclStmt*> mMultipleDecls;
  DenseMap<const clang::Type *, decltype(mDeadDecls)::const_iterator> mTypeDecls;
};
}

bool ClangUselessVariablesPass::runOnFunction(Function &F) {
  auto *M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M->getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  DeclVisitor Visitor(TfmCtx->getRewriter());
  Visitor.TraverseStmt(FuncDecl->getBody());
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  LLVM_DEBUG(
    dbgs() << "[DEAD DECLS ELIMINATION]: list of all dead declarations ";
    Visitor.printRemovedDecls());
  Visitor.eliminateDeadDecls(GIP.getRawInfo());
  LLVM_DEBUG(
    dbgs() << "[DEAD DECLS ELIMINATION]: list of removed declarations ";
    Visitor.printRemovedDecls());
  return false;
}

void ClangUselessVariablesPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangUselessVariablesPass() {
  return new ClangUselessVariablesPass();
}
