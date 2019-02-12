#include "CalleeProcLocation.h"
#include "InterprocAttr.h"
#include "Attributes.h"
#include "tsar_loop_matcher.h"
#include "tsar_pass_provider.h"
#include "tsar_transformation.h"
#include <clang/AST/Expr.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/raw_ostream.h>
#include <set>
#include <vector>

using namespace tsar;
using namespace llvm;
using namespace clang;

#undef DEBUG_TYPE
#define DEBUG_TYPE "functionattrs"

STATISTIC(NumLibFunc, "Number of functions marked as sapfor.libfunc");

typedef FunctionPassProvider<
  TransformationEnginePass,
  LoopMatcherPass> InterprocAttrProvider;

INITIALIZE_PROVIDER_BEGIN(InterprocAttrProvider, "interproc-attr-provider",
  "Interprocedural Attribute Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PROVIDER_END(InterprocAttrProvider, "interproc-attr-provider",
    "Interprocedural Attribute Provider")

char InterprocAttrPass::ID = 0;
INITIALIZE_PASS_BEGIN(InterprocAttrPass, "interproc-attr",
  "Interprocedural Attribute Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(InterprocAttrProvider)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(InterprocAttrPass, "interproc-attr",
  "Interprocedural Attribute Pass", false, false)

namespace {
/// This pass walks SCCs of the call  graph in RPO to deduce and propagate
/// function attributes.
///
/// Currently it only handles synthesizing sapfor.libfunc attributes.
/// We consider a function as a library function if it is called from some
/// known by llvm::TargetLibraryInfo library function.
struct RPOFunctionAttrsAnalysis : public ModulePass, private bcl::Uncopyable {
  static char ID;
  RPOFunctionAttrsAnalysis() : ModulePass(ID) {
    initializeRPOFunctionAttrsAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<CallGraphWrapperPass>();
    AU.addPreserved<CallGraphWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
  }
};

bool addLibFuncAttrsTopDown(Function &F) {
  for (auto *U : F.users()) {
    CallSite CS(U);
    if (CS || hasFnAttr(*CS.getParent()->getParent(), AttrKind::LibFunc)) {
      addFnAttr(F, AttrKind::LibFunc);
      ++NumLibFunc;
      return true;
    }
  }
  return true;
}

void setLoopIA(llvm::Module &M, Stmt *S,
    tsar::InterprocAttrFuncInfo &IAFI,
    tsar::InterprocAttrStmtInfo &IALI) {
  tsar::InterprocAttrs IA;
  std::vector<CallExpr *> VecCallExpr;
  std::vector<Function *> SetCalleeFunc;
  clang::getStmtTypeFromStmtTree(S, VecCallExpr);
  for (auto CE : VecCallExpr)
    SetCalleeFunc.push_back(M.getFunction(CE->getDirectCallee()->getName()));
  for (auto CF : SetCalleeFunc) {
    auto E = IAFI.find(CF)->second;
    if (E.hasAttr(tsar::InterprocAttrs::Attr::InOutFunc))
      IA.setAttr(tsar::InterprocAttrs::Attr::InOutFunc);
    if (E.hasAttr(tsar::InterprocAttrs::Attr::NoReturn))
      IA.setAttr(tsar::InterprocAttrs::Attr::NoReturn);
  }
  IALI.insert(std::make_pair(S, IA));
}
}

char RPOFunctionAttrsAnalysis::ID = 0;

INITIALIZE_PASS_BEGIN(RPOFunctionAttrsAnalysis, "rpo-sapfor-functionattrs",
  "Deduce function attributes in RPO", false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(RPOFunctionAttrsAnalysis, "rpo-sapfor-functionattrs",
  "Deduce function attributes in RPO", false, false)

ModulePass * llvm::createRPOFunctionAttrsAnalysis() {
  return new RPOFunctionAttrsAnalysis();
}

bool RPOFunctionAttrsAnalysis::runOnModule(llvm::Module &M) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  std::vector<Function *> Worklist;
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    auto CGNIdx = Worklist.size();
    bool HasLibFunc = false;
    for (auto *CGN : *I)
      if (auto F = CGN->getFunction()) {
        LibFunc LibId;
        Worklist.push_back(F);
        HasLibFunc = HasLibFunc || TLI.getLibFunc(*F, LibId);
      }
    if (HasLibFunc)
      for (std::size_t EIdx = Worklist.size(); CGNIdx < EIdx; ++CGNIdx)
        addFnAttr(*Worklist[CGNIdx], AttrKind::LibFunc);
  }
  bool Changed = false;
  for (auto *F : llvm::reverse(Worklist))
    Changed |= addLibFuncAttrsTopDown(*F);
  return Changed;
}

bool InterprocAttrPass::runOnModule(llvm::Module &M) {
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

void InterprocAttrPass::runOnSCC(CallGraphSCC &SCC, llvm::Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return;
  }
  InterprocAttrProvider::initialize<TransformationEnginePass>(
      [&M, &TfmCtx](TransformationEnginePass &TEP) {
    TEP.setContext(M, TfmCtx);
  });
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
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
      auto IACF = mInterprocAttrFuncInfo.find(CF);
      if (IACF != mInterprocAttrFuncInfo.end() &&
          IACF->second.hasAttr(tsar::InterprocAttrs::Attr::InOutFunc)) {
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
      auto IACF = mInterprocAttrFuncInfo.find(CF);
      if (IACF != mInterprocAttrFuncInfo.end() &&
          IACF->second.hasAttr(tsar::InterprocAttrs::Attr::NoReturn)) {
        for (auto SCCF : SetSCCFunc)
          if (SetInOutFunc.find(SCCF->getName()) == SetInOutFunc.end()) {
            NoReturn = true;
            break;
          }
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
    tsar::InterprocAttrs IA;
    if (InOutFunc)
      IA.setAttr(tsar::InterprocAttrs::Attr::InOutFunc);
    if (NoReturn)
      IA.setAttr(tsar::InterprocAttrs::Attr::NoReturn);
    mInterprocAttrFuncInfo.insert(std::make_pair(F, IA));
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F || F->empty())
      continue;
    auto &Provider = getAnalysis<InterprocAttrProvider>(*F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    for (auto Match : Matcher)
      setLoopIA(M, Match.first,
          mInterprocAttrFuncInfo, mInterprocAttrLoopInfo);
    for (auto Unmatch : Unmatcher)
      setLoopIA(M, Unmatch,
          mInterprocAttrFuncInfo, mInterprocAttrLoopInfo);
  }
}

void InterprocAttrPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<InterprocAttrProvider>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}

ModulePass *llvm::createInterprocAttrPass() {
  return new InterprocAttrPass();
}
