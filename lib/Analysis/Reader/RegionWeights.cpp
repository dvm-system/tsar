//=== RegionWeights.cpp ---- Region Weights Estimator -----------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file defines a pass to estimate weights of regions in a source code.
//
//===---------------------------------------------------------------------===//

#include "tsar/Analysis/Reader/RegionWeights.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Function.h>
#include "llvm/IR/Instruction.h"
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/Debug.h>
#include <vector>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "region-weights"

char RegionWeightsEstimator::ID = 0;

INITIALIZE_PASS_BEGIN(RegionWeightsEstimator, "region-weights",
  "Region Weights Estimator", true, true)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(RegionWeightsEstimator, "region-weights",
  "Region Weights Estimator", true, true)

static uint64_t getExecutionCount(const Instruction &I,
                                  const coverage::CoverageData &Coverage) {
  auto Loc{I.getDebugLoc()};
  if (!Loc || Coverage.empty())
    return 0;
  auto BI{Coverage.begin()}, EI{Coverage.end()};
  if (BI->Line > Loc.getLine() ||
      BI->Line == Loc.getLine() && BI->Col > Loc.getCol())
    return 0;
  for (;;) {
    auto Size{std::distance(BI, EI)};
    if (Size == 1)
      return BI->Count;
    auto Half{Size / 2};
    auto HI{BI + Half};
    if (HI->Line < Loc.getLine() ||
        HI->Line == Loc.getLine() && HI->Col <= Loc.getCol())
      BI = HI;
    else
      EI = HI;
  }
  return 0;
}

static int64_t getLoopExecutionCount(const Loop *L,
                                     const coverage::CoverageData &Coverage) {
  // TODO(kaniandr@gmail.com): is it better to count backedge taken count?
  for (auto &I : *L->getHeader())
    if (auto LoopExecutionCount{getExecutionCount(I, Coverage)};
        LoopExecutionCount > 0)
      return LoopExecutionCount;
  return 0;
}

std::pair<InstructionCost, InstructionCost>
RegionWeightsEstimator::getWeight(const Instruction &I,
                                     const coverage::CoverageData &Coverage,
                                     TargetTransformInfo &TTI,
                                     unsigned UnknownFunctionWeight,
                                     unsigned UnknownBuiltinWeight) {
  auto Count{getExecutionCount(I, Coverage)};
  auto SelfWeight{
      TTI.getInstructionCost(&I, TargetTransformInfo::TCK_SizeAndLatency)};
  if (auto *CB{dyn_cast<CallBase>(&I)}) {
    if (auto *F{CB->getCalledFunction()}) {
      if (!isDbgInfoIntrinsic(F->getIntrinsicID()) &&
          !isMemoryMarkerIntrinsic(F->getIntrinsicID()))
        if (auto WeightItr{mSelfWeights.find(F)};
            WeightItr != mSelfWeights.end())
          SelfWeight += WeightItr->second;
        else if (F->isIntrinsic() || hasFnAttr(*F, AttrKind::LibFunc))
          // TODO (kaniandr@gmail.com): estimate calls to intrinsics and
          // library functions separately.
          SelfWeight += UnknownBuiltinWeight;
        else
          SelfWeight += UnknownFunctionWeight;
    } else {
      SelfWeight += UnknownFunctionWeight;
    }
  }
  return std::pair{SelfWeight, SelfWeight * Count};
}

