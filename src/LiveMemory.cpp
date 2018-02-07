//===--- LiveMemory.h ------ Lived Memory Analysis --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to determine live memory locations.
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Debug.h>
#ifdef DEBUG
# include <llvm/IR/Dominators.h>
#endif
#include "tsar_dbg_output.h"
#include "DefinedMemory.h"
#include "LiveMemory.h"

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "live-mem"

char LiveMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(LiveMemoryPass, "live-mem",
  "Live Memory Analysis", false, true)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_END(LiveMemoryPass, "live-mem",
  "Live Memory Analysis", false, true)

bool llvm::LiveMemoryPass::runOnFunction(Function & F) {
  auto &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &DefInfo = getAnalysis<DefinedMemoryPass>().getDefInfo();
  DominatorTree *DT = nullptr;
  DEBUG(
    auto DTPass = getAnalysisIfAvailable<DominatorTreeWrapperPass>();
  if (DTPass)
    DT = &DTPass->getDomTree();
  );
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  auto LiveItr = mLiveInfo.insert(
    std::make_pair(DFF, llvm::make_unique<LiveSet>())).first;
  auto &LS = LiveItr->get<LiveSet>();
  auto DefItr = DefInfo.find(DFF);
  assert(DefItr != DefInfo.end() && DefItr->get<DefUseSet>() &&
    "Def-use set must not be null!");
  auto &DefUse = DefItr->get<DefUseSet>();
  // If inter-procedural analysis is not performed conservative assumption for
  // live variable analysis should be made. All locations except 'alloca' are
  // considered as alive before exit from this function.
  MemorySet<MemoryLocation> MayLives;
  for (const MemoryLocation &Loc : DefUse->getDefs()) {
    if (!Loc.Ptr || !isa<AllocaInst>(Loc.Ptr))
      MayLives.insert(Loc);
  }
  for (const MemoryLocation &Loc : DefUse->getMayDefs()) {
    if (!Loc.Ptr || !isa<AllocaInst>(Loc.Ptr))
      MayLives.insert(Loc);
  }
  LS->setOut(std::move(MayLives));
  LiveDFFwk LiveFwk(mLiveInfo, DefInfo, DT);
  solveDataFlowDownward(&LiveFwk, DFF);
  return false;
}

void LiveMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<DefinedMemoryPass>();
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
    std::make_pair(N, llvm::make_unique<LiveSet>()));
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
  MemorySet<MemoryLocation> newIn(DU->getUses());
  for (auto &Loc : LS->getOut()) {
    if (!DU->hasDef(Loc))
      newIn.insert(Loc);
  }
  DEBUG(
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
  for (auto &Loc : V)
    (printLocationSource(dbgs(), Loc.Ptr, DFF->getDomTree()), dbgs() << "\n");
  dbgs() << "[END LIVE]\n";
  );
  if (LS->getIn() != newIn) {
    LS->setIn(std::move(newIn));
    return true;
  }
  return false;
}
