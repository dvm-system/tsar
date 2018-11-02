//=== UselessVariables.cpp - High Level Variables Loop Analyzer --------*- C++ -*-===//
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
#include <llvm/Support/Casting.h>

#include <iostream>
#include <assert.h>
#include <typeinfo>
#include <vector>
#include <stack>
#include <map>


using namespace llvm;
using namespace clang;

using namespace tsar;

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
    pDecls_.insert(D);
    pDecls_map.insert(std::pair<VarDecl*, Stmt*>( D, stmt_stack.top() ) );
    return true;    
  }

  bool VisitDeclRefExpr(DeclRefExpr *D) {
    pDecls_.erase(   (VarDecl*)(D->getFoundDecl())   );
    auto it = pDecls_map.find( (VarDecl*)(D->getFoundDecl()) );
    if(it != pDecls_map.end())
      pDecls_map.erase(it);
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S) {
    auto group = S->getDeclGroup();

    if(S->isSingleDecl()) {

    } else {
      //Добавить warning, когда нашлось множественное объявление
      for(auto i = group.begin(); i != group.end(); i++) {
        bool res = pDecls_.erase(  cast<VarDecl>(*i)  );

        auto it = pDecls_map.find( cast<VarDecl>(*i) );
        if(it != pDecls_map.end())
          pDecls_map.erase(it);
      }
    }

    return true;
  }

  bool VisitIfStmt(IfStmt *S) {
    auto D = S->getConditionVariable();
    if (D != nullptr) {
      pDecls_.erase(D);
      auto it = pDecls_map.find( D );
      if (it != pDecls_map.end())
        pDecls_map.erase(it);
    }
    return true;
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


  void DelVarsFromCode(clang::Rewriter &mRewriter) {
    for (auto i = pDecls_.begin(); i != pDecls_.end(); i++) {      
      if (((*i)->isLocalVarDecl() == 0) && ((*i)->isLocalVarDeclOrParm() == 1))
        pDecls_.erase(i);
    }


    for (auto i = pDecls_map.begin(); i != pDecls_map.end(); i++) {
      if (((i->first)->isLocalVarDecl() == 0)  \
        && ((i->first)->isLocalVarDeclOrParm() == 1)) {
          pDecls_map.erase(i);
      }
    }

/*
    //'этот кусок игнорирует переменные, которые инициализируются функцией
    for (auto i = pDecls_map.begin(); i != pDecls_map.end(); i++) {  
      if ((i->first)->hasInit()) {
        if (!isa<CallExpr>((i->first)->getInit())) {
          pDecls_map.erase(i);
        }
      }
    }
*/

    for (auto i = pDecls_.begin(); i != pDecls_.end(); i++)
      mRewriter.RemoveText((*i)->getSourceRange());
    mRewriter.overwriteChangedFiles();

  }



  void print_decls() {
    std::cout << std::endl << std::endl << "=======function pass========" << std::endl;
    std::cout << "######### usless Useless Variables" << std::endl;
    for (auto i = pDecls_map.begin(); i != pDecls_map.end(); i++) {
      std::cout << "DenseSet:  " << (i->first)->getNameAsString() \
      << "  " << (long)(i->first) <<std::endl;
    }
  }
  std::map<VarDecl*, Stmt*> pDecls_map;
  std::stack<Stmt*> stmt_stack;
private:
  DenseSet<VarDecl*>pDecls_;
  
  
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

  Visitor.stmt_stack.push(FuncDecl->getBody());
  Visitor.TraverseStmt(FuncDecl->getBody());

  auto &GIP = getAnalysis<ClangGlobalInfoPass>();

  for (auto i = Visitor.pDecls_map.begin(); i != Visitor.pDecls_map.end(); i++) {
    bool flag = false;
    for_each_macro(i->second, TfmCtx->getContext().getSourceManager(), \
      TfmCtx->getContext().getLangOpts(), GIP.getRawInfo().Macros, \
      [&flag](SourceLocation x){flag = true;});
    if (flag)
      Visitor.pDecls_map.erase(i);
  }
  //Visitor.print_decls();

  Visitor.DelVarsFromCode(TfmCtx->getRewriter());

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
