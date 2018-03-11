#include "InterprocAnalysis.h"
#include "tsar_transformation.h"
#include <clang/AST/Stmt.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <vector>
#include <set>

using namespace llvm;
using namespace clang;

char InterprocAnalysisPass::ID = 0;
INITIALIZE_PASS_BEGIN(InterprocAnalysisPass, "interproc-analysis",
    "Interprocedural Analysis Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(InterprocAnalysisPass, "interproc-analysis",
  "Interprocedural Analysis Pass", false, false)

namespace {
void getCallExprFromStmtTree(Stmt *S, std::vector<CallExpr *> &VCE) {
  if (!S)
    return;
  if (!strcmp(S->getStmtClassName(), "CallExpr"))
    VCE.push_back(cast<CallExpr>(S));
  for (Stmt *SChild : S->children())
    getCallExprFromStmtTree(SChild, VCE);
}
}

bool InterprocAnalysisPass::runOnModule(llvm::Module &M) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  scc_iterator<CallGraph *> CGSCCIter = scc_begin(&CG);
  CallGraphSCC CGSCC(CG, &CGSCCIter);
  while (!CGSCCIter.isAtEnd()) {
    const std::vector<CallGraphNode *> &VecCGN = *CGSCCIter;
    CGSCC.initialize(VecCGN);
    runOnSCC(CGSCC);
    ++CGSCCIter;
  }
  return false;
}

void InterprocAnalysisPass::runOnSCC(CallGraphSCC &SCC) {
  auto &M = SCC.getCallGraph().getModule();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  LibFunc LibID;
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F || TLI.getLibFunc(*F, LibID))
      continue;
    auto D = TfmCtx->getDeclForMangledName(F->getName());
    if (!D)
      continue;
    std::vector<FuncCallee> VecFuncCallee;
    std::vector<CallExpr *> VecCallExpr;
    std::set<Function *> SetFunc;
    getCallExprFromStmtTree(D->getBody(), VecCallExpr);
    for (auto CFGTo : *CGN)
      SetFunc.insert(CFGTo.second->getFunction());
    for (auto SF : SetFunc) {
      if (!SF)
        continue;
      FuncCallee FC(SF);
      for (auto CE : VecCallExpr)
        if (CE->getDirectCallee()->getName().equals(FC.Callee->getName()))
          FC.Locations.push_back(CE->getLocStart());
      VecFuncCallee.push_back(FC);
    }
    mInterprocAnalysisInfo.insert(std::make_pair(F, VecFuncCallee));
  }
}
void InterprocAnalysisPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}
ModulePass *llvm::createInterprocAnalysisPass() {
  return new InterprocAnalysisPass();
}
