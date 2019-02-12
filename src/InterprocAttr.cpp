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

void setLoopIA(llvm::Module &M, Stmt *S, tsar::InterprocAttrStmtInfo &IALI) {
  tsar::InterprocAttrs IA;
  std::vector<CallExpr *> VecCallExpr;
  std::vector<Function *> SetCalleeFunc;
  clang::getStmtTypeFromStmtTree(S, VecCallExpr);
  for (auto CE : VecCallExpr)
    SetCalleeFunc.push_back(M.getFunction(CE->getDirectCallee()->getName()));
  for (auto CF : SetCalleeFunc) {
    if (!hasFnAttr(*CF, AttrKind::NoIO))
      IA.setAttr(tsar::InterprocAttrs::Attr::InOutFunc);
    if (!hasFnAttr(*CF, AttrKind::AlwaysReturn))
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
        if (F->isIntrinsic())
          continue;
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
  std::set<Function *> SetSCCFunc;
  std::set<Function *> SetCalleeFunc;
  bool InOutFunc = false;
  bool NoReturn = false;
  for (auto *CGN : SCC)
    if(auto F = CGN->getFunction())
      SetSCCFunc.insert(F);
  for (auto *CGN : SCC)
    if (auto F = CGN->getFunction())
      for (auto &CFGTo : *CGN)
        if (auto FTo = CFGTo.second->getFunction())
          SetCalleeFunc.insert(FTo);
  for (auto *SCCF : SetSCCFunc) {
    if (SCCF->isIntrinsic())
      continue;
    LibFunc LibId;
    // We can not use here 'sapfor.libfunc' attribute to check whether a
    // function is a library function because set of in/out functions contains
    // functions which treated as library by TargetLibraryInfo only. So, the
    // mentioned set may not contain some 'safpor.libfunc' functions which are
    // in/out functions.
    bool IsLibFunc = TLI.getLibFunc(*SCCF, LibId);
    if ((IsLibFunc && SetInOutFunc.count(SCCF->getName())) ||
        (!IsLibFunc && SCCF->isDeclaration())) {
      InOutFunc = true;
      break;
    }
  }
  if (!InOutFunc)
    for (auto *CF : SetCalleeFunc)
      if (!hasFnAttr(*CF, AttrKind::NoIO)) {
        InOutFunc = true;
        break;
      }
  for (auto *SCCF : SetSCCFunc) {
    if (SCCF->hasFnAttribute(Attribute::NoReturn) ||
        (SCCF->isDeclaration() &&
          !SCCF->isIntrinsic() && !hasFnAttr(*SCCF, AttrKind::LibFunc))) {
      NoReturn = true;
      break;
    }
  }
  if (!NoReturn)
    for (auto *CF : SetCalleeFunc) {
      if (!hasFnAttr(*CF, AttrKind::AlwaysReturn)) {
        NoReturn = true;
        break;
      }
    }
  for (auto *CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F)
      continue;
    if (!InOutFunc)
      addFnAttr(*F, AttrKind::NoIO);
    if (!NoReturn)
      addFnAttr(*F, AttrKind::AlwaysReturn);
  }
  for (auto CGN : SCC) {
    auto F = CGN->getFunction();
    if (!F || F->empty())
      continue;
    auto &Provider = getAnalysis<InterprocAttrProvider>(*F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    for (auto Match : Matcher)
      setLoopIA(M, Match.first, mInterprocAttrLoopInfo);
    for (auto Unmatch : Unmatcher)
      setLoopIA(M, Unmatch, mInterprocAttrLoopInfo);
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
