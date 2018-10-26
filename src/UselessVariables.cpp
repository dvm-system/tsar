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


//INITIALIZE_PASS_BEGIN(ClangUselessVariablesPass, "useless-vars", "Searching of useless vars", true, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
//INITIALIZE_PASS_END(ClangUselessVariablesPass, "useless-vars", "Searching of useless vars", true, true)


INITIALIZE_PASS_IN_GROUP_END(ClangUselessVariablesPass, "clang-useless-vars",
  "search usless vars (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())



namespace {
/// This visits and analyzes all for-loops in a source code.
class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {
public:
  /// Creates visitor.
  explicit DeclVisitor(){}
  
  bool VisitVarDecl(VarDecl *D)
  {
    //std::cout << "&&&&&Var" << std::endl;
    pDecls_.insert(D);
    pDecls_map.insert(std::pair<VarDecl*, Stmt*>( D, stmt_stack.top() ) );
    return true;    
  }

  bool VisitDeclRefExpr(DeclRefExpr *D)
  {
    //std::cout << "&&&&&&&&" << std::endl;
    //сделать нормальное приведение типов
    //что лучше использоавть getdecl или getfounddecl?
    pDecls_.erase(   (VarDecl*)(D->getFoundDecl())   );
    auto it = pDecls_map.find( (VarDecl*)(D->getFoundDecl()) );
    if(it != pDecls_map.end())
      pDecls_map.erase(it);
    //if(D != NULL)
    //  std::cout << "decl ref name = " /*<< ((VarDecl*)(D))->getNameAsString()*/ << "    " <<(long)((VarDecl*)(D->getDecl())) << std::endl;
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S)
  {
    //std::cout << "&&&&&&&&" << std::endl;
    //std::cout << "Decl statement:  " << S->isSingleDecl() << std::endl;

    auto group = S->getDeclGroup();

    if(S->isSingleDecl())
    {
      //сделать нормальное приведение типов
      //auto d = (VarDecl*)(S->getSingleDecl());
      //std::cout << "visit decl stmt:   " << pDecls_.count(d) << std::endl;
    }
    else
    {
      //Добавить warning, когда нашлось множественное объявление
      for(auto i = group.begin(); i != group.end(); i++)
      {
        bool res = pDecls_.erase(  (VarDecl*)(*i)  );

        auto it = pDecls_map.find( (VarDecl*)(*i) );
        if(it != pDecls_map.end())
          pDecls_map.erase(it);
        //std::cout << "group var decl   " << (long)(*i) << " " << res << std::endl;
      }
    }

    return true;
  }

  bool VisitIfStmt(IfStmt *S)
  {
    //std::cout << "&&&&&&&&" << std::endl;
    //std::cout << "IfStmt" << std::endl;
    auto D = S->getConditionVariable();
    if(D != NULL)
    {
      pDecls_.erase(D);
      auto it = pDecls_map.find( D );
      if(it != pDecls_map.end())
        pDecls_map.erase(it);
    }
    return true;
  }

  bool TraverseStmt(Stmt *S, DataRecursionQueue *Queue=nullptr)
  {
    //std::cout << "TraverseStmt" << std::endl;
    for(auto i = S->child_begin(); i != S->child_end(); i++)
    {

      if(*i != NULL)
      {
        
        std::string classname = i->getStmtClassName();

        if((classname == "ForStmt") || (classname == "IfStmt") || (classname == "CompoundStmt") || (classname == "DoStmt") \
           || (classname == "WhileStmt"))
          stmt_stack.push(*i);

        if(classname == "DeclStmt")
        {
          //std::cout << "decl stmt" << std::endl;
          DeclStmt* d_stmt = (DeclStmt*)(*i);
          for(auto j = d_stmt->decl_begin(); j != d_stmt->decl_end(); j++)
          {
            if(*j != NULL)
            {
              //std::cout << (*j)->getDeclKindName() << "###" << std::endl;

              if( std::string((*j)->getDeclKindName()) == "Var")
              {
                //std::cout << "____________" << std::endl;
                this->VisitVarDecl((VarDecl*)(*j));
              }
            }
          }

          this->VisitDeclStmt((DeclStmt*)(*i));

        }

        if(classname == "DeclRefExpr")
        {
          //std::cout << "decl ref expr" << std::endl;
          this->VisitDeclRefExpr((DeclRefExpr*)(*i));
        }
        if(classname == "IfStmt")
        {
          //std::cout << "if Stmt" << std::endl;
          this->VisitIfStmt((IfStmt*)(*i));
        }


        this->TraverseStmt(*i);

        if((classname == "ForStmt") || (classname == "IfStmt") || (classname == "CompoundStmt") || (classname == "DoStmt") \
           || (classname == "WhileStmt"))
          stmt_stack.pop();
      }


      
    }
    return true;
  }


  void DelVarsFromCode(clang::Rewriter &mRewriter)
  {
    for(auto i = pDecls_.begin(); i != pDecls_.end(); i++)
    {
      //std::cout << "Del decl:  " << (*i)->getNameAsString() << std::endl;

      //std::cout << "check decl :  " << (*i)->isLocalVarDecl() << std::endl;
      //std::cout << "check param :  " << (*i)->isLocalVarDeclOrParm() << std::endl;
      
      if(((*i)->isLocalVarDecl() == 0) && ((*i)->isLocalVarDeclOrParm() == 1))
        pDecls_.erase(i);
      //попробовать проверку через location
    }


    for(auto i = pDecls_map.begin(); i != pDecls_map.end(); i++)
    {
      if(((i->first)->isLocalVarDecl() == 0) && ((i->first)->isLocalVarDeclOrParm() == 1))
      {
          pDecls_map.erase(i);
      }
    }


    //for(auto i = pDecls_.begin(); i != pDecls_.end(); i++)
    //  mRewriter.RemoveText((*i)->getSourceRange());
    


    //after using this function clang delete declarations
    // but add some strange pragmas 
    //mRewriter.overwriteChangedFiles();

  }



  void print_decls()
  {
    //std::cout << "map size ___ " << pDecls_map.size() << std::endl;
    for(auto i = pDecls_.begin(); i != pDecls_.end(); i++)
    {
      std::cout << "DenseSet:  " << (*i)->getNameAsString() << "  " << (long)(*i) <<std::endl;
    }

    for(auto i = pDecls_map.begin(); i != pDecls_map.end(); i++)
    {
      std::cout << "DenseSet:  " << (i->first)->getNameAsString() << "  " << (long)(i->first) <<std::endl;
    }


  }

  //переписать через DenseMap
  std::map<VarDecl*, Stmt*> pDecls_map;
  std::stack<Stmt*> stmt_stack;
private:

  DenseSet<VarDecl*>pDecls_;
  
  
};
}


bool ClangUselessVariablesPass::runOnFunction(Function &F) {
  std::cout << std::endl << std::endl << "=======function pass========" << std::endl;
 
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;

  DeclVisitor Visitor;
  //Visitor.TraverseDecl(FuncDecl);
  //Visitor.print_decls();


  Visitor.stmt_stack.push(FuncDecl->getBody());


  //Visitor.TraverseStmt(FuncDecl);
  Visitor.TraverseStmt(FuncDecl->getBody());

  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  //std::cout << "map size " << Visitor.pDecls_map.size() << std::endl;
  for(auto i = Visitor.pDecls_map.begin(); i != Visitor.pDecls_map.end(); i++)
  {
    bool flag = false;
    for_each_macro(i->second, TfmCtx->getContext().getSourceManager(), \
      TfmCtx->getContext().getLangOpts(), GIP.getRawInfo().Macros, \
      [&flag](SourceLocation x){flag = true;});

    //std::cout << (i->first)->getNameAsString() << " " << (i->second)->getStmtClassName() << " " << flag << std::endl;
    if(flag)
      Visitor.pDecls_map.erase(i);
  }

  //Visitor.print_decls();

  std::cout << "######### usless Useless Variables" << std::endl;
  Visitor.print_decls();

  

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
