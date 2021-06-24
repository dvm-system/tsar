//===- DependenceInliner.cpp - Inline Functions for Analysis -----*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a custom inliner that handles only functions which
// prevent data dependence analysis.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Transform/IR/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/IPO/Inliner.h>

#define DEBUG_TYPE "dependence-inline"

STATISTIC(CallsToInline, "Number of calls to inline");

using namespace llvm;
using namespace tsar;

namespace {
using DependenceInlinerProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass,
                           LoopInfoWrapperPass, LoopAttributesDeductionPass>;

class DependenceInlinerAttributer : public ModulePass, bcl::Uncopyable {
public:
  static char ID;

  DependenceInlinerAttributer() : ModulePass(ID) {
    initializeDependenceInlinerAttributerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class DependenceInlinerPass : public LegacyInlinerBase {
public:
  static char ID;

  explicit DependenceInlinerPass(bool InsertLifetime = true)
      : LegacyInlinerBase(ID, InsertLifetime) {
    initializeDependenceInlinerPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSCC(CallGraphSCC &SCC) override;
  InlineCost getInlineCost(CallBase &CB) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    LegacyInlinerBase::getAnalysisUsage(AU);
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<GlobalOptionsImmutableWrapper>();
  }

private:
  void evaluateCostOnSCC(CallGraphSCC &SCC);

  DenseMap<Function *, unsigned> mAnalysisCost;
  unsigned mCurrentSCCCost;
};
}

INITIALIZE_PROVIDER(DependenceInlinerProvider, "dependence-inline-provider",
                    "Inliner for Dependence Analysis (Provider)")

ModulePass *llvm::createDependenceInlinerAttributer() {
  return new DependenceInlinerAttributer();
}

char DependenceInlinerAttributer::ID = 0;
INITIALIZE_PASS_BEGIN(DependenceInlinerAttributer, "dependence-inline-attrs",
  "Inliner for Dependence Analysis (Attributer)", false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DependenceInlinerProvider)
INITIALIZE_PASS_END(DependenceInlinerAttributer, "dependence-inline-attrs",
  "Inliner for Dependence Analysis (Attributer)", false, false)

Pass *llvm::createDependenceInlinerPass(bool InsertLifetime) {
  return new DependenceInlinerPass(InsertLifetime);
}

char DependenceInlinerPass::ID = 0;
INITIALIZE_PASS_BEGIN(DependenceInlinerPass, "dependence-inline",
  "Inliner for Dependence Analysis", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(DependenceInlinerPass, "dependence-inline",
  "Inliner for Dependence Analysis", false, false)

void DependenceInlinerAttributer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DependenceInlinerProvider>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<GlobalDefinedMemoryWrapper>();
  AU.addRequired<GlobalLiveMemoryWrapper>();
  AU.setPreservesAll();
}

void DependenceInlinerPass::evaluateCostOnSCC(CallGraphSCC &SCC) {
  mCurrentSCCCost = 0;
  LLVM_DEBUG(
      dbgs() << "[DEPENDENDENCE INLINER]: evaluate analysis cost\n");
  for (auto &CGN : SCC) {
    if (auto *F{CGN->getFunction()}) {
      auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);
      for_each_memory(
          *F, TLI,
          [this](Instruction &, MemoryLocation &&, unsigned, AccessInfo,
                 AccessInfo) { ++mCurrentSCCCost; },
          [this](Instruction &, AccessInfo, AccessInfo) { ++mCurrentSCCCost; });
      LLVM_DEBUG(dbgs() << "[DEPENDENDENCE INLINER]: proccess " << F->getName()
                        << ", update number of memory accesse: "
                        << mCurrentSCCCost << "\n");
    }
  }
}

bool DependenceInlinerPass::runOnSCC(CallGraphSCC &SCC) {
  evaluateCostOnSCC(SCC);
  for (auto &CGN : SCC)
    if (auto *F{CGN->getFunction()})
      mAnalysisCost[F] = mCurrentSCCCost;
  auto Res = inlineCalls(SCC);
  for (auto &CGN : SCC)
    if (auto *F{CGN->getFunction()})
      mAnalysisCost[F] = mCurrentSCCCost;
  return Res;
}

InlineCost DependenceInlinerPass::getInlineCost(CallBase &CB) {
  if (!CB.getMetadata(getAsString(AttrKind::Inline)))
    return InlineCost::getNever("no dependence");
  Function *Callee = CB.getCalledFunction();
  if (!Callee)
    return InlineCost::getNever("indirect call");
  if (Callee->isDeclaration())
    return InlineCost::getNever("no definition");
  auto IsViable = isInlineViable(*Callee);
  if (!IsViable.isSuccess())
    return InlineCost::getNever(IsViable.getFailureReason());
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*Callee);
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto Cost = mCurrentSCCCost + mAnalysisCost[Callee];
  return InlineCost::get(Cost, GO.MemoryAccessInlineThreshold);
}

  bool DependenceInlinerAttributer::runOnModule(Module &M) {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    auto &GlobalsAA = getAnalysis<GlobalsAAWrapperPass>().getResult();
    auto &DIMEnv = getAnalysis<DIMemoryEnvironmentWrapper>().get();
    DependenceInlinerProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GO](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&GO);
      });
    DependenceInlinerProvider::initialize<GlobalsAAResultImmutableWrapper>(
        [&GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
          Wrapper.set(GlobalsAA);
        });
    DependenceInlinerProvider::initialize<DIMemoryEnvironmentWrapper>(
        [&DIMEnv](DIMemoryEnvironmentWrapper &Wrapper) {
          Wrapper.set(DIMEnv);
        });
    auto &DIMTraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
    DependenceInlinerProvider::initialize<DIMemoryTraitPoolWrapper>(
      [&DIMTraitPool](DIMemoryTraitPoolWrapper &Wrapper) {
        Wrapper.set(DIMTraitPool);
      });
    auto &GlobalDefUse = getAnalysis<GlobalDefinedMemoryWrapper>().get();
    DependenceInlinerProvider::initialize<GlobalDefinedMemoryWrapper>(
      [&GlobalDefUse](GlobalDefinedMemoryWrapper &Wrapper) {
        Wrapper.set(GlobalDefUse);
      });
    auto &GlobalLiveMemory = getAnalysis<GlobalLiveMemoryWrapper>().get();
    DependenceInlinerProvider::initialize<GlobalLiveMemoryWrapper>(
      [&GlobalLiveMemory](GlobalLiveMemoryWrapper &Wrapper) {
        Wrapper.set(GlobalLiveMemory);
      });
    if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
      DependenceInlinerProvider::initialize<GlobalsAccessWrapper>(
          [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
    auto InlineMDKind =
        M.getContext().getMDKindID(getAsString(AttrKind::Inline));
    auto InlineMD = MDNode::get(M.getContext(), {});
    auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
    bool IsChanged = false;
    DenseSet<Function *> ToInline;
    for (auto &F: M) {
      if (F.isDeclaration())
        continue;
      auto *CGN = CG[&F];
      if (!CGN)
        continue;
      DenseMap<MDNode *, CallBase *> Calls;
      for (auto &CR : *CGN) {
        if (!CR.first)
          continue;
        if (auto *CB = dyn_cast_or_null<CallBase>(*CR.first)) {
          if (auto DbgLoc = CB->getDebugLoc())
            Calls.try_emplace(DbgLoc.getAsMDNode(), CB);
        }
      }
      auto check = [InlineMDKind, InlineMD, &Calls, &ToInline,
                    &IsChanged](trait::DIDependence *Dep) {
        if (!Dep)
          return 0u;
        unsigned Count = 0;
        for (auto &C : Dep->getCauses()) {
          if (auto DbgLoc = C.get<DebugLoc>()) {
            auto CallItr = Calls.find(DbgLoc.getAsMDNode());
            if (CallItr == Calls.end() ||
                CallItr->second->hasMetadata(InlineMDKind))
              continue;
            ++Count;
            CallItr->second->setMetadata(InlineMDKind, InlineMD);
            IsChanged = true;
            if (auto *Callee = CallItr->second->getCalledFunction())
              ToInline.insert(Callee);
            LLVM_DEBUG(
              dbgs() << "[DEPENDENCE INLINER]: value to "
                        "inline found at ";
              C.get<DebugLoc>().print(dbgs());
              if (auto DISub = dyn_cast_or_null<DISubprogram>(
                      C.get<ObjectID>()))
                dbgs() << " (call to " << DISub->getName() << ")";
              CallItr->second->print(dbgs());
              dbgs() << "\n";
            );
          }
        }
        return Count;
      };
      auto &Provider = getAnalysis<DependenceInlinerProvider>(F);
      auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
      auto &DIAT = Provider.get<DIEstimateMemoryPass>().getAliasTree();
      auto &DIDep = Provider.get<DIDependencyAnalysisPass>().getDependencies();
      auto &LoopAttr = Provider.get<LoopAttributesDeductionPass>();
      unsigned ToInlineCount = 0;
      for_each_loop(LI, [this, GO, &DIAT, &DIDep, &LoopAttr, InlineMDKind,
                         InlineMD, &check, &IsChanged, &ToInline,
                         &ToInlineCount](Loop *L) {
        auto LoopID = L->getLoopID();
        if (!LoopID)
          return;
        if (!LoopAttr.hasAttr(*L, AttrKind::AlwaysReturn) ||
            !LoopAttr.hasAttr(*L, AttrKind::NoIO) ||
            !LoopAttr.hasAttr(*L, Attribute::NoUnwind) ||
            LoopAttr.hasAttr(*L, Attribute::ReturnsTwice))
          return;
        auto DIDepItr = DIDep.find(LoopID);
        if (DIDepItr == DIDep.end())
          return;
        auto &DIDepSet = DIDepItr->get<DIDependenceSet>();
        DenseSet<const DIAliasNode *> Coverage;
        accessCoverage<bcl::SimpleInserter>(DIDepSet, DIAT, Coverage,
                                            GO.IgnoreRedundantMemory);
        for (auto &TS : DIDepSet) {
          if (!Coverage.count(TS.getNode()))
            continue;
          for (auto &T : TS)
            if (T->is_any<trait::Anti, trait::Flow, trait::Output>()) {
              ToInlineCount += check(T->get<trait::Anti>());
              ToInlineCount += check(T->get<trait::Flow>());
              ToInlineCount += check(T->get<trait::Output>());
              if (auto *DIUM = dyn_cast<DIUnknownMemory>(T->getMemory()))
                if (isa<DISubprogram>(DIUM->getMetadata()))
                  for (auto &VH : *DIUM)
                    if (VH && !isa<UndefValue>(VH)) {
                      if (auto CB = dyn_cast<CallBase>(VH)) {
                        if (CB->hasMetadata(InlineMDKind))
                          continue;
                        ++CallsToInline;
                        CB->setMetadata(InlineMDKind, InlineMD);
                        IsChanged = true;
                        if (auto *Callee = CB->getCalledFunction())
                          ToInline.insert(Callee);
                        LLVM_DEBUG(
                          dbgs() << "[DEPENDENCE INLINER]: value to "
                                    "inline found ";
                          if (auto DbgLoc = CB->getDebugLoc()) {
                            dbgs() << "at "; DbgLoc.print(dbgs());
                            dbgs() << " ";
                          }
                          if (auto *Callee = CB->getCalledFunction())
                            dbgs() << "(call to " << Callee->getName() << ") ";
                          CB->print(dbgs());
                        );
                      }
                    }
            }
        }
      });
    }
    std::vector<Function *> Worklist(ToInline.begin(), ToInline.end());
    while (!Worklist.empty()) {
      auto *F = Worklist.back();
      Worklist.pop_back();
      auto *CGN = CG[F];
      if (!CGN)
        continue;
      for (auto &CR : *CGN) {
        if (!CR.first)
          continue;
        if (auto *CB = dyn_cast_or_null<CallBase>(*CR.first)) {
          if (auto Callee = CB->getCalledFunction())
            if (ToInline.insert(Callee).second)
              Worklist.push_back(Callee);
        }
      }
    }
    for (auto *F : ToInline) {
      auto *CGN = CG[F];
      if (!CGN)
        continue;
      LLVM_DEBUG(dbgs() << "[DEPENDENDENCE INLINER]: mark all calls from "
                        << F->getName() << "\n");
      for (auto &CR : *CGN) {
        if (!CR.first)
          continue;
        if (auto *CB = dyn_cast_or_null<CallBase>(*CR.first)) {
          if (CB->hasMetadata(InlineMDKind))
            continue;
          ++CallsToInline;
          IsChanged = true;
          CB->setMetadata(InlineMDKind, InlineMD);
        }
      }
    }
    return IsChanged;
  }