bool RegionWeightsEstimator::runOnModule(Module &M) {
  auto GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  if (GO.ProfileUse.empty())
    return false;
  std::vector<StringRef> ObjectFilenames{GO.ObjectFilenames.size()};
  transform(GO.ObjectFilenames, ObjectFilenames.begin(),
            [](auto &F) { return StringRef{F}; });
  auto CoverageOrErr{
      coverage::CoverageMapping::load(ObjectFilenames, GO.ProfileUse)};
  if (auto E{CoverageOrErr.takeError()}) {
    M.getContext().diagnose(DiagnosticInfoPGOProfile(
        GO.ProfileUse.data(),
        Twine("unable to load coverage data: ") + toString(std::move(E))));
    return false;
  }
  StringMap<const coverage::FunctionRecord *> Functions;
  for (auto &FRI : (**CoverageOrErr).getCoveredFunctions())
    Functions.try_emplace(FRI.Name, &FRI);
  auto &CG{getAnalysis<CallGraphWrapperPass>().getCallGraph()};
  for (auto SCCIt{scc_begin(&CG)}; !SCCIt.isAtEnd(); ++SCCIt) {
    // TODO (kaniandr@gmail.com): process recursion
    // For functions in SCC:
    // - compute a number of calls inside SCC,
    // - compute a number of calls outside SCC
    // - compute self weight for each instruction in each function in SCC (if it
    // is a call of a function from SCC ignore function weight)
    // - compute total weight of SCC sum(function weights = number of calls of a
    // function * sum(self instruction weights))
    // - compute self weight of SCC: divide total weight on a number of calls
    // outside SCC.
    if (SCCIt.hasCycle())
      continue;
    for (auto *CGN : *SCCIt) {
      auto *F{CGN->getFunction()};
      // Note, that there is no edges to intrinsics in a call graph, so a check
      // for an intrinsic function seems to be forbidden.
      if (!F || F->isIntrinsic() || F->isDeclaration())
        continue;
      auto &TTI{getAnalysis<TargetTransformInfoWrapperPass>().getTTI(*F)};
      auto FuncItr{Functions.find(F->getName())};
      if (FuncItr == Functions.end())
        continue;
      auto Coverage{
          (**CoverageOrErr).getCoverageForFunction(*FuncItr->getValue())};
      InstructionCost FunctionTotalWeight;
      for (auto &I : instructions(F)) {
        auto W{getWeight(I, Coverage, TTI, GO.UnknownFunctionWeight,
                         GO.UnknownBuiltinWeight)};
        mSelfWeights.try_emplace(&I, W.first);
        mTotalWeights.try_emplace(&I, W.second);
        FunctionTotalWeight += W.second;
      }
      unsigned AssumedEntryCount = FuncItr->getValue()->ExecutionCount > 0
                                       ? FuncItr->getValue()->ExecutionCount
                                       : 1;
      F->setEntryCount(FuncItr->getValue()->ExecutionCount);
      mTotalWeights.try_emplace(F, FunctionTotalWeight);
      mSelfWeights.try_emplace(F, FunctionTotalWeight / AssumedEntryCount);
      LLVM_DEBUG(dbgs() << "[WEIGHT ESTIMATOR]: weight for function '"
                        << F->getName().str()
                        << "': entry count: " << F->getEntryCount()->getCount()
                        << ", self weight: " << getSelfWeight(F)
                        << ", total weight " << getTotalWeight(F) << "\n");
      auto &LI{getAnalysis<LoopInfoWrapperPass>(*F).getLoopInfo()};
      for_each_loop(LI, [this, AssumedEntryCount, &Coverage](const Loop *L) {
        auto LoopID{L->getLoopID()};
        if (!LoopID)
          return;
        auto *Parent{L->getParentLoop()};
        uint64_t ParentTotalIterations{AssumedEntryCount};
        if (Parent)
          if (auto *ID{Parent->getLoopID()})
            ParentTotalIterations = getTotalWeight(ID).second;
        InstructionCost LoopTotalWeight;
        for (auto *BB : L->blocks())
          for (auto &I : *BB)
            LoopTotalWeight += getTotalWeight(&I);
        auto LoopSelfWeight = LoopTotalWeight / ParentTotalIterations;
        auto TotalIterations{getLoopExecutionCount(L, Coverage)};
        auto SelfIterations{TotalIterations / ParentTotalIterations};
        mLoopTotalWeights.try_emplace(LoopID, LoopTotalWeight, TotalIterations);
        mLoopSelfWeights.try_emplace(LoopID, LoopSelfWeight, SelfIterations);
        LLVM_DEBUG(dbgs() << "[WEIGHT ESTIMATOR]: weight for loop at ";
                   L->getStartLoc().print(dbgs());
                   dbgs() << ": self weight: " << LoopSelfWeight
                          << ", self iterations " << SelfIterations
                          << ", total weight " << LoopTotalWeight
                          << ", total iterations " << TotalIterations << "\n");
      });
    }
  }
  return false;
}

void RegionWeightsEstimator::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<TargetTransformInfoWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

ModulePass *llvm::createRegionWeightsEstimator() {
  return new RegionWeightsEstimator();
}
