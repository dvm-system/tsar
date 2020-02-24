#include "tsar/Analysis/Memory/AddressAccess.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Core/Query.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/CallGraph.h"

#include <iostream>

using namespace llvm;
using namespace tsar;
//using namespace tsar::detail;
using bcl::operator "" _b;

char AddressAccessAnalyser::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(AddressAccessAnalyser, "address-access",
  "address-access", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())  // todo (vld): alter name
//INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
//INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
//INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
//INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
//INITIALIZE_PASS_DEPENDENCY(LiveMemoryPass)
//INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
//INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
//INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
//INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)  // todo (vld): remove unused deps
INITIALIZE_PASS_IN_GROUP_END(AddressAccessAnalyser, "address-access",
  "address-access", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())  // todo (vld): alter name
// toask: define PassGroup

void AddressAccessAnalyser::getAnalysisUsage(AnalysisUsage &AU) const {
    // todo (vld): load deps results
    //AU.addRequired<LoopInfoWrapperPass>();
    AU.setPreservesAll();
}

bool AddressAccessAnalyser::runOnSCC(CallGraphSCC &SCC) {
    for (auto node : SCC) {
        Function *F = node->getFunction();
        if (F)
            std::cout << F->getName().data() << std::endl;
    }

    return false;
}

Pass *llvm::createAddressAccessAnalyserPass() {
    return new AddressAccessAnalyser();
}
