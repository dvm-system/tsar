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
#include <clang/AST/ASTContext.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/IR/Module.h>
#include "tsar_query.h"
#include "tsar_pragma.h"

using namespace llvm;
using namespace clang;
using namespace tsar;
char testpass::ID=0;//значение не важно для каждого прохода это своя переменная


INITIALIZE_PASS_IN_GROUP_BEGIN(testpass,"Korchagintestpass",
		"Korchagintestpass description",false,false,
		tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass) //нужне include
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass);
INITIALIZE_PASS_IN_GROUP_END  (testpass,"Korchagintestpass",
		"Korchagintestpass description",false,false,
		tsar::TransformationQueryManager::getPassRegistry())


//дублирует описание зависимостей в макросах
void testpass::getAnalysisUsage(AnalysisUsage & AU) const{
	//указание зависимости
	AU.addRequired<TransformationEnginePass>();
	//AU.setPreservesAll();//проход ничего не меняет
}

ModulePass * llvm::createtestpass(){
	return new testpass();
}

class DeclVisitor : public RecursiveASTVisitor <DeclVisitor> {
	public:
	explicit DeclVisitor(tsar::TransformationContext * a):
		mRewriter(a->getRewriter()){}
	//это вспомогательная функция, вызывается в переопределениях
	void clr_mLstdel(){
		auto decl=mLstdel.back();
		mLstdel.pop_back();
		mChange.erase(mChange.find(decl));
	}
	bool TraverseCompoundStmt(CompoundStmt * S){
		int d=mLstdel.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseCompoundStmt(S);
		while (mLstdel.size()!=d) clr_mLstdel();
		return true;
	}
	bool TraverseForStmt(ForStmt * S){
		int d=mLstdel.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseForStmt(S);
		while (mLstdel.size()!=d) clr_mLstdel();
		return true;
	}
	bool TraverseIfStmt(IfStmt * S){
		int d=mLstdel.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseIfStmt(S);
		while (mLstdel.size()!=d) clr_mLstdel();
		return true;
	}
	bool TraverseWhileStmt(WhileStmt * S){
		int d=mLstdel.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseWhileStmt(S);
		while (mLstdel.size()!=d) clr_mLstdel();
		return true;
	}
	bool TraverseDoStmt(DoStmt * S){
		int d=mLstdel.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseDoStmt(S);
		while (mLstdel.size()!=d) clr_mLstdel();
		return true;
	}
	bool TraverseFunctionDecl(FunctionDecl * S){
		auto copy_mNames=mNames;
		int d=mLstdel.size();
		RecursiveASTVisitor<DeclVisitor>::TraverseFunctionDecl(S);
		while (mLstdel.size()!=d) clr_mLstdel();
		mNames=copy_mNames;
		return true;
	}
	bool VisitDeclRefExpr(DeclRefExpr * V){
		std::string Name=((V->getNameInfo()).getName()).getAsString();
		clang::Decl * ptr=V->getFoundDecl();
		auto it=mChange.find(ptr);
		if (it!=mChange.end()){
			mRewriter.ReplaceText(V->getLocation(),Name.length(), it->second);
			errs()<<"Replace "<<Name<<" to "<<it->second;
		}
		return true;
	}

	bool VisitVarDecl(VarDecl * V){
		std::string Name=V->getName();
		std::string Buf;
		unsigned int count=1;
		//ищем повторы
		if (mNames.count(Name)){
			//нахдим новое имя
			while (mNames.count(Name+std::to_string(count))) count++;
			Buf=Name+std::to_string(count);
			//вставляем новое имя в список имен
			mNames.insert(Name+std::to_string(count));
			//вставляем новое соотстветствие
			mChange.insert(std::pair<clang::Decl*,std::string>(V,Buf));
			//добавляем элемент в список на удаление
			mLstdel.push_back(V);
			//заменяем имя в тексте
			mRewriter.ReplaceText(V->getLocation(),Name.length(), Buf);
		}
		else mNames.insert(Name);
		errs()<<"VISIT VARDECL\n";
		return true;
	}
	//для отладки
	void printchange(){
		errs()<<"mChange\n";
		for (auto each : mChange){
			errs()<<((VarDecl*)each.first)->getName()<<" "<<each.second<<"\n";
		}
	}
	void printnames(){
		errs()<<"mNames\n";
		for (auto each : mNames){
			errs()<<each<<"\n";
		}
	}
	private:
	//список всех имен
	std::set<std::string> mNames;
	//таблица соотвествий для VarDecl и замен имен
	std::map<clang::Decl*,std::string> mChange;
	//список тех VarDecl, которые нужно удалить по выходу из траверса
	std::list<clang::Decl*> mLstdel;
	clang::Rewriter &mRewriter;
};

//этот визитер обходит дерево в поиске прагмы
//если обнаруживает, то вызывает DeclVisitor для следующего за прагмой
//CompoundStmt
class RenameChecker : public RecursiveASTVisitor <RenameChecker>{
	public:
	RenameChecker(tsar::TransformationContext * TfmCtx){
		mTfmCtx=TfmCtx;
		flag=false;
	}
	bool TraverseCompoundStmt(clang::CompoundStmt * S){
		if (flag) {
			flag=false;
			DeclVisitor Vis(mTfmCtx);
			Vis.TraverseCompoundStmt(S);
			return true;
		}
		SmallVector<Stmt *, 1> Clauses;
		Pragma P(*S);
		if (findClause(P, ClauseId::Rename, Clauses)) {
			flag=true;
			return true;
		}
		else
			RecursiveASTVisitor::TraverseCompoundStmt(S);
		return true;
	}
	private:
	tsar::TransformationContext * mTfmCtx;
	bool flag;
};

bool testpass::runOnModule(Module & F){
	releaseMemory();	
	auto TfmCtx  = getAnalysis<TransformationEnginePass>().getContext(F);
	//auto s=TfmCtx->getContext().getTranslationUnitDecl();
	//auto s=TfmCtx->getDeclForMangledName(F.getName());
	//s->dump();//печатает все дерево
	RenameChecker Vis(TfmCtx);
	Vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
	releaseMemory();
	errs()<<"Korchagin test pass end\n";
	return false;

}
