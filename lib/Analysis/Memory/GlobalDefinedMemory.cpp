//===--- GlobalDefinedMemory.cpp --- Global Defined Memory Analysis ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===----------------------------------------------------------------------===//
//
// This file implements pass to determine global defined memory locations.
//
//===----------------------------------------------------------------------===//
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
#define DEBUG_TYPE "GDM"


using namespace llvm;
using namespace tsar;

char GlobalDefinedMemory::ID = 0;

typedef FunctionPassProvider <
  DFRegionInfoPass,
  EstimateMemoryPass,
  DominatorTreeWrapperPass
  >
  passes;

INITIALIZE_PROVIDER_BEGIN(passes, "GDM-FP", "global-def-mem-func-provider")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(passes, "GDM-FP", "global-def-mem-func-provider")

INITIALIZE_PASS_BEGIN(GlobalDefinedMemory, "GDM", "global-def-mem",  true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(passes)
INITIALIZE_PASS_END(GlobalDefinedMemory, "GDM", "global-def-mem", true, true)



void GlobalDefinedMemory::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<passes>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createGlobalDefinedMemoryPass() {
  return new GlobalDefinedMemory();
}

bool GlobalDefinedMemory::runOnModule(Module &SCC) {
  LLVM_DEBUG(
    dbgs() << "[GLOBAL_DEFINED_MEMORY]: Begin of GlobalDefinedMemoryPass\n";
  );
  auto& CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto& TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  for (auto& CurrNode = po_begin(&CG), LastNode = po_end(&CG);
    CurrNode != LastNode;
    CurrNode++) {

    CallGraphNode* CGN = *CurrNode;
    if (Function* F = CGN->getFunction()) {

      if (F->hasName() && !F->empty()) {
        LLVM_DEBUG(
          dbgs() <<"[GLOBAL_DEFINED_MEMORY]: " << F->getName() << "\n";
        );

        auto& PassesInfo = getAnalysis<passes>(*F);
        auto &RegionInfoForF = PassesInfo
          .get<DFRegionInfoPass>()
          .getRegionInfo();
        auto &AliasTree = PassesInfo.
          get<EstimateMemoryPass>()
          .getAliasTree();
        const DominatorTree *DT = nullptr;
        LLVM_DEBUG(
          DT = &PassesInfo.get<DominatorTreeWrapperPass>().getDomTree()
        );
        auto *DFF = cast<DFFunction>(RegionInfoForF.getTopLevelRegion());

        DefinedMemoryInfo DefInfo;
        ReachDFFwk ReachDefFwk(AliasTree, TLI, DT, DefInfo, mInterprocDefInfo);
        solveDataFlowUpward(&ReachDefFwk, DFF);
        auto DefUseSetItr = ReachDefFwk.getDefInfo().find(DFF);
        assert(DefUseSetItr != ReachDefFwk.getDefInfo().end() &&
          "Def-use set must exist for a function!");
        mInterprocDefInfo
          .try_emplace(F, std::move(DefUseSetItr->get<DefUseSet>()));
      }
    }
  }
  LLVM_DEBUG(
    dbgs() << "[GLOBAL_DEFINED_MEMORY]: End of GlobalDefinedMemoryPass\n";
  );
  return false;
}