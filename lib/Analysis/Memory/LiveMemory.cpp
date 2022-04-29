//===--- LiveMemory.h ------ Lived Memory Analysis --------------*- C++ -*-===//
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
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to determine live memory locations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/ValueTracking.h>
#ifdef LLVM_DEBUG
# include <llvm/IR/Dominators.h>
#endif
#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "live-mem"

char LiveMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(LiveMemoryPass, "live-mem",
  "Live Memory Analysis", false, true)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_END(LiveMemoryPass, "live-mem",
  "Live Memory Analysis", false, true)

  bool llvm::LiveMemoryPass::runOnFunction(Function &F) {
  auto &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &DefInfo = getAnalysis<DefinedMemoryPass>().getDefInfo();
  DominatorTree *DT = nullptr;
  LLVM_DEBUG(
    auto DTPass = getAnalysisIfAvailable<DominatorTreeWrapperPass>();
  if (DTPass)
    DT = &DTPass->getDomTree();
  );
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  auto &GLM = getAnalysis<GlobalLiveMemoryWrapper>();
  auto LiveItr = mLiveInfo.insert(
    std::make_pair(DFF, std::make_unique<LiveSet>())).first;
  auto &LS = LiveItr->get<LiveSet>();
  bool IsIPOAvailable = false;
  if (GLM) {
    auto InfoItr = GLM->find(&F);
    // If it is not safe to perform interprocedural analysis for a function,
    // results won't be available.
    if (InfoItr != GLM->end()) {
      LS->setOut(InfoItr->get<LiveSet>()->getOut());
      IsIPOAvailable = true;
    }
  }
  if (!IsIPOAvailable) {
    auto DefItr = DefInfo.find(DFF);
    assert(DefItr != DefInfo.end() && DefItr->get<DefUseSet>() &&
      "Def-use set must not be null!");
    auto &DefUse = DefItr->get<DefUseSet>();
    auto &DL = F.getParent()->getDataLayout();
    // If inter-procedural analysis is not performed conservative assumption for
    // live variable analysis should be made. All locations except 'alloca' are
    // considered as alive before exit from this function.
    DataFlowTraits<LiveDFFwk *>::ValueType MayLives;
    for (auto &Loc : DefUse->getDefs()) {
      assert(Loc.Ptr && "Pointer to location must not be null!");
      if (!isa<AllocaInst>(getUnderlyingObject(Loc.Ptr, 0)))
        MayLives.insert(Loc);
    }
    for (auto &Loc : DefUse->getMayDefs()) {
      assert(Loc.Ptr && "Pointer to location must not be null!");
      if (!isa<AllocaInst>(getUnderlyingObject(Loc.Ptr, 0)))
        MayLives.insert(Loc);
    }
    LS->setOut(std::move(MayLives));
  }
  LiveDFFwk LiveFwk(mLiveInfo, DefInfo, DT);
  solveDataFlowDownward(&LiveFwk, DFF);
  return false;
}

void LiveMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<GlobalLiveMemoryWrapper>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createLiveMemoryPass() {
  return new LiveMemoryPass();
}

void DataFlowTraits<LiveDFFwk *>::initialize(
  DFNode *N, LiveDFFwk *DFF, GraphType) {
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null!");
  DFF->getLiveInfo().insert(
    std::make_pair(N, std::make_unique<LiveSet>()));
}

bool DataFlowTraits<LiveDFFwk*>::transferFunction(
  ValueType V, DFNode *N, LiveDFFwk *DFF, GraphType) {
  // Note, that transfer function is never evaluated for the exit node.
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null!");
  auto I = DFF->getLiveInfo().find(N);
  assert(I != DFF->getLiveInfo().end() && I->get<LiveSet>() &&
    "Data-flow value must be specified!");
  auto &LS = I->get<LiveSet>();
  LS->setOut(std::move(V)); // Do not use V below to avoid undefined behavior.
  if (isa<DFEntry>(N)) {
    if (LS->getIn() != LS->getOut()) {
      LS->setIn(LS->getOut());
      return true;
    }
    return false;
  }
  auto DefItr = DFF->getDefInfo().find(N);
  assert(DefItr != DFF->getDefInfo().end() && DefItr->get<DefUseSet>() &&
    "Def-use set must not be null!");
  auto &DU = DefItr->get<DefUseSet>();
  ValueType newIn(DU->getUses());
  for (auto &Loc : LS->getOut()) {
    if (!DU->hasDef(Loc))
      newIn.insert(Loc);
  }
  LLVM_DEBUG(
    dbgs() << "[LIVE] Live locations analysis, transfer function results for:";
  if (isa<DFBlock>(N)) {
    cast<DFBlock>(N)->getBlock()->print(dbgs());
  } else if (isa<DFLoop>(N)) {
    dbgs() << " loop with the following header:";
    cast<DFLoop>(N)->getLoop()->getHeader()->print(dbgs());
  } else {
    dbgs() << " unknown node.\n";
  }
  dbgs() << "IN:\n";
  for (auto &Loc : newIn)
    (printLocationSource(dbgs(), Loc.Ptr, DFF->getDomTree()), dbgs() << "\n");
  dbgs() << "OUT:\n";
  for (auto &Loc : LS->getOut())
    (printLocationSource(dbgs(), Loc.Ptr, DFF->getDomTree()), dbgs() << "\n");
  dbgs() << "[END LIVE]\n";
  );
  if (LS->getIn() != newIn) {
    LS->setIn(std::move(newIn));
    return true;
  }
  return false;
}
