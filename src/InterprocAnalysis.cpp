#include "InterprocAnalysis.h"
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
  LoopMatcherPass> InterprocProvider;

INITIALIZE_PROVIDER_BEGIN(InterprocProvider, "interproc-provider",
  "Interprocedural Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PROVIDER_END(InterprocProvider, "interproc-provider",
  "Interprocedural Provider")

char InterprocAnalysisPass::ID = 0;
INITIALIZE_PASS_BEGIN(InterprocAnalysisPass, "interproc-analysis",
  "Interprocedural Analysis Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(InterprocProvider)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(InterprocAnalysisPass, "interproc-analysis",
  "Interprocedural Analysis Pass", false, false)

namespace clang {
template<class StmtType>
void getStmtTypeFromStmtTree(Stmt *S, std::vector<StmtType *> &VCE) {
  if (!S)
    return;
  if (auto A = dyn_cast<StmtType>(S))
    VCE.push_back(A);
  for (Stmt *SChild : S->children())
    getStmtTypeFromStmtTree(SChild, VCE);
}
}

namespace {
  void setFuncLoc(std::vector<CallExpr *> VecCallExpr,
      std::set<Function *> SetCalleeFunc,
      tsar::InterprocElemInfo &IEI) {
    for (auto CalleeFunc : SetCalleeFunc) {
      std::vector<SourceLocation> VecSL;
      for (auto CE : VecCallExpr)
        if (CE->getDirectCallee()->getName().equals(CalleeFunc->getName()))
          VecSL.push_back(CE->getLocStart());
      IEI.addCalleeFunc(CalleeFunc, VecSL);
    }
  }

  void setLoopIEI(llvm::Module &M, Stmt *S,
      tsar::InterprocAnalysisFuncInfo &IAFI,
      tsar::InterprocAnalysisLoopInfo &IALI) {
    tsar::InterprocElemInfo IEI;
    std::vector<CallExpr *> VecCallExpr;
    std::set<Function *> SetCalleeFunc;
    clang::getStmtTypeFromStmtTree(S, VecCallExpr);
    for (auto CE : VecCallExpr)
      SetCalleeFunc.insert(M.getFunction(CE->getDirectCallee()->getName()));
    for (auto CF : SetCalleeFunc) {
      auto E = IAFI.find(CF)->second;
      if (E.hasAttr(tsar::InterprocElemInfo::Attr::InOutFunc))
        IEI.setAttr(tsar::InterprocElemInfo::Attr::InOutFunc);
      if (E.hasAttr(tsar::InterprocElemInfo::Attr::NoReturn))
        IEI.setAttr(tsar::InterprocElemInfo::Attr::NoReturn);
    }
    setFuncLoc(VecCallExpr, SetCalleeFunc, IEI);
    IALI.insert(std::make_pair(S, IEI));
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
  InterprocProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass &TEP) {
    TEP.setContext(M, TfmCtx);
  });
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  LibFunc LibID;
  std::set<Function *> SetSCCFunc;
  std::set<Function *> SetCalleeFunc;
  bool InOutFunc = false;
  bool NoReturn = false;
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (F && TfmCtx->getDeclForMangledName(F->getName()))
      SetSCCFunc.insert(F);
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    for (auto CFGTo : *CGN) {
      auto FTo = CFGTo.second->getFunction();
      if (FTo && SetSCCFunc.find(FTo) == SetSCCFunc.end())
        SetCalleeFunc.insert(FTo);
    }
  }
  for (auto SCCF : SetSCCFunc)
    if ((TLI.getLibFunc(*SCCF, LibID) &&
        SetInOutFunc.find(SCCF->getName()) != SetInOutFunc.end()) ||
        (!TLI.getLibFunc(*SCCF, LibID) && SCCF->empty())) {
      InOutFunc = true;
      break;
    }
  if (!InOutFunc)
    for (auto CF : SetCalleeFunc) {
      auto IACF = mInterprocAnalysisFuncInfo.find(CF);
      if (IACF != mInterprocAnalysisFuncInfo.end() &&
          IACF->second.hasAttr(tsar::InterprocElemInfo::Attr::InOutFunc)) {
        InOutFunc = true;
        break;
      }
    }
  for (auto SCCF : SetSCCFunc)
    if (SCCF->hasFnAttribute(Attribute::NoReturn) ||
        (!TLI.getLibFunc(*SCCF, LibID) && SCCF->empty())) {
      NoReturn = true;
      break;
    }
  if (!NoReturn)
    for (auto CF : SetCalleeFunc) {
      auto IACF = mInterprocAnalysisFuncInfo.find(CF);
      if (IACF != mInterprocAnalysisFuncInfo.end() &&
          IACF->second.hasAttr(tsar::InterprocElemInfo::Attr::NoReturn)) {
        NoReturn = true;
        break;
      }
    }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    auto D = TfmCtx->getDeclForMangledName(F->getName());
    if (!D)
      continue;
    std::vector<CallExpr *> VecCallExpr;
    std::set<Function *> SetCalleeFunc;
    tsar::InterprocElemInfo IEI;
    if (InOutFunc)
      IEI.setAttr(tsar::InterprocElemInfo::Attr::InOutFunc);
    if (NoReturn)
      IEI.setAttr(tsar::InterprocElemInfo::Attr::NoReturn);
    if (F->empty()) {
      mInterprocAnalysisFuncInfo.insert(std::make_pair(F, IEI));
      continue;
    }
    getStmtTypeFromStmtTree<CallExpr>(D->getBody(), VecCallExpr);
    for (auto CFGTo : *CGN)
      if (CFGTo.second->getFunction())
        SetCalleeFunc.insert(CFGTo.second->getFunction());
    setFuncLoc(VecCallExpr, SetCalleeFunc, IEI);
    mInterprocAnalysisFuncInfo.insert(std::make_pair(F, IEI));
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F || F->empty())
      continue;
    auto &LMP = getAnalysis<LoopMatcherPass>(*F);
    auto &Matcher = LMP.getMatcher();
    auto &Unmatcher = LMP.getUnmatchedAST();
    for (auto Match : Matcher)
      setLoopIEI(M, Match.first,
          mInterprocAnalysisFuncInfo, mInterprocAnalysisLoopInfo);
    for (auto Unmatch : Unmatcher)
      setLoopIEI(M, Unmatch,
          mInterprocAnalysisFuncInfo, mInterprocAnalysisLoopInfo);
  }
}

void InterprocAnalysisPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<InterprocProvider>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}

ModulePass *llvm::createInterprocAnalysisPass() {
  return new InterprocAnalysisPass();
}
