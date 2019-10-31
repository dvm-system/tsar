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
  TargetLibraryInfoWrapperPass,
  EstimateMemoryPass>
  passes;

INITIALIZE_PROVIDER_BEGIN(passes, "", "")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(passes, "", "")

INITIALIZE_PASS_BEGIN(GlobalDefinedMemory, "global-def-mem", "", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(GlobalDefinedMemory, "global-def-mem", "", true, true)



void GlobalDefinedMemory::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<CallGraphWrapperPass>();
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
        auto &TLI = PassesInfo
          .getAnalysis<TargetLibraryInfoWrapperPass>()
          .getTLI();
        auto &AliasTree = PassesInfo.
          getAnalysis<EstimateMemoryPass>()
          .getAliasTree();
        const DominatorTree *DT = nullptr;
        LLVM_DEBUG(DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree());
        auto *DFF = cast<DFFunction>(RegionInfoForF.getTopLevelRegion());
        DefinedMemoryInfo DefInfo;
        ReachDFFwk ReachDefFwk(AliasTree, TLI, DT, DefInfo, mInterprocDefInfo);
        solveDataFlowUpward(&ReachDefFwk, DFF);
        mInterprocDefInfo.insert(std::make_pair(F, std::get<0>(ReachDefFwk.getDefInfo()[DFF])));
      }
    }
  }
  //printInterprocDefInfo(mInterprocDefInfo);
  
  return false;

}