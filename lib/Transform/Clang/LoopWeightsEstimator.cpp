//=== LoopWeightsEstimator.cpp - Loop Weights Estimator (Clang) --*- C++ -*===//
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
//===---------------------------------------------------------------------===//
//
// This file defines a pass to estimate loop weights in a source code.
//
//===---------------------------------------------------------------------===//

#include "tsar/Transform/Clang/LoopWeightsEstimator.h"
#include "tsar/Analysis/Clang/CountedRegionsExtractor.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/Basic/SourceLocation.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/Debug.h>
#include <map>
#include <vector>

#include <iostream>


using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-loop-weights"

class ClangCopyPropagationInfo final : public PassGroupInfo {
  void addBeforePass(llvm::legacy::PassManager &PM) const override {
    PM.add(createClangCountedRegionsExtractor());
  }
};

char ClangLoopWeightsEstimator::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopWeightsEstimator, "clang-loop-weights",
  "Loop Weights Estimator (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangCopyPropagationInfo);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
// INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)  // causes build error "‘initializeCallGraphWrapperPassPass’ was not declared in this scope"
// INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)  // the same
// INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)  // the same
INITIALIZE_PASS_DEPENDENCY(ClangCountedRegionsExtractor)
INITIALIZE_PASS_IN_GROUP_END(ClangLoopWeightsEstimator, "clang-loop-weights",
  "Loop Weights Estimator (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {
bool operator==(const coverage::LineColPair &lhs, const coverage::LineColPair &rhs) {
  return lhs.first == rhs.first
      && lhs.second == rhs.second;
}

bool operator!=(const coverage::LineColPair &lhs, const coverage::LineColPair &rhs) {
  return !(lhs == rhs);
}

bool operator<(const coverage::LineColPair &lhs, const coverage::LineColPair &rhs) {
  return (lhs.first < rhs.first)
      || (lhs.first == rhs.first && lhs.second < rhs.second);
}

bool operator<=(const coverage::LineColPair &lhs, const coverage::LineColPair &rhs) {
  return (lhs.first < rhs.first)
      || (lhs.first == rhs.first && lhs.second <= rhs.second);
}

bool operator>(const coverage::LineColPair &lhs, const coverage::LineColPair &rhs) {
  return rhs < lhs;
}

bool operator>=(const coverage::LineColPair &lhs, const coverage::LineColPair &rhs) {
  return rhs <= lhs;
}

uint64_t getExecutionCount(Instruction *I,
    const std::vector<llvm::coverage::CountedRegion> &CRs) {
  const auto &DL = I->getDebugLoc();
  coverage::LineColPair ILineCol{ DL.getLine(), DL.getCol() };
  coverage::LineColPair MinEnclosingStartLoc{ 0, 0 };
  coverage::LineColPair MinEnclosingEndLoc{ -1, -1 };
  int64_t InstructionExecutionCount = 0;
  for (const auto &CR : CRs) {
    coverage::LineColPair CRStartLoc = CR.startLoc();
    coverage::LineColPair CREndLoc = CR.endLoc();
    if (CRStartLoc <= ILineCol && ILineCol <= CREndLoc
        && MinEnclosingStartLoc < CRStartLoc && MinEnclosingEndLoc > CREndLoc) {
      MinEnclosingStartLoc = CRStartLoc;
      MinEnclosingEndLoc = CREndLoc;
      InstructionExecutionCount = CR.ExecutionCount;
    }
  }
  return InstructionExecutionCount;
}

uint64_t getExecutionCount(Function *F,
    const std::vector<llvm::coverage::CountedRegion> &CRs) {
  return getExecutionCount(&*inst_begin(F), CRs);
}

uint64_t getInstructionEffectiveWeight(Instruction *I,
    const std::vector<llvm::coverage::CountedRegion> &CRs,
    TargetTransformInfo &TTI,
    std::map<llvm::Function *, std::pair<uint64_t, uint64_t>>
        &FunctionEffectiveWeightsAndExecutionCounts) {
  uint64_t InstructionExecutionCount = getExecutionCount(I, CRs);
  uint64_t InstructionEffectiveWeight =
      TTI.getInstructionCost(I, TargetTransformInfo::TCK_SizeAndLatency)
      * InstructionExecutionCount;
  Function *Called = nullptr;
  if ((isa<CallInst>(I) || isa<InvokeInst>(I))
      && (Called = cast<CallBase>(I)->getCalledFunction())) {
    uint64_t CalledEffectiveWeight = FunctionEffectiveWeightsAndExecutionCounts[Called].first;
    uint64_t CalledExecutionCount = FunctionEffectiveWeightsAndExecutionCounts[Called].second;
    InstructionEffectiveWeight +=
        CalledEffectiveWeight * InstructionExecutionCount / CalledExecutionCount;
  }
  return InstructionEffectiveWeight;
}
}

ClangLoopWeightsEstimator::ClangLoopWeightsEstimator() : ModulePass(ID) {
  initializeClangLoopWeightsEstimatorPass(*PassRegistry::getPassRegistry());
}

bool ClangLoopWeightsEstimator::runOnModule(Module &M) {
  auto &CRs = getAnalysis<ClangCountedRegionsExtractor>().getCountedRegions();
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::map<Function *, std::pair<uint64_t, uint64_t>> FunctionEffectiveWeightsAndExecutionCounts;
  for (scc_iterator<CallGraph *> SCCIt = scc_begin(&CG); !SCCIt.isAtEnd(); ++SCCIt) {
    for (auto *CGN : *SCCIt) {
      if (Function *F = CGN->getFunction()) {
        auto &TTI = getAnalysis<TargetTransformInfoWrapperPass>().getTTI(*F);
        std::map<Instruction *, uint64_t> InstructionEffectiveWeights;
        uint64_t FunctionEffectiveWeight = 0;
        for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
          uint64_t InstructionEffectiveWeight = getInstructionEffectiveWeight(&*I, CRs, TTI,
              FunctionEffectiveWeightsAndExecutionCounts);
          InstructionEffectiveWeights[&*I] = InstructionEffectiveWeight;
          FunctionEffectiveWeight += InstructionEffectiveWeight;
        }
        FunctionEffectiveWeightsAndExecutionCounts[F].first = FunctionEffectiveWeight;
        FunctionEffectiveWeightsAndExecutionCounts[F].second = getExecutionCount(F, CRs);
        auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
        auto Loops = LI.getLoopsInPreorder();
        for (Loop *L = *Loops.begin(), *LEnd = *Loops.end(); L != LEnd; ++L) {
          uint64_t LoopEffectiveWeight = 0;
          for (BasicBlock *BB = *L->block_begin(), *BBEnd = *L->block_end(); BB != BBEnd; ++BB) {
            for (Instruction &I : *BB) {
              LoopEffectiveWeight += InstructionEffectiveWeights[&I];
            }
          }
          LoopEffectiveWeights[L->getLoopID()] = LoopEffectiveWeight;
        }
      }
    }
  }
  std::cout << "Loop Weights:" << std::endl;
  for (const auto &[LoopId, LoopWeight] : LoopEffectiveWeights) {
    std::cout << "\t" << LoopId << ": " << LoopWeight << std::endl;
  }
  return false;
}

const std::map<MDNode *, uint64_t> &
ClangLoopWeightsEstimator::getLoopWeights() const noexcept {
	return LoopEffectiveWeights;
}

void ClangLoopWeightsEstimator::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetTransformInfoWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ClangCountedRegionsExtractor>();
  AU.setPreservesAll();
}

ModulePass *llvm::createClangLoopWeightsEstimator() {
  return new ClangLoopWeightsEstimator();
}
