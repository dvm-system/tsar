//=== UselessVariables.cpp - High Level Variables Loop Analyzer --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements classes to find declarations of uselsess variables
// in a source code.
//
//===----------------------------------------------------------------------===//



#include "DFRegionInfo.h"
#include "Diagnostic.h"
#include "UselessVariables.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

#include "NoMacroAssert.h"
#include "GlobalInfoExtractor.h"
#include "tsar_query.h"
#include "tsar_transformation.h"
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/DenseSet.h>


#include <iostream>
#include <assert.h>
#include <typeinfo>
#include <vector>
#include <stack>
#include <map>


using namespace llvm;
using namespace clang;

using namespace tsar;


#undef DEBUG_TYPE
#define DEBUG_TYPE "useless-vars"

char ClangUselessVariablesPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangUselessVariablesPass, "clang-useless-vars",
  "search usless vars (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)

INITIALIZE_PASS_IN_GROUP_END(ClangUselessVariablesPass, "clang-useless-vars",
  "search usless vars (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())



namespace {
/// This visits and analyzes all for-loops in a source code.
class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {
public:
  /// Creates visitor.
  explicit DeclVisitor(){}

  bool VisitVarDecl(VarDecl *D) {
    pDecls_map.insert(std::pair<VarDecl*, Stmt*>( D, stmt_stack.top() ) );
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *D) {
    if (isa<VarDecl>(D->getFoundDecl())) {
      auto it = pDecls_map.find( cast<VarDecl>(D->getFoundDecl()) );
      if(it != pDecls_map.end()) {
        pDecls_map.erase(it);
      }
    }
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S) {
    auto group = S->getDeclGroup();
    if(!S->isSingleDecl())
      pGroups_.insert(S);
    return true;
  }

  bool VisitIfStmt(IfStmt *S) {
    auto D = S->getConditionVariable();
    if (D != nullptr) {
      auto it = pDecls_map.find( D );
      if (it != pDecls_map.end()) {
        toDiag(mRewriter_->getSourceMgr().getDiagnostics(), \
          (it->first)->getLocation(), diag::warn_remove_useless_variables);
        pDecls_map.erase(it);
      }
    }
    return true;
  }

  bool findCallExpr(Stmt *S) {
    bool result = isa<CallExpr>(S);
    if (result == false) {
      for (auto i = S->child_begin(); i != S->child_end(); i++) {
        if (*i != nullptr) {
          result = this->findCallExpr(*i);
          if (result == true)
            return true;
        }
      }
      return result;
    } else
      return result;
  }

  bool TraverseStmt(Stmt *S, DataRecursionQueue *Queue=nullptr) {
    for (auto i = S->child_begin(); i != S->child_end(); i++) {

      if (*i != nullptr) {
        if ((isa<ForStmt>(*i)) || (isa<IfStmt>(*i)) || (isa<CompoundStmt>(*i)) \
         || (isa<DoStmt>(*i)) || (isa<WhileStmt>(*i)))
          stmt_stack.push(*i);

        if (isa<DeclStmt>(*i)) {
          DeclStmt* d_stmt = cast<DeclStmt>(*i);
          for (auto j = d_stmt->decl_begin(); j != d_stmt->decl_end(); j++) {
            if (*j != nullptr) {
              if( std::string((*j)->getDeclKindName()) == "Var") {
                this->VisitVarDecl(cast<VarDecl>(*j));
              }
            }
          }
          this->VisitDeclStmt(cast<DeclStmt>(*i));
        }

        if (isa<DeclRefExpr>(*i)) {
          this->VisitDeclRefExpr(cast<DeclRefExpr>(*i));
        }
        if (isa<IfStmt>(*i)) {
          this->VisitIfStmt(cast<IfStmt>(*i));
        }

        this->TraverseStmt(*i);

        if ((isa<ForStmt>(*i)) || (isa<IfStmt>(*i)) || (isa<CompoundStmt>(*i)) \
         || (isa<DoStmt>(*i)) || (isa<WhileStmt>(*i)))
          stmt_stack.pop();
      }

    }
    return true;
  }


  void DelVarsFromCode() {
    for (auto i = pDecls_map.begin(); i != pDecls_map.end();) {
      if (((i->first)->isLocalVarDecl() == 0)  \
        && ((i->first)->isLocalVarDeclOrParm() == 1)) {
        toDiag(mRewriter_->getSourceMgr().getDiagnostics(), \
          (i->first)->getLocation(), diag::warn_remove_useless_variables);
        i = pDecls_map.erase(i);
      } else
        i++;
    }


    //ignore declaration which init by function return value
    for (auto i = pDecls_map.begin(); i != pDecls_map.end();) {
      if ((i->first)->hasInit()) {
        if(findCallExpr((i->first)->getInit())) {
          toDiag(mRewriter_->getSourceMgr().getDiagnostics(), \
            (i->first)->getLocation(), diag::warn_remove_useless_variables);
          i = pDecls_map.erase(i);
        } else
          ++i;
      } else
        ++i;
    }

    //remove decl group of useless variables
    for (auto i = pGroups_.begin(); i != pGroups_.end(); i++) {
      auto group = (*i)->getDeclGroup();
      int group_size = 0, num_useless = 0;
      for(auto j = group.begin(); j != group.end(); j++) {
        group_size++;
        auto it = pDecls_map.find( cast<VarDecl>(*j) );
        if(it != pDecls_map.end()) {
          num_useless++;
        }
      }
      if (num_useless != group_size) {
        for(auto j = group.begin(); j != group.end(); j++) {
          auto it = pDecls_map.find( cast<VarDecl>(*j) );
          if(it != pDecls_map.end()) {
            toDiag(mRewriter_->getSourceMgr().getDiagnostics(), \
            (it->first)->getLocation(), diag::warn_remove_useless_variables);
            pDecls_map.erase(it);
          }
        }
      }

    }

    for (auto i = pDecls_map.begin(); i != pDecls_map.end(); i++)
      mRewriter_->RemoveText((i->first)->getSourceRange());
    mRewriter_->overwriteChangedFiles();

  }

  void print_decls() {
    dbgs() << "=======function pass========\n";
    dbgs() << "######### usless Useless Variables\n";
    for (auto i = pDecls_map.begin(); i != pDecls_map.end(); i++) {
      dbgs() << "DenseSet:  " << (i->first)->getNameAsString() \
      << "  " << (long)(i->first) <<"\n";
    }
    dbgs() << "\n\n";
  }

  void set_rewriter(clang::Rewriter *Rewriter) {
    mRewriter_ = Rewriter;
  }

  std::map<VarDecl*, Stmt*> pDecls_map;
  std::stack<Stmt*> stmt_stack;
  clang::Rewriter *mRewriter_;
  DenseSet<DeclStmt*>pGroups_;

};
}


bool ClangUselessVariablesPass::runOnFunction(Function &F) {
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;

  DeclVisitor Visitor;
  Visitor.set_rewriter( &(TfmCtx->getRewriter()));

  Visitor.stmt_stack.push(FuncDecl->getBody());
  Visitor.TraverseStmt(FuncDecl->getBody());

  auto &GIP = getAnalysis<ClangGlobalInfoPass>();

  //search macros in statement
  for (auto i = Visitor.pDecls_map.begin(); i != Visitor.pDecls_map.end();) {
    bool flag = false;
    for_each_macro(i->second, TfmCtx->getContext().getSourceManager(), \
      TfmCtx->getContext().getLangOpts(), GIP.getRawInfo().Macros, \
      [&flag](SourceLocation x){flag = true;});
    if (flag) {
      toDiag(Visitor.mRewriter_->getSourceMgr().getDiagnostics(), \
          (i->first)->getLocation(), diag::warn_remove_useless_variables);
      i = Visitor.pDecls_map.erase(i);
    }
    else
      ++i;
  }

  LLVM_DEBUG(Visitor.print_decls());

  Visitor.DelVarsFromCode();

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
