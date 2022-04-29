//===- ParallelLoop.cpp ---- Parallel Loop Analysis -------------*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// This file implements passes to determine parallelization opportunities
// of loops.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <llvm/InitializePasses.h>
#include <llvm/Analysis/LoopInfo.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "parallel-loop"

using namespace llvm;
using namespace tsar;

char ParallelLoopPass::ID = 0;
INITIALIZE_PASS_BEGIN(ParallelLoopPass, "parallel-loop",
                      "Parallel Loop Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopAttributesDeductionPass)
INITIALIZE_PASS_END(ParallelLoopPass, "parallel-loop", "Parallel Loop Analysis",
                    true, true)

void ParallelLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<LoopAttributesDeductionPass>();
  AU.setPreservesAll();
}

bool ParallelLoopPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &LoopAttr = getAnalysis<LoopAttributesDeductionPass>();
  DIAliasTree *DIAT = nullptr;
  DIDependencInfo *DIDepInfo = nullptr;
  std::function<ObjectID(ObjectID)> getLoopID = [](ObjectID ID) { return ID; };
  std::function<Value * (Value *)> getValue = [](Value *V) { return V; };
  if (auto *SInfo = getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>()) {
    if (auto *Socket = (*SInfo)->getActiveSocket()) {
      if (auto R = Socket->getAnalysis<AnalysisClientServerMatcherWrapper>()) {
        auto *Matcher = R->value<AnalysisClientServerMatcherWrapper *>();
        getLoopID = [Matcher](ObjectID ID) {
          auto ServerID = (*Matcher)->getMappedMD(ID);
          return ServerID ? cast<MDNode>(*ServerID) : nullptr;
        };
        getValue = [Matcher](Value *V) {
          return (**Matcher)[V];
        };
        if (auto R = Socket->getAnalysis<
          DIEstimateMemoryPass, DIDependencyAnalysisPass>(F)) {
          DIAT = &R->value<DIEstimateMemoryPass *>()->getAliasTree();
          DIDepInfo = &R->value<DIDependencyAnalysisPass *>()->getDependencies();
        }
      }
    }
  }
  if (!DIAT || !DIDepInfo) {
    LLVM_DEBUG(dbgs() << "[PARALLE LOOP]: analysis server is not available\n");
    if (auto *P = getAnalysisIfAvailable<DIEstimateMemoryPass>())
      DIAT = &P->getAliasTree();
    else
      return false;
    if (auto *P = getAnalysisIfAvailable<DIDependencyAnalysisPass>())
      DIDepInfo = &P->getDependencies();
    else
      return false;
    LLVM_DEBUG(
        dbgs() << "[PARALLEL LOOP]: use dependence analysis from client\n");
  }
  for_each_loop(LI, [this, &F, &GO, &LoopAttr, &getLoopID, &getValue, DIAT,
                     DIDepInfo](Loop *L) {
    auto SLoc = L->getStartLoc();
    if (!getValidExitingBlock(*L) ||
        !LoopAttr.hasAttr(*L, AttrKind::AlwaysReturn) ||
        !LoopAttr.hasAttr(*L, AttrKind::NoIO) ||
        !LoopAttr.hasAttr(*L, Attribute::NoUnwind) ||
        LoopAttr.hasAttr(*L, Attribute::ReturnsTwice)) {
      LLVM_DEBUG(
          dbgs() << "[PARALLEL LOOP]: unsafe CFG prevents parallelization: ";
          SLoc.print(dbgs()); dbgs() << "\n");
      return;
    }
    bool AllowGPU = true;
    for (auto *BB : L->getBlocks())
      for (auto &I : *BB) {
        auto *Call = dyn_cast<CallBase>(&I);
        if (!Call)
          continue;
        auto Callee =
            dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
        if (!Callee || !hasFnAttr(*Callee, AttrKind::DirectUserCallee)) {
          LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: indirect call of user-defined "
                               "function prevents parallelization: ";
                     SLoc.print(dbgs()); dbgs() << "\n");
          return;
        } else {
          if (!Callee->doesNotAccessMemory() && !Callee->isSpeculatable() &&
              (!isa<IntrinsicInst>(I) ||
               !isDbgInfoIntrinsic(Callee->getIntrinsicID()) &&
                   !isMemoryMarkerIntrinsic(Callee->getIntrinsicID()))) {
            if (auto OnServer{cast_or_null<Function>(getValue(Callee))}) {
              AllowGPU &= OnServer->onlyAccessesArgMemory();
            } else {
              AllowGPU = false;
              // TDOD (kaniandr@gmail.com): sometimes mapping is lost after
              // some transform passes if the function is internal
              LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: callee traits for '"
                                << Callee->getName()
                                << "' are not available on server: ";
                         SLoc.print(dbgs()); dbgs() << "\n");
            }
          }
        }
      }
    auto *LoopID = L->getLoopID();
    if (!LoopID || !(LoopID = getLoopID(LoopID))) {
      LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: ignore loop without ID: ";
                 SLoc.print(dbgs()); dbgs() << "\n");
      return;
    }
    auto DepItr = DIDepInfo->find(LoopID);
    // TODO (kaniandr@gmail.com): investigate cases which lead to absence of
    // analysis results. This situation occurs if CG from NAS NPB 3.3.1 is
    // analyzed for example.
    if (DepItr == DIDepInfo->end()) {
      LLVM_DEBUG(
          dbgs() << "[PARALLEL LOOP]: ignore loop without analysis results: ";
          SLoc.print(dbgs()); dbgs() << "\n");
      return;
    }
    auto &DIDepSet = DepItr->get<DIDependenceSet>();
    DenseSet<const DIAliasNode *> Coverage;
    accessCoverage<bcl::SimpleInserter>(DIDepSet, *DIAT, Coverage,
                                        GO.IgnoreRedundantMemory);
    for (auto &TS : DIDepSet) {
      if (!Coverage.count(TS.getNode()))
        continue;
      if (TS.is_any<trait::AddressAccess, trait::Output>() ||
          TS.is<trait::DynamicPrivate>() && !TS.is<trait::Shared>()) {
        LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: the presence of data "
                             "dependencies prevents parallelization: ";
                   SLoc.print(dbgs()); dbgs() << "\n");
        return;
      }
      // TODO (kaniandr@gmail.com): ignore redundant memory in TS.size() if
      // appropriate option is specified.
      if (TS.size() > 1 && !TS.is_any<trait::Shared, trait::Readonly>()) {
        LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: memory aliasing "
                             "prevents parallelization: ";
                   SLoc.print(dbgs()); dbgs() << "\n");
        return;
      }
      auto &DIMTraitItr = *TS.begin();
      if (DIMTraitItr->is<trait::Reduction>()) {
        auto *Info = DIMTraitItr->get<trait::Reduction>();
        if (!Info) {
          LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: unknown reduction operation "
                               "prevents parallelization: ";
                     SLoc.print(dbgs()); dbgs() << "\n");
          return;
        }
      }
      if (DIMTraitItr->is<trait::Flow>()) {
        auto *Info = DIMTraitItr->get<trait::Flow>();
        if (!Info || !Info->isKnownDistance()) {
          LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: unknown distance of a flow "
                               "dependence prevents parallelization: ";
                     SLoc.print(dbgs()); dbgs() << "\n");
          return;
        }
      }
      if (DIMTraitItr->is<trait::Anti>()) {
        auto *Info = DIMTraitItr->get<trait::Anti>();
        if (!Info || !Info->isKnownDistance()) {
          LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: unknown distance of an anti "
                               "dependence prevents parallelization: ";
                     SLoc.print(dbgs()); dbgs() << "\n");
          return;
        }
      }
    }
    LLVM_DEBUG(dbgs() << "[PARALLEL LOOP]: parallel loop found: ";
               SLoc.print(dbgs()); dbgs() << "\n");
    mParallelLoops.try_emplace(L, !AllowGPU);
  });
  return false;
}
