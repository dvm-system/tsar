//===--- GlobalLiveMemory.cpp - Global Live Memory Analysis -----*- C++ -*-===//
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
//
//===---------------------------------------------------------------------===//
//
// This file implements passes to determine global live memory locations.
//
//===---------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/GlobalLiveMemory.h"
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
#include <vector>

#undef DEBUG_TYPE
#define DEBUG_TYPE "live-mem"

using namespace llvm;
using namespace tsar;

namespace {
using CallList = std::vector<
    bcl::tagged_pair<bcl::tagged<Instruction *, Instruction>,
                     bcl::tagged<std::unique_ptr<LiveSet>, LiveSet>>>;

/// This container contains results of the live memory analysis for calls to
/// a function (which is a key).
using LiveMemoryForCalls = DenseMap<const Function *, CallList>;

using GlobalLiveMemoryProvider = FunctionPassProvider<
  DFRegionInfoPass,
  DefinedMemoryPass,
  DominatorTreeWrapperPass>;
}

char GlobalLiveMemory::ID = 0;

INITIALIZE_PROVIDER_BEGIN(GlobalLiveMemoryProvider, "global-live-mem-provider",
                          "Global Live Memory Analysis (Provider)")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(GlobalLiveMemoryProvider, "global-live-mem-provider",
                        "Global Live Memory Analysis (Provider)")

INITIALIZE_PASS_BEGIN(GlobalLiveMemory, "global-live-mem",
                      "Global Live Memory Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryProvider)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_END(GlobalLiveMemory, "global-live-mem",
                    "Global Live Memory Analysis", true, true)

void GlobalLiveMemory::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalLiveMemoryProvider>();
  AU.addRequired<GlobalDefinedMemoryWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createGlobalLiveMemoryPass() {
  return new GlobalLiveMemory();
}

#ifdef LLVM_DEBUG
void visitedFunctionsLog(const LiveMemoryForCalls &Info) {
  dbgs() << "[GLOBAL LIVE MEMORY]: list of visited functions\n";
  for (auto &FInfo : Info) {
    dbgs() << FInfo.first->getName() << "has calls from:\n";
    for (auto &CallTo : FInfo.second)
      dbgs() << "  " << CallTo.get<Instruction>()->getFunction()->getName()
             << "\n";
  }
}
#endif

bool GlobalLiveMemory::runOnModule(Module &M) {
  auto &GDM = getAnalysis<GlobalDefinedMemoryWrapper>();
  if (GDM) {
    GlobalLiveMemoryProvider::initialize<GlobalDefinedMemoryWrapper>(
        [&GDM](GlobalDefinedMemoryWrapper &Wrapper) { Wrapper.set(*GDM); });
  }
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  ReversePostOrderTraversal<CallGraph *> RPOT(&CG);
  LiveMemoryForCalls LiveSetForCalls;
  for (auto &CurrNode = RPOT.begin(), LastNode = RPOT.end();
       CurrNode != LastNode; CurrNode++) {
    CallGraphNode *CGN = *CurrNode;
    auto F = CGN->getFunction();
    if (!F || F->empty())
      continue;
    LLVM_DEBUG(dbgs() << "[GLOBAL LIVE MEMORY]: analyze " << F->getName()
                      << "\n";);
    auto &Provider = getAnalysis<GlobalLiveMemoryProvider>(*F);
    auto &RegInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto *TopRegion = cast<DFFunction>(RegInfo.getTopLevelRegion());
    auto &DefInfo = Provider.get<DefinedMemoryPass>().getDefInfo();
    DominatorTree *DT = nullptr;
    LLVM_DEBUG(DT = &Provider.get<DominatorTreeWrapperPass>().getDomTree());
    LiveMemoryInfo IntraLiveInfo;
    auto LiveItr =
        IntraLiveInfo.try_emplace(TopRegion, llvm::make_unique<LiveSet>()).first;
    auto &LS = LiveItr->get<LiveSet>();
    auto FOut = LS->getOut();
    auto FInfoItr = LiveSetForCalls.find(F);
    if (FInfoItr != LiveSetForCalls.end()) {
      for (auto &CallInfo : FInfoItr->second) {
        assert(CallInfo.get<LiveSet>() &&
               "Live set must be already constructed for a call!");
          FOut.merge(CallInfo.get<LiveSet>()->getOut());
      }
      LS->setOut(FOut);
    }
    LiveDFFwk LiveFwk(IntraLiveInfo, DefInfo, DT);
    solveDataFlowDownward(&LiveFwk, TopRegion);
    for (auto &CallRecord : *CGN) {
      Function *Callee = CallRecord.second->getFunction();
      if (!Callee)
        continue;
      auto FuncInfo = LiveSetForCalls.try_emplace(Callee);
      auto *BB = cast<Instruction>(CallRecord.first)->getParent();
      auto *DFB = RegInfo.getRegionFor(BB);
      assert(DFB && "Data-flow node must not be null!");
      FuncInfo.first->second.push_back(
          std::make_pair(cast<Instruction>(CallRecord.first),
                         std::move(LiveFwk.getLiveInfo()[DFB])));
    }
    mInterprocLiveMemory.try_emplace(F, std::move(IntraLiveInfo[TopRegion]));
  }
  LLVM_DEBUG(visitedFunctionsLog(LiveSetForCalls));
  return false;
}
