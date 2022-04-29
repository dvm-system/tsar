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

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassProvider.h"
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DiagnosticInfo.h>
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
class GlobalLiveMemory : public ModulePass, private bcl::Uncopyable {
public:
  using IterprocLiveMemoryInfo =
    DenseMap<Function *, std::unique_ptr<tsar::LiveSet>,
      DenseMapInfo<Function *>,
      tsar::TaggedDenseMapPair<
        bcl::tagged<Function *, Function>,
        bcl::tagged<std::unique_ptr<tsar::LiveSet>, tsar::LiveSet>>>;

  static char ID;

  GlobalLiveMemory() : ModulePass(ID) {
    initializeGlobalLiveMemoryPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class GlobalLiveMemoryStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  GlobalLiveMemoryStorage() : ImmutablePass(ID) {
    initializeGlobalLiveMemoryStoragePass(*PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    getAnalysis<GlobalLiveMemoryWrapper>().set(mInterprocLiveMemory);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GlobalLiveMemoryWrapper>();
  }

  const InterprocLiveMemoryInfo &getLiveMemoryInfo() const noexcept {
    return mInterprocLiveMemory;
  }

  InterprocLiveMemoryInfo &getLiveMemoryInfo() noexcept {
    return mInterprocLiveMemory;
  }

private:
  InterprocLiveMemoryInfo mInterprocLiveMemory;
};

using CallList = std::vector<
    bcl::tagged_pair<bcl::tagged<Instruction *, Instruction>,
                     bcl::tagged<std::unique_ptr<LiveSet>, LiveSet>>>;

/// This container contains results of the live memory analysis for calls to
/// a function (which is a key).
using LiveMemoryForCalls = DenseMap<const Function *, CallList>;

using GlobalLiveMemoryProvider = FunctionPassProvider<
  GlobalOptionsImmutableWrapper,
  DFRegionInfoPass,
  DefinedMemoryPass,
  DominatorTreeWrapperPass>;

void initMayLivesWithIPO(Function &F, LiveMemoryForCalls &LiveSetForCalls,
    DefUseSet &DefUse, DataFlowTraits<LiveDFFwk *>::ValueType &MayLives) {
  auto FInfoItr = LiveSetForCalls.find(&F);
  // Check that a current function is entry point or that it is never called.
  // In this case list of live locations after exist from this function is empty.
  // This assumption is safe if -fno-external-calls option is set.
  if (FInfoItr == LiveSetForCalls.end())
    return;
  MemorySet<MemoryLocationRange> FOut;
  auto &DL = F.getParent()->getDataLayout();
  for (auto &CallInfo : FInfoItr->second) {
    assert(CallInfo.get<LiveSet>() &&
      "Live set must be already constructed for a call!");
    FOut.merge(CallInfo.get<LiveSet>()->getOut());
  }
  auto init = [&DL, &F, &FOut, &MayLives](const MemoryLocationRange &Loc) {
    assert(Loc.Ptr && "Pointer to location must not be null!");
    auto Ptr = getUnderlyingObject(Loc.Ptr, 0);
    if (isa<AllocaInst>(Ptr))
      return;
    if (find_if(F.args(), [Ptr](Argument &Arg) { return Ptr == &Arg; }) !=
            F.arg_end() ||
        isa<GlobalValue>(Ptr)) {
      if (Ptr == Loc.Ptr && !FOut.overlap(Loc) ||
          !FOut.overlap(MemoryLocation::getAfter(Ptr)))
        return;
    }
    MayLives.insert(Loc);
  };
  for (auto &Loc : DefUse.getDefs())
    init(Loc);
  for (auto &Loc : DefUse.getMayDefs())
    init(Loc);
}

/// Return true if each call is extracted to its own basic block.
bool checkCallsFrom(CallGraphNode &CGN) {
  assert(CGN.getFunction() && "Function must not be null!");
  for (auto &CallInfo : CGN) {
    if (!CallInfo.first.hasValue())
      continue;
    assert(*CallInfo.first && "Call instruction must not be null!");
    bool HasUsefulInstr = false;
    for (auto &I : *cast<Instruction>(**CallInfo.first).getParent()) {
      if (auto *II = dyn_cast<IntrinsicInst>(&I))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      if (isa<CallBase>(&I) && HasUsefulInstr) {
        llvm::DiagnosticInfoOptimizationFailure Diag(
            *CGN.getFunction(), I.getDebugLoc(),
            "inter-procedural live memory analysis was disabled: unable to "
            "extract function call into its own basic block");
        I.getContext().diagnose(Diag);
        return false;
      }
      HasUsefulInstr = true;
    }
  }
  return true;
}

#ifdef LLVM_DEBUG
void visitedFunctionsLog(const LiveMemoryForCalls &Info) {
  dbgs() << "[GLOBAL LIVE MEMORY]: list of visited functions\n";
  for (auto &FInfo : Info) {
    dbgs() << FInfo.first->getName() << " has calls from:\n";
    for (auto &CallTo : FInfo.second)
      dbgs() << "  " << CallTo.get<Instruction>()->getFunction()->getName()
             << "\n";
  }
}
#endif
}

INITIALIZE_PROVIDER_BEGIN(GlobalLiveMemoryProvider, "global-live-mem-provider",
                          "Global Live Memory Analysis (Provider)")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PROVIDER_END(GlobalLiveMemoryProvider, "global-live-mem-provider",
                        "Global Live Memory Analysis (Provider)")

char GlobalLiveMemoryStorage::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalLiveMemoryStorage, "global-live-mem-is",
  "Global Live Memory Analysis (Immutable Storage)", true, true)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_END(GlobalLiveMemoryStorage, "global-live-mem-is",
  "Global Live Memory Analysis (Immutable Storage)", true, true)

