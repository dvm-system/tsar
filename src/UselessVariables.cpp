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
#include <llvm/ADT/DenseSet.h>
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

  bool VisitDeclRefExpr(DeclRefExpr *D) {
    if (auto VD = dyn_cast<VarDecl>(D->getFoundDecl()))
      mDeadDecls.erase(VD);
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S) {
    auto Group = S->getDeclGroup();
    if(!S->isSingleDecl())
      mMultipleDecls.insert(S);
    return true;
  }

  bool VisitIfStmt(IfStmt *S) {
    auto D = S->getConditionVariable();
    if (D != nullptr) {
      auto it = mDeadDecls.find( D );
      if (it != mDeadDecls.end()) {
        toDiag(mRewriter->getSourceMgr().getDiagnostics(), \
          (it->first)->getLocation(), diag::warn_remove_useless_variables);
        mDeadDecls.erase(it);
      }
    }
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

  bool findCallExpr(Stmt *S) {
    bool Result = isa<CallExpr>(S);
    if (!Result) {
      for (auto I = S->child_begin(), EI = S->child_end(); I != EI; I++) {
        if (*I != nullptr) {
          if ((Result = findCallExpr(*I)))
            return true;
        }
      }
      return false;
    }
    return true;
  }

  void DelVarsFromCode() {
    for (auto I = mDeadDecls.begin(), EI = mDeadDecls.end(); I != EI;) {
      if (!I->first->isLocalVarDecl() && I->first->isLocalVarDeclOrParm()) {
        toDiag(mRewriter->getSourceMgr().getDiagnostics(),
          (I->first)->getLocation(), diag::warn_remove_useless_variables);
        I = mDeadDecls.erase(I);
      } else {
        I++;
      }
    }
    //ignore declaration which init by function return value
    for (auto I = mDeadDecls.begin(), EI = mDeadDecls.end(); I != EI;) {
      if (I->first->hasInit()) {
        if(findCallExpr(I->first->getInit())) {
          toDiag(mRewriter->getSourceMgr().getDiagnostics(),
            (I->first)->getLocation(), diag::warn_remove_useless_variables);
          I = mDeadDecls.erase(I);
        } else {
          ++I;
        }
      } else {
        ++I;
      }
    }
    // Check that all declarations in a group are dead.
    for (auto I = mMultipleDecls.begin(), EI = mMultipleDecls.end();
         I != EI; I++) {
      auto Group = (*I)->getDeclGroup();
      int GroupSize = 0, DeadNum = 0;
      for (auto DI = Group.begin(), DEI = Group.end(); DI != DEI; DI++) {
        GroupSize++;
        auto Itr = mDeadDecls.find(cast<VarDecl>(*DI));
        if(Itr != mDeadDecls.end())
          DeadNum++;
      }
      if (DeadNum != GroupSize) {
        for (auto DI = Group.begin(), DEI = Group.end(); DI != DEI; DI++) {
          auto Itr = mDeadDecls.find( cast<VarDecl>(*DI) );
          if(Itr != mDeadDecls.end()) {
            toDiag(mRewriter->getSourceMgr().getDiagnostics(),
              Itr->first->getLocation(), diag::warn_remove_useless_variables);
            mDeadDecls.erase(Itr);
          }
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

  std::map <VarDecl*, Stmt*> mDeadDecls;
  std::stack<Stmt*> mScopes;
  clang::Rewriter *mRewriter;
  DenseSet<DeclStmt*>mMultipleDecls;
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
  //search macros in statement
  for (auto I = Visitor.mDeadDecls.begin(), EI = Visitor.mDeadDecls.end();
       I != EI;) {
    bool HasMacro = false;
    for_each_macro(I->second, TfmCtx->getContext().getSourceManager(),
      TfmCtx->getContext().getLangOpts(), GIP.getRawInfo().Macros,
      [&HasMacro](SourceLocation x) {HasMacro = true; });
    if (!HasMacro) {
      ++I;
    } else {
      toDiag(Visitor.mRewriter->getSourceMgr().getDiagnostics(),
          (I->first)->getLocation(), diag::warn_remove_useless_variables);
      I = Visitor.mDeadDecls.erase(I);
    }
  }
  LLVM_DEBUG(
    dbgs() << "[DEAD DECLS ELIMINATION]: list of all dead declarations ";
    Visitor.printRemovedDecls());
  Visitor.DelVarsFromCode();
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
