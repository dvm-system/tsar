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
// This file implements a pass to eliminate dead declarations in a source code.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/DeadDeclsElimination.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Clang/Diagnostic.h"
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

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-de-decls"

char ClangDeadDeclsElimination::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangDeadDeclsElimination, "clang-de-decls",
  "Dead Declarations Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangDeadDeclsElimination, "clang-de-decls",
  "Dead Declarations Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {
class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {
  struct DeclarationInfo {
    DeclarationInfo(Stmt *S) : Scope(S) {}

    Stmt *Scope;
    SmallVector<Stmt *, 16> DeadAccesses;
  };
public:
  explicit DeclVisitor(clang::Rewriter &Rewriter) : mRewriter(&Rewriter) {}

  bool VisitVarDecl(VarDecl *D) {
    mDeadDecls.emplace(D, getScope());
    return true;
  }

  bool VisitTypeDecl(TypeDecl *D) {
    auto Itr = mDeadDecls.emplace(D, getScope()).first;
    assert(Itr != mDeadDecls.end() &&
      "Unable to insert declaration in the map!");
    mTypeDecls.try_emplace(D->getTypeForDecl(), Itr);
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *D) {
    if (D != mSimpleAssignLHS)
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
    if(!S->isSingleDecl())
      mMultipleDecls.insert(S);
    return true;
  }

  bool TraverseBinaryOperator(clang::BinaryOperator *BO) {
    if (!BO || !BO->isAssignmentOp())
      return RecursiveASTVisitor::TraverseBinaryOperator(BO);
    if (!BO->isCompoundAssignmentOp()) {
      auto ParentItr = mScopes.rbegin() + 1; // mScopes.back() is BO
      if (isa<CompoundStmt>(*ParentItr))
        if (auto *Ref = dyn_cast<DeclRefExpr>(BO->getLHS()))
          if (!findSideEffect(*BO->getRHS())) {
            auto Itr = mDeadDecls.find(Ref->getFoundDecl());
            if (Itr != mDeadDecls.end()) {
              Itr->second.DeadAccesses.push_back(BO);
              auto Stash = mSimpleAssignLHS;
              mSimpleAssignLHS = Ref;
              auto Res = RecursiveASTVisitor::TraverseBinaryOperator(BO);
              mSimpleAssignLHS = Stash;
              return Res;
            }
          }
    }
    return RecursiveASTVisitor::TraverseBinaryOperator(BO);
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    mScopes.push_back(S);
    auto Res = RecursiveASTVisitor::TraverseStmt(S);
    mScopes.pop_back();
    return Res;
  }

