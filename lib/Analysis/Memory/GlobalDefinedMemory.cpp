#include <tsar/Analysis/Memory/GlobalDefinedMemory.h>
#include <tsar/Support/PassProvider.h>
#include <tsar/Analysis/Memory/LiveMemory.h>
#include <tsar/Analysis/Memory/EstimateMemory.h>

#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/Function.h>

#include <Vector>
#include <map>
#ifdef LLVM_DEBUG
#include <llvm/IR/Dominators.h>
#endif

#undef DEBUG_TYPE
#define DEBUG_TYPE "def-mem"


using namespace llvm;
using namespace tsar;

char GlobalDefinedMemory::ID = 0;

typedef FunctionPassProvider <
  DFRegionInfoPass,
  EstimateMemoryPass,
  DominatorTreeWrapperPass
  >
  passes;

INITIALIZE_PROVIDER_BEGIN(passes, "test", "test")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(passes, "test", "test")

INITIALIZE_PASS_BEGIN(GlobalDefinedMemory, "global-def-mem", "GDM", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(passes)
INITIALIZE_PASS_END(GlobalDefinedMemory, "global-def-mem", "GDM", true, true)



void GlobalDefinedMemory::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<passes>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createGlobalDefinedMemoryPass() {
  return new GlobalDefinedMemory();
}

/*void printInterprocDefInfo(InterprocDefInfo& IPDefInfo) {
  for(auto& currPair = IPDefInfo.begin(), lastPair = IPDefInfo.end();
    currPair!=lastPair;
    currPair++)
  {
    errs() << "Function: " << currPair->first->getName();
    for each (auto var in currPair->getSecond->)
    {

    }
  }
}*/

bool GlobalDefinedMemory::runOnModule(Module &SCC) {
  //LLVM_DEBUG(
    errs() << "Begin of GlobalDefinedMemoryPass\n";
  //);
  auto& CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto& TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  for (auto& CurrNode = po_begin(&CG), LastNode = po_end(&CG);
    CurrNode != LastNode;
    CurrNode++) {
    CallGraphNode* CGN = *CurrNode;
    if (Function* F = CGN->getFunction()) {
      if (F->hasName() && !F->empty()) {
        errs() << "===============\n";
        errs() << F->getName() << "\n";
        errs() << "===============\n";

        auto& PassesInfo = getAnalysis<passes>(*F);
        auto &RegionInfoForF = PassesInfo
          .getAnalysis<DFRegionInfoPass>()
          .getRegionInfo();
        auto &AliasTree = PassesInfo.
          getAnalysis<EstimateMemoryPass>()
          .getAliasTree();
        const DominatorTree *DT = nullptr;
        LLVM_DEBUG(DT = &PassesInfo.getAnalysis<DominatorTreeWrapperPass>().getDomTree());
        auto *DFF = cast<DFFunction>(RegionInfoForF.getTopLevelRegion());
        DefinedMemoryInfo DefInfo;
        ReachDFFwk ReachDefFwk(AliasTree, TLI, DT, DefInfo, mInterprocDefInfo);
        solveDataFlowUpward(&ReachDefFwk, DFF);
		auto DefUseSetItr = ReachDefFwk.getDefInfo().find(DFF);
		assert(DefUseSetItr != ReachDefFwk.getDefInfo().end() &&
			"Def-use set must exist for a function!");
        mInterprocDefInfo.try_emplace(F, std::move(DefUseSetItr->get<DefUseSet>()));
      }
    }
  }
  //printInterprocDefInfo(mInterprocDefInfo);
  
  return false;

}