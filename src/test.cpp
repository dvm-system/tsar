#include "test.h"
#include <list>
#include <iterator>
#include <string>
#include <set>
#include <tuple>
#include "tsar_transformation.h"
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Support/raw_ostream.h>


#include <clang/Rewrite/Core/Rewriter.h>
#include "PerfectLoop.h"
#include "DFRegionInfo.h"
#include "tsar_transformation.h"
#include "tsar_loop_matcher.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
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
	//AU.addRequired<LoopInfoWrapperPass>(); //указание зависимости
	AU.addRequired<TransformationEnginePass>();
	//AU.setPreservesAll();//проход ничего не меняет
}

ModulePass * llvm::createtestpass(){
	return new testpass();
}


class DeclVisitor : public RecursiveASTVisitor <DeclVisitor> {
	public:
	std::set<std::string> names;
	std::list<std::pair<std::string,std::string>> change;
	clang::Rewriter &rwr;
	tsar::TransformationContext * TfmCtx;
	public:
	
	explicit DeclVisitor(tsar::TransformationContext * a):rwr(a->getRewriter()){
		TfmCtx=a;
	}
	
	bool TraverseCompoundStmt(CompoundStmt * S){
		auto copy_names=names;
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseCompoundStmt(S);
		names=copy_names;
		change=copy_change;
		return true;
	}
	bool TraverseForStmt(ForStmt * S){
		auto copy_names=names;
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseForStmt(S);
		names=copy_names;
		change=copy_change;
		return true;
	}
	bool TraverseIfStmt(IfStmt * S){
		auto copy_names=names;
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseIfStmt(S);
		names=copy_names;
		change=copy_change;
		return true;
	}
	bool TraverseWhileStmt(WhileStmt * S){
		auto copy_names=names;
		auto copy_change=change;
		RecursiveASTVisitor<DeclVisitor>::TraverseWhileStmt(S);
		names=copy_names;
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
		for (auto elem : change){
			if (elem.first.compare(name)==0)
				//REWRITE
				rwr.ReplaceText(V->getLocation(),name.length(), elem.second);
				errs()<<"REWRITER CALLED IN DECLREF: "<<elem.first<<" TO "<<elem.second<<"\n";
		}
		return true;
	}

	bool VisitVarDecl(VarDecl * V){
		auto & rew=TfmCtx->getRewriter();
		std::string name=V->getName();
		errs()<<name<<"\n";
		std::string buf;
		unsigned int count=1;
		bool flag=true;
		int nnp=0;
		
		errs()<<"WE SEARCH "<<name<<" IN \n";
		errs()<<"NAMES\n";
		for (auto elem: names){
			errs()<<elem<<"\n";
		}
		errs()<<"WE FIND "<<names.count(name)<<"\n";
		
		//ищем повторы
		if (names.count(name)){
			errs()<<"GDE?!"<<nnp<<"\n";
			nnp=1;
			//нахдим новое имя
			while (names.count(name+std::to_string(count))) count++;
			buf=name+std::to_string(count);
			//вставляем новое имя в список имен
			names.insert(name+std::to_string(count));
			auto it=change.begin();
			//ищем старое имя в списке замен
			while (it!=change.end()){
				if ((*it).first.compare(name)==0){
					(*it).second=buf;
					flag=false;
					errs()<<"REWRITER CALLED IN VARDECL: "<<name<<" TO "<<buf<<"\n";
					rwr.ReplaceText(V->getLocation(),name.length(), buf);
					break;
				}
				++it;
			}
			if (flag) {
				change.push_back(*(new std::pair<std::string,std::string>(name,buf)));
				rew.ReplaceText(V->getLocation(),name.length(), buf);
				errs()<<"REWRITER CALLED IN VARDECL: "<<name<<" TO "<<buf<<"\n";
			}
		}
		else {names.insert(name); errs()<<"insert name "<<name<<"\n";}
		errs()<<"GDE?!"<<nnp<<"\n";

	
		
		return true;
	}
};

//bool what=false;

bool testpass::runOnModule(Module & F){
	releaseMemory();
	//if (what) return false;
	//what=true;
	
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