template<> char GlobalLiveMemoryWrapper::ID = 0;
INITIALIZE_PASS(GlobalLiveMemoryWrapper, "global-live-mem-iw",
  "Global Live Memory Analysis (Immutable Wrapper)", true, true)

char GlobalLiveMemory::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalLiveMemory, "global-live-mem",
                      "Global Live Memory Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryProvider)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_END(GlobalLiveMemory, "global-live-mem",
                    "Global Live Memory Analysis", true, true)

void GlobalLiveMemory::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalLiveMemoryProvider>();
  AU.addRequired<GlobalDefinedMemoryWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<GlobalLiveMemoryWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createGlobalLiveMemoryPass() {
  return new GlobalLiveMemory;
}

ImmutablePass *llvm::createGlobalLiveMemoryStorage() {
  return new GlobalLiveMemoryStorage;
}

bool GlobalLiveMemory::runOnModule(Module &M) {
  auto &Wrapper = getAnalysis<GlobalLiveMemoryWrapper>();
  if (!Wrapper)
    return false;
  Wrapper->clear();
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::vector<CallGraphNode *> Worklist;
  SmallPtrSet<CallGraphNode *, 32> HasExternalCalls;
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    // TODO (kaniandr@gmail.com): implement analysis in case of recursion.
    if (I->size() > 1)
      return false;
    CallGraphNode *CGN = I->front();
    auto F = CGN->getFunction();
    if (!F && !GO.NoExternalCalls)
      for (auto Callee : *CGN)
        HasExternalCalls.insert(Callee.second);
    // Avoid analysis of a library function because we must ensure that
    // all callers will be analyzed earlier. However, in general a library
    // function without body may call another library function.
    if (!F || hasFnAttr(*F, AttrKind::LibFunc) ||
        isDbgInfoIntrinsic(F->getIntrinsicID()) ||
        isMemoryMarkerIntrinsic(F->getIntrinsicID()))
      continue;
    if (F->empty() || !hasFnAttr(*F, AttrKind::DirectUserCallee))
      return false;
    if (!checkCallsFrom(*CGN))
      return false;
    Worklist.push_back(CGN);
  }
  GlobalLiveMemoryProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GO](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&GO);
      });
  auto &GDM = getAnalysis<GlobalDefinedMemoryWrapper>();
  if (GDM) {
    GlobalLiveMemoryProvider::initialize<GlobalDefinedMemoryWrapper>(
        [&GDM](GlobalDefinedMemoryWrapper &Wrapper) { Wrapper.set(*GDM); });
  }
  if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
    GlobalLiveMemoryProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  auto &DL = M.getDataLayout();
  LiveMemoryForCalls LiveSetForCalls;
  for (auto *CGN : llvm::reverse(Worklist)) {
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
    DataFlowTraits<LiveDFFwk *>::ValueType MayLives;
    auto DefItr = DefInfo.find(TopRegion);
    assert(DefItr != DefInfo.end() && DefItr->get<DefUseSet>() &&
      "Def-use set must not be null!");
    auto &DefUse = DefItr->get<DefUseSet>();
    if (!HasExternalCalls.count(CGN)) {
      initMayLivesWithIPO(*F, LiveSetForCalls, *DefUse, MayLives);
    } else {
      LLVM_DEBUG(dbgs() << "[GLOBAL LIVE MEMORY]: "
        "use conservative boundary conditions\n");
      for (auto &Loc : DefUse->getDefs())
        if (!isa<AllocaInst>(getUnderlyingObject(Loc.Ptr, 0)))
          MayLives.insert(Loc);
      for (auto &Loc : DefUse->getMayDefs())
        if (!isa<AllocaInst>(getUnderlyingObject(Loc.Ptr, 0)))
          MayLives.insert(Loc);
    }
    LiveMemoryInfo IntraLiveInfo;
    auto LiveItr =
      IntraLiveInfo.try_emplace(TopRegion, std::make_unique<LiveSet>()).first;
    auto &LS = LiveItr->get<LiveSet>();
    LS->setOut(MayLives);
    LiveDFFwk LiveFwk(IntraLiveInfo, DefInfo, DT);
    solveDataFlowDownward(&LiveFwk, TopRegion);
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);
    for (auto &CallRecord : *CGN) {
      Function *Callee = CallRecord.second->getFunction();
      if (!CallRecord.first || !Callee)
        continue;
      auto FuncInfo = LiveSetForCalls.try_emplace(Callee);
      auto *BB = cast<Instruction>(*CallRecord.first)->getParent();
      auto *DFB = RegInfo.getRegionFor(BB);
      assert(DFB && "Data-flow node must not be null!");
      FuncInfo.first->second.push_back(
          std::make_pair(cast<Instruction>(*CallRecord.first),
                         std::move(LiveFwk.getLiveInfo()[DFB])));
      auto &CallLS = FuncInfo.first->second.back().get<LiveSet>();
      auto &CallLiveOut =
          const_cast<MemorySet<MemoryLocationRange> &>(CallLS->getOut());
      if (!Callee->isVarArg())
        for_each_memory(*cast<Instruction>(*CallRecord.first), TLI,
          [Callee, &CallLiveOut](Instruction &I, MemoryLocation &&Loc,
              unsigned Idx, AccessInfo, AccessInfo) {
            auto OverlapItr = CallLiveOut.findOverlappedWith(Loc);
            if (OverlapItr == CallLiveOut.end())
              return;
            auto *Arg = Callee->arg_begin() + Idx;
            CallLiveOut.insert(MemoryLocationRange(Arg, 0, Loc.Size));
          },
          [](Instruction &, AccessInfo, AccessInfo) {});
    }
    Wrapper->try_emplace(F, std::move(IntraLiveInfo[TopRegion]));
  }
  LLVM_DEBUG(visitedFunctionsLog(LiveSetForCalls));
  return false;
}
