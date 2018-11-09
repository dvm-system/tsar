#include "test.h"
#include <list>
#include <iterator>
#include <string>
#include <set>
#include "tsar_transformation.h"
#include <llvm/Support/raw_ostream.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include "DFRegionInfo.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/IR/Module.h>
#include "tsar_query.h"
/*
 * по поводу мап
 * отображение VarDecl * -> string не позволяет делать поиск через find или count
 * все равно придется разыменовывать указатель и смотреть имя
 *  
 * если использовать ClangGlobalInfoPass
 * вероятно, выгодно вернуться к функциональному проходу
 * 
 * 
 * TO DO
 * как улучшить траверсы
 * подумать про DeclContext
 * сделать через ллвм мап
 */
using namespace llvm;
using namespace clang;
using namespace tsar;
char testpass::ID=0;//значение не важно для каждого прохода это своя переменная


INITIALIZE_PASS_IN_GROUP_BEGIN(testpass,"Korchagintestpass","Korchagintestpass description",false,false,tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass) //нужне include
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass);
INITIALIZE_PASS_IN_GROUP_END  (testpass,"Korchagintestpass","Korchagintestpass description",false,false,tsar::TransformationQueryManager::getPassRegistry())

void testpass::getAnalysisUsage(AnalysisUsage & AU) const{//дублирует описание зависимостей в макросах нужно обязательно
	//указание зависимости
	AU.addRequired<TransformationEnginePass>();
	//AU.setPreservesAll();//проход ничего не меняет
}

ModulePass * llvm::createtestpass(){
	return new testpass();
}



class DeclVisitor : public RecursiveASTVisitor <DeclVisitor> {
	public:
	std::set<std::string> names;
	std::map<std::string,std::string> change;
	clang::Rewriter &rwr;
	tsar::TransformationContext * TfmCtx;
	public:
	explicit DeclVisitor(tsar::TransformationContext * a):rwr(a->getRewriter()){
		TfmCtx=a;
	}
	bool TraverseCompoundStmt(CompoundStmt * S){
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseCompoundStmt(S);
		change=copy_change;
		return true;
	}
	bool TraverseForStmt(ForStmt * S){
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseForStmt(S);
		change=copy_change;
		return true;
	}
	bool TraverseIfStmt(IfStmt * S){
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseIfStmt(S);
		change=copy_change;
		return true;
	}
	bool TraverseWhileStmt(WhileStmt * S){
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseWhileStmt(S);
		change=copy_change;
		return true;
	}
	bool TraverseFunctionDecl(FunctionDecl * S){
		auto copy_names=names;
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseFunctionDecl(S);
		names=copy_names;
		change=copy_change;
		return true;
	}
	bool VisitDeclRefExpr(DeclRefExpr * V){
		std::string name=((V->getNameInfo()).getName()).getAsString();
		auto it=change.find(name);
		if (it!=change.end()){
			rwr.ReplaceText(V->getLocation(),name.length(), it->second);
		}
		return true;
	}
	bool VisitVarDecl(VarDecl * V){
		std::string name=V->getName();
		std::string buf;
		unsigned int count=1;
		//ищем повторы
		if (names.count(name)){
			//нахдим новое имя
			while (names.count(name+std::to_string(count))) count++;
			buf=name+std::to_string(count);
			//вставляем новое имя в список имен
			names.insert(name+std::to_string(count));
			//ищем старое имя в списке замеH
			auto it=change.find(name);
			if (it!=change.end()){
				it->second=buf;
				rwr.ReplaceText(V->getLocation(),name.length(), buf);
			}
			else {
				change.insert(std::pair<std::string,std::string>(name,buf));
				rwr.ReplaceText(V->getLocation(),name.length(), buf);
			}
		}
		else names.insert(name);
		
		//debug print
		//errs()<<"NAMES\n";
		//for (auto elem : change) errs()<<elem.first<<" TO "<<elem.second<<"\n";
		return true;
	}
};


bool testpass::runOnModule(Module & F){
	releaseMemory();	
	auto TfmCtx  = getAnalysis<TransformationEnginePass>().getContext(F);
	auto s=TfmCtx->getContext().getTranslationUnitDecl();
	//auto s=TfmCtx->getDeclForMangledName(F.getName());
	s->dump();//печатает все дерево
	DeclVisitor Vis(TfmCtx);
	Vis.TraverseDecl(s);
	releaseMemory();
	errs()<<"Korchagin test pass end\n";
	return false;

}