  /// Checks precondition and performs elimination of dead declarations.
  void eliminateDeadDecls(const ClangGlobalInfo::RawInfo &RawInfo) {
    auto &Diags = mRewriter->getSourceMgr().getDiagnostics();
    DenseMap<Stmt *, SourceLocation> ScopeWithMacro;
    for (auto I = mDeadDecls.begin(), EI = mDeadDecls.end(); I != EI;) {
      if (auto VD = dyn_cast<VarDecl>(I->first)) {
        if (!VD->isLocalVarDecl() && VD->isLocalVarDeclOrParm()) {
          toDiag(Diags, VD->getLocation(),
                 tsar::diag::warn_disable_de_parameter);
          I = mDeadDecls.erase(I);
          continue;
        } else if (VD->hasInit()) {
          if (auto SideEffect = findSideEffect(*VD->getInit())) {
            toDiag(Diags, VD->getLocation(), tsar::diag::warn_disable_de);
            toDiag(Diags, SideEffect->getBeginLoc(),
                   tsar::diag::note_de_side_effect_prevent);
            I = mDeadDecls.erase(I);
            continue;
          }
        }
      }
      assert(I->second.Scope && "Scope must not be global!");
      SourceLocation MacroLoc;
      auto MacroItr = ScopeWithMacro.find(I->second.Scope);
      if (MacroItr == ScopeWithMacro.end()) {
        for_each_macro(I->second.Scope, mRewriter->getSourceMgr(),
          mRewriter->getLangOpts(), RawInfo.Macros,
          [&MacroLoc](SourceLocation Loc) { MacroLoc = Loc; });
        ScopeWithMacro.try_emplace(I->second.Scope, MacroLoc);
      } else {
        MacroLoc = MacroItr->second;
      }
      if (MacroLoc.isValid()) {
        toDiag(Diags, I->first->getLocation(), tsar::diag::warn_disable_de);
        toDiag(Diags, MacroLoc, tsar::diag::note_de_macro_prevent);
        I = mDeadDecls.erase(I);
        continue;
      }
      ++I;
    }
    // Check that all declarations in a group are dead.
    for (auto *S : mMultipleDecls) {
      auto Group = S->getDeclGroup();
      const Decl *LiveD = nullptr;
      SmallVector<NamedDecl *, 8> DeadInGroup;
      for (auto *D : Group) {
        if (auto ND = dyn_cast<NamedDecl>(D))
          if (mDeadDecls.count(ND)) {
            DeadInGroup.push_back(ND);
            continue;
          }
        LiveD = D;
      }
      if (!DeadInGroup.empty() && LiveD) {
        for (auto *ND : DeadInGroup) {
          toDiag(Diags, ND->getLocation(), tsar::diag::warn_disable_de);
          toDiag(Diags, LiveD->getLocation(),
                 tsar::diag::note_de_multiple_prevent);
          mDeadDecls.erase(ND);
        }
      }
    }
    auto &SrcMgr = mRewriter->getSourceMgr();
    auto &LangOpts = mRewriter->getLangOpts();
    Rewriter::RewriteOptions RemoveEmptyLine;
    /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
    /// set to true then removing (in RewriterBuffer) works incorrect.
    RemoveEmptyLine.RemoveLineIfEmpty = false;
    for (auto I = mDeadDecls.begin(), EI = mDeadDecls.end(); I != EI; I++) {
      mRewriter->RemoveText(I->first->getSourceRange());
      for (auto S : I->second.DeadAccesses) {
        mRewriter->RemoveText(S->getSourceRange());
        // TODO (kaniandr@gmail.com): at this moment dead assignments could be
        // removed inside a compound statement only, so it is safe to remove
        // ending semicolon.
        Token SemiTok;
        if (!getRawTokenAfter(SrcMgr.getFileLoc(S->getEndLoc()),
            SrcMgr, LangOpts, SemiTok) && SemiTok.is(tok::semi))
          mRewriter->RemoveText(SemiTok.getLocation(), RemoveEmptyLine);
      }
      if (I->second.Scope && isa<CompoundStmt>(I->second.Scope)) {
        Token SemiTok;
        if (!getRawTokenAfter(SrcMgr.getFileLoc(I->first->getEndLoc()),
            SrcMgr, LangOpts, SemiTok) && SemiTok.is(tok::semi))
          mRewriter->RemoveText(SemiTok.getLocation(), RemoveEmptyLine);
      }
    }
  }

#ifdef LLVM_DEBUG
  void printRemovedDecls() {
    for (auto &D : mDeadDecls)
      dbgs() << D.first->getName() << "(" << D.first <<") ";
    dbgs() << "\n";
  }
#endif

private:
  /// Return current scope.
  Stmt *getScope() {
    for (auto I = mScopes.rbegin(), EI = mScopes.rend(); I != EI; ++I)
      if (isa<ForStmt>(*I) || isa<CompoundStmt>(*I))
        return *I;
    return nullptr;
  }

  std::map<NamedDecl *, DeclarationInfo> mDeadDecls;
  std::vector<Stmt*> mScopes;
  clang::Rewriter *mRewriter;
  DenseSet<DeclStmt*> mMultipleDecls;
  DenseMap<const clang::Type *, decltype(mDeadDecls)::const_iterator> mTypeDecls;
  DeclRefExpr *mSimpleAssignLHS = nullptr;
};
}

bool ClangDeadDeclsElimination::runOnFunction(Function &F) {
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
    F.getContext().emitError("cannot transform sources"
                              ": transformation context is not available");
    return false;
  }
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  DeclVisitor Visitor(TfmCtx->getRewriter());
  Visitor.TraverseDecl(FuncDecl);
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  LLVM_DEBUG(
    dbgs() << "[DEAD DECLS ELIMINATION]: list of all dead declarations ";
    Visitor.printRemovedDecls());
  auto GI{GIP.getGlobalInfo(TfmCtx)};
  assert(GI && "Raw info must be available!");
  Visitor.eliminateDeadDecls(GI->RI);
  LLVM_DEBUG(
    dbgs() << "[DEAD DECLS ELIMINATION]: list of removed declarations ";
    Visitor.printRemovedDecls());
  return false;
}

void ClangDeadDeclsElimination::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangDeadDeclsElimination() {
  return new ClangDeadDeclsElimination();
}
