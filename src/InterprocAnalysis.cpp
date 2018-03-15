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
    runOnSCC(CGSCC, M);
    ++CGSCCIter;
  }
  return false;
}

void InterprocAnalysisPass::runOnSCC(CallGraphSCC &SCC, llvm::Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return;
  }
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  LibFunc LibID;
  std::set<Function *> SetSCCFunc;
  std::set<Function *> SetCalleeFunc;
  bool IsLibFunc = false;
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    SetSCCFunc.insert(F);
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    for (auto CFGTo : *CGN) {
      auto FTo = CFGTo.second->getFunction();
      if (SetSCCFunc.find(FTo) == SetSCCFunc.end())
        SetCalleeFunc.insert(FTo);
    }
  }
  for (auto SCCF : SetSCCFunc)
    if (TLI.getLibFunc(*SCCF, LibID) ||
        !TfmCtx->getDeclForMangledName(SCCF->getName()) ||
        !TfmCtx->getDeclForMangledName(SCCF->getName())->hasBody()) {
      IsLibFunc = true;
      break;
    }
  if (!IsLibFunc)
    for (auto CF : SetCalleeFunc)
      if (!CF || TLI.getLibFunc(*CF, LibID)) {
        IsLibFunc = true;
        break;
      }
  for (auto Attr : AndAttrs) {
    bool Val = false;
    for (auto SCCF : SetSCCFunc)
      if (!SCCF->hasFnAttribute(Attr) ||
          !TfmCtx->getDeclForMangledName(SCCF->getName()) ||
          !TfmCtx->getDeclForMangledName(SCCF->getName())->hasBody()) {
        Val = true;
        break;
      }
    if (!Val)
      for (auto CF : SetCalleeFunc)
        if (!CF || !CF->hasFnAttribute(Attr)) {
          Val = true;
          break;
        }
    if (Val)
      for (auto SCCF : SetSCCFunc)
        if (SCCF->hasFnAttribute(Attr))
          SCCF->removeFnAttr(Attr);
  }
  for (auto Attr : OrAttrs) {
    bool Val = false;
    for (auto SCCF : SetSCCFunc)
      if (SCCF->hasFnAttribute(Attr) ||
          !TfmCtx->getDeclForMangledName(SCCF->getName()) ||
          !TfmCtx->getDeclForMangledName(SCCF->getName())->hasBody()) {
        Val = true;
        break;
      }
    if (!Val)
      for (auto CF : SetCalleeFunc)
        if (!CF || CF->hasFnAttribute(Attr)) {
          Val = true;
          break;
        }
    if (Val)
      for (auto SCCF : SetSCCFunc)
        if (!SCCF->hasFnAttribute(Attr))
          SCCF->addFnAttr(Attr);
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    std::vector<CallExpr *> VecCallExpr;
    std::set<Function *> SetCalleeFunc;
    InterprocFuncInfo IFI;
    IFI.setLibFunc(IsLibFunc);
    auto D = TfmCtx->getDeclForMangledName(F->getName());
    if (!D || !D->hasBody())
      continue;
    getCallExprFromStmtTree(D->getBody(), VecCallExpr);
    for (auto CFGTo : *CGN)
      SetCalleeFunc.insert(CFGTo.second->getFunction());
    for (auto CalleeFunc : SetCalleeFunc) {
      if (!CalleeFunc)
        continue;
      std::vector<SourceLocation> VecSL;
      for (auto CE : VecCallExpr)
        if (CE->getDirectCallee()->getName().equals(CalleeFunc->getName()))
          VecSL.push_back(CE->getLocStart());
      IFI.addCalleeFunc(CalleeFunc, VecSL);
    }
    mInterprocAnalysisInfo.insert(std::make_pair(F, IFI));
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
