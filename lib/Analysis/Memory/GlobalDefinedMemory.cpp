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

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassProvider.h"
#include <bcl/utility.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Dominators.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "def-mem"

using namespace llvm;
using namespace tsar;

namespace {
class GlobalDefinedMemory : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  GlobalDefinedMemory() : ModulePass(ID) {
    initializeGlobalDefinedMemoryPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &SCC) override;
  void getAnalysisUsage(AnalysisUsage& AU) const override;
};

class GlobalDefinedMemoryStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  GlobalDefinedMemoryStorage() : ImmutablePass(ID) {
    initializeGlobalDefinedMemoryStoragePass(*PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    getAnalysis<GlobalDefinedMemoryWrapper>().set(mInterprocDUInfo);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GlobalDefinedMemoryWrapper>();
  }

  /// Return results of interprocedural reach definition analysis.
  tsar::InterprocDefUseInfo & getInterprocDefUseInfo() noexcept {
    return mInterprocDUInfo;
  }

  /// Return results of interprocedural reach definition analysis.
  const tsar::InterprocDefUseInfo & getInterprocDefUseInfo() const noexcept {
    return mInterprocDUInfo;
  }

private:
  tsar::InterprocDefUseInfo mInterprocDUInfo;
};

using GlobalDefinedMemoryProvider = FunctionPassProvider<
  DFRegionInfoPass,
  EstimateMemoryPass,
  DominatorTreeWrapperPass,
  DelinearizationPass,
  ScalarEvolutionWrapperPass>;
}

INITIALIZE_PROVIDER_BEGIN(GlobalDefinedMemoryProvider,
                          "global-def-mem-provider",
                          "Global Defined Memory Analysis (Provider)")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PROVIDER_END(GlobalDefinedMemoryProvider, "global-def-mem-provider",
                        "Global Defined Memory Analysis (Provider)")

char GlobalDefinedMemoryStorage::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalDefinedMemoryStorage, "global-def-mem-is",
  "Global Defined Memory Analysis (Immutable Storage)", true, true)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_END(GlobalDefinedMemoryStorage, "global-def-mem-is",
  "Global Defined Memory Analysis (Immutable Storage)", true, true)

template<> char GlobalDefinedMemoryWrapper::ID = 0;
INITIALIZE_PASS(GlobalDefinedMemoryWrapper, "global-def-mem-iw",
  "Global Defined Memory Analysis (Immutable Wrapper)", true, true)

char GlobalDefinedMemory::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalDefinedMemory, "global-def-mem",
                      "Global Defined Memory Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryProvider)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_END(GlobalDefinedMemory, "global-def-mem",
                    "Global Defined Memory Analysis", true, true)

void GlobalDefinedMemory::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalDefinedMemoryProvider>();
  AU.addRequired<GlobalDefinedMemoryWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createGlobalDefinedMemoryPass() {
  return new GlobalDefinedMemory;
}

ImmutablePass *llvm::createGlobalDefinedMemoryStorage() {
  return new GlobalDefinedMemoryStorage;
}

bool GlobalDefinedMemory::runOnModule(Module &SCC) {
  auto &Wrapper = getAnalysis<GlobalDefinedMemoryWrapper>();
  if (!Wrapper)
    return false;
  Wrapper->clear();
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  GlobalDefinedMemoryProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GO](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&GO);
      });
  auto &GAP{getAnalysis<GlobalsAccessWrapper>()};
  if (GAP)
    GlobalDefinedMemoryProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto &DL = SCC.getDataLayout();
  for (scc_iterator<CallGraph *> SCC = scc_begin(&CG); !SCC.isAtEnd(); ++SCC) {
    /// TODO (kaniandr@gmail.com): implement analysis in case of recursion.
    if (SCC->size() > 1)
      continue;
    CallGraphNode *CGN = *SCC->begin();
    auto F = CGN->getFunction();
    // Indirect calls or calls to functions without body may lead to implicit
    // recursion. So, disable analysis in this case.
    // TODO (kaniandr@gmail.com): sapfor.direct-user-callee is not set for
    // library functions, may be analysis of these functions is a special case
    // and these functions should be pre-analyzed.
    if (!F || F->empty() || !hasFnAttr(*F, AttrKind::DirectUserCallee))
      continue;
    LLVM_DEBUG(dbgs() << "[GLOBAL DEFINED MEMORY]: analyze " << F->getName()
                      << "\n";);
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);
    auto &Provider = getAnalysis<GlobalDefinedMemoryProvider>(*F);
    auto &RegInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto &AT = Provider.get<EstimateMemoryPass>().getAliasTree();
    const auto &DT = Provider.get<DominatorTreeWrapperPass>().getDomTree();
    auto *DFF = cast<DFFunction>(RegInfo.getTopLevelRegion());
    auto &DI = Provider.get<DelinearizationPass>().getDelinearizeInfo();
    auto &SE = Provider.get<ScalarEvolutionWrapperPass>().getSE();
    DefinedMemoryInfo DefInfo;
    ReachDFFwk ReachDefFwk(AT, TLI, RegInfo, DT, DI, SE, DL, GO, DefInfo,
                           *Wrapper);
    solveDataFlowUpward(&ReachDefFwk, DFF);
    auto DefUseSetItr = ReachDefFwk.getDefInfo().find(DFF);
    assert(DefUseSetItr != ReachDefFwk.getDefInfo().end() &&
           "Def-use set must exist for a function!");
    Wrapper->try_emplace(F, std::move(DefUseSetItr->get<DefUseSet>()));
    LLVM_DEBUG(dbgs() << "[GLOBAL DEFINED MEMORY]: leave " << F->getName()
                      << "\n";);
  }
  return false;
}
