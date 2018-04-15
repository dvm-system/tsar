#include "CalleeProcLocation.h"
#include "tsar_loop_matcher.h"
#include "tsar_pass_provider.h"
#include "tsar_transformation.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <set>

using namespace llvm;
using namespace clang;

typedef FunctionPassProvider<
  TransformationEnginePass,
  LoopMatcherPass> CalleeProcLocationProvider;

INITIALIZE_PROVIDER_BEGIN(CalleeProcLocationProvider, "callee-proc-location-provider",
  "Callee Procedures Locations Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PROVIDER_END(CalleeProcLocationProvider, "callee-proc-location-provider",
  "Callee Procedures Locations Provider")

char CalleeProcLocationPass::ID = 0;
INITIALIZE_PASS_BEGIN(CalleeProcLocationPass, "callee-proc-location",
  "Callee Procedures Location Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(CalleeProcLocationProvider)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(CalleeProcLocationPass, "callee-proc-location",
  "Callee Procedures Location Pass", false, false)

namespace {
void setFuncLoc(std::vector<CallExpr *> VecCallExpr,
    std::set<Function *> SetCalleeFunc,
    tsar::CalleeProcLocation &CPL) {
  for (auto CalleeFunc : SetCalleeFunc) {
    std::vector<SourceLocation> VecSL;
    for (auto CE : VecCallExpr)
      if (CE->getDirectCallee()->getName().equals(CalleeFunc->getName()))
        VecSL.push_back(CE->getExprLoc());
    CPL.addCalleeFunc(CalleeFunc, VecSL);
  }
}

void setLoopCPL(llvm::Module &M, Stmt *S,
    tsar::StmtCalleeProcInfo &LCPI) {
  tsar::CalleeProcLocation CPL;
  std::vector<CallExpr *> VecCallExpr;
  std::vector<BreakStmt *> VecBreakStmt;
  std::vector<ReturnStmt *> VecReturnStmt;
  std::vector<GotoStmt *> VecGotoStmt;
  std::set<Function *> SetCalleeFunc;
  std::set<Stmt::StmtClass> SetLoopStmt = {
    Stmt::StmtClass::ForStmtClass,
    Stmt::StmtClass::WhileStmtClass,
    Stmt::StmtClass::DoStmtClass
  };
  clang::getStmtTypeFromStmtTree(S, VecCallExpr);
  clang::getStmtTypeFromStmtTree(S, VecBreakStmt, SetLoopStmt);
  clang::getStmtTypeFromStmtTree(S, VecReturnStmt);
  clang::getStmtTypeFromStmtTree(S, VecGotoStmt);
  for (auto CE : VecCallExpr)
    SetCalleeFunc.insert(M.getFunction(CE->getDirectCallee()->getName()));
  if (!VecBreakStmt.empty()) {
    std::set<clang::SourceLocation> VSL;
    for (auto BS : VecBreakStmt)
      VSL.insert(BS->getBreakLoc());
    CPL.setBreak(VSL);
  }
  if (!VecReturnStmt.empty()) {
    std::set<clang::SourceLocation> VSL;
    for (auto RS : VecReturnStmt)
      VSL.insert(RS->getReturnLoc());
    CPL.setReturn(VSL);
  }
  if (!VecGotoStmt.empty()) {
    std::set<clang::SourceLocation> VSL;
    for (auto GS : VecGotoStmt)
      if (GS->getLabel()->getLocation() < S->getLocStart() ||
          S->getLocEnd() < GS->getLabel()->getLocation())
        VSL.insert(GS->getGotoLoc());
    CPL.setGoto(VSL);
  }
  setFuncLoc(VecCallExpr, SetCalleeFunc, CPL);
  LCPI.insert(std::make_pair(S, CPL));
}
}

bool CalleeProcLocationPass::runOnModule(llvm::Module &M) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  scc_iterator<CallGraph *> CGSCCIter = scc_begin(&CG);
  CallGraphSCC CGSCC(CG, &CGSCCIter);
  while (!CGSCCIter.isAtEnd()) {
    const std::vector<CallGraphNode *> &VecCGN = *CGSCCIter;
    CGSCC.initialize(VecCGN);
    runOnSCC(CGSCC, M);
    ++CGSCCIter;
  }
  return false;
}

void CalleeProcLocationPass::runOnSCC(CallGraphSCC &SCC, llvm::Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return;
  }
  CalleeProcLocationProvider::initialize<TransformationEnginePass>(
      [&M, &TfmCtx](TransformationEnginePass &TEP) {
    TEP.setContext(M, TfmCtx);
  });
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    auto D = TfmCtx->getDeclForMangledName(F->getName());
    if (!D)
      continue;
    std::vector<CallExpr *> VecCallExpr;
    std::set<Function *> SetCalleeFunc;
    tsar::CalleeProcLocation CPL;
    if (F->empty()) {
      mFuncCalleeProcInfo.insert(std::make_pair(F, CPL));
      continue;
    }
    getStmtTypeFromStmtTree(D->getBody(), VecCallExpr);
    for (auto CFGTo : *CGN)
      if (CFGTo.second->getFunction())
        SetCalleeFunc.insert(CFGTo.second->getFunction());
    setFuncLoc(VecCallExpr, SetCalleeFunc, CPL);
    mFuncCalleeProcInfo.insert(std::make_pair(F, CPL));
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F || F->empty())
      continue;
    auto &Provider = getAnalysis<CalleeProcLocationProvider>(*F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    for (auto Match : Matcher)
      setLoopCPL(M, Match.first, mLoopCalleeProcInfo);
    for (auto Unmatch : Unmatcher)
      setLoopCPL(M, Unmatch, mLoopCalleeProcInfo);
  }
}

void CalleeProcLocationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<CalleeProcLocationProvider>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}

ModulePass *llvm::createCalleeProcLocationPass() {
  return new CalleeProcLocationPass();
}
