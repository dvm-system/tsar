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
	std::set<std::string> names;
	std::map<clang::Decl*,std::string> change;
	std::list<clang::Decl*> del_lst;
	clang::Rewriter &rwr;
	public:
	explicit DeclVisitor(tsar::TransformationContext * a):rwr(a->getRewriter()){}
	//это вспомогательная функция
	void clr_del_lst(){
		auto decl=del_lst.back();
		del_lst.pop_back();
		change.erase(change.find(decl));
	}
	bool TraverseCompoundStmt(CompoundStmt * S){
		int d=del_lst.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseCompoundStmt(S);
		while (del_lst.size()!=d) clr_del_lst();
		return true;
	}
	bool TraverseForStmt(ForStmt * S){
		int d=del_lst.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseForStmt(S);
		while (del_lst.size()!=d) clr_del_lst();
		return true;
	}
	bool TraverseIfStmt(IfStmt * S){
		int d=del_lst.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseIfStmt(S);
		while (del_lst.size()!=d) clr_del_lst();
		return true;
	}
	bool TraverseWhileStmt(WhileStmt * S){
		int d=del_lst.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseWhileStmt(S);
		while (del_lst.size()!=d) clr_del_lst();
		return true;
	}
	bool TraverseDoStmt(DoStmt * S){
		int d=del_lst.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseDoStmt(S);
		while (del_lst.size()!=d) clr_del_lst();
		return true;
	}
	bool TraverseFunctionDecl(FunctionDecl * S){
		auto copy_names=names;
		int d=del_lst.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseFunctionDecl(S);
		while (del_lst.size()!=d) clr_del_lst();
		names=copy_names;
		return true;
	}
	bool VisitDeclRefExpr(DeclRefExpr * V){
		std::string name=((V->getNameInfo()).getName()).getAsString();
		clang::Decl * ptr=V->getFoundDecl();
		auto it=change.find(ptr);
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
			//вставляем новое соотстветствие
			change.insert(std::pair<clang::Decl*,std::string>(V,buf));
			//добавляем элемент в список на удаление
			del_lst.push_back(V);
			//заменяем имя в тексте
			rwr.ReplaceText(V->getLocation(),name.length(), buf);
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
