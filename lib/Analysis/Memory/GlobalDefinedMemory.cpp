//===-- GlobalDefinedMemory.cpp - Global Defined Memory Analysis-*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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

#include "tsar/Analysis/Memory/GlobalDefinedMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Support/PassProvider.h"
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>
#ifdef LLVM_DEBUG
#include <llvm/IR/Dominators.h>
#endif

#undef DEBUG_TYPE
#define DEBUG_TYPE "def-mem"

using namespace llvm;
using namespace tsar;

char GlobalDefinedMemory::ID = 0;

namespace {
using GlobalDefinedMemoryProvider = FunctionPassProvider<
  DFRegionInfoPass,
  EstimateMemoryPass,
  DominatorTreeWrapperPass>;
}

INITIALIZE_PROVIDER_BEGIN(GlobalDefinedMemoryProvider,
                          "global-def-mem-provider",
                          "Global Defined Memory Analysis (Provider)")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(GlobalDefinedMemoryProvider, "global-def-mem-provider",
                        "Global Defined Memory Analysis (Provider)")

INITIALIZE_PASS_BEGIN(GlobalDefinedMemory, "global-def-mem",
                      "Global Defined Memory Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryProvider)
INITIALIZE_PASS_END(GlobalDefinedMemory, "global-def-mem",
                    "Global Defined Memory Analysis", true, true)

void GlobalDefinedMemory::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalDefinedMemoryProvider>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

ModulePass *llvm::createGlobalDefinedMemoryPass() {
  return new GlobalDefinedMemory();
}

bool GlobalDefinedMemory::runOnModule(Module &SCC) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  for (auto &CurrNode = po_begin(&CG), LastNode = po_end(&CG);
       CurrNode != LastNode; ++CurrNode) {
    CallGraphNode *CGN = *CurrNode;
    auto F = CGN->getFunction();
    if (!F || F->empty())
      continue;
    LLVM_DEBUG(dbgs() << "[GLOBAL DEFINED MEMORY]: analyze " << F->getName()
                      << "\n";);
    auto &Provider = getAnalysis<GlobalDefinedMemoryProvider>(*F);
    auto &RegInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto &AT = Provider.get<EstimateMemoryPass>().getAliasTree();
    const DominatorTree *DT = nullptr;
    LLVM_DEBUG(DT = &Provider.get<DominatorTreeWrapperPass>().getDomTree());
    auto *DFF = cast<DFFunction>(RegInfo.getTopLevelRegion());
    DefinedMemoryInfo DefInfo;
    ReachDFFwk ReachDefFwk(AT, TLI, DT, DefInfo, mInterprocDUInfo);
    solveDataFlowUpward(&ReachDefFwk, DFF);
    auto DefUseSetItr = ReachDefFwk.getDefInfo().find(DFF);
    assert(DefUseSetItr != ReachDefFwk.getDefInfo().end() &&
           "Def-use set must exist for a function!");
    mInterprocDUInfo.try_emplace(F, std::move(DefUseSetItr->get<DefUseSet>()));
  }
  return false;
}