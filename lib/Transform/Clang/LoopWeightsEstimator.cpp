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
#include "tsar/Support/IRUtils.h"
#include <clang/Basic/SourceLocation.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/InitializePasses.h>
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
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)  // causes build error "‘initializeCallGraphWrapperPassPass’ was not declared in this scope"
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)  // the same
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)  // the same
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

uint64_t getInstructionExecutionCount(const Instruction *I,
    const std::vector<llvm::coverage::CountedRegion> &CRs) {
  const auto &DL = I->getDebugLoc();
  if (!DL) {
    return 0;
  }
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

uint64_t getLoopExecutionCount(const Loop *L,
    const std::vector<llvm::coverage::CountedRegion> &CRs) {
  Instruction *HeadInstruction = &*L->getHeader()->begin();
  uint64_t LoopExecutionCount = getInstructionExecutionCount(HeadInstruction, CRs);
  return LoopExecutionCount;
}

uint64_t getInstructionEffectiveWeight(const Instruction *I,
    const std::vector<llvm::coverage::CountedRegion> &CRs,
    TargetTransformInfo &TTI,
    std::map<llvm::Function *, uint64_t> &FunctionBaseWeights) {
  uint64_t InstructionExecutionCount = getInstructionExecutionCount(I, CRs);
  uint64_t InstructionEffectiveWeight =
      TTI.getInstructionCost(I, TargetTransformInfo::TCK_SizeAndLatency)
      * InstructionExecutionCount;
  Function *Called = nullptr;
  if ((isa<CallInst>(I) || isa<InvokeInst>(I))
      && (Called = cast<CallBase>(I)->getCalledFunction())
      && !(Called->isIntrinsic())
      && !(Called->empty())) {
    uint64_t CalledBaseWeight = FunctionBaseWeights[Called];
    InstructionEffectiveWeight += CalledBaseWeight * InstructionExecutionCount;
  }
  return InstructionEffectiveWeight;
}
}

ClangLoopWeightsEstimator::ClangLoopWeightsEstimator() : ModulePass(ID) {
  initializeClangLoopWeightsEstimatorPass(*PassRegistry::getPassRegistry());
}

bool ClangLoopWeightsEstimator::runOnModule(Module &M) {
  std::cout << "Loop & Function Weights:" << std::endl << std::endl;
  auto &CRs = getAnalysis<ClangCountedRegionsExtractor>().getCountedRegions();
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  for (scc_iterator<CallGraph *> SCCIt = scc_begin(&CG); !SCCIt.isAtEnd(); ++SCCIt) {
    for (auto *CGN : *SCCIt) {
      Function *F = nullptr;
      if ((F = CGN->getFunction()) && !(F->isIntrinsic()) && !(F->empty())) {
        auto &TTI = getAnalysis<TargetTransformInfoWrapperPass>().getTTI(*F);
        std::map<Instruction *, uint64_t> InstructionEffectiveWeights;
        uint64_t FunctionBaseWeight = 0;
        for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
          uint64_t InstructionEffectiveWeight = getInstructionEffectiveWeight(&*I, CRs, TTI,
              mFunctionBaseWeights);
          InstructionEffectiveWeights[&*I] = InstructionEffectiveWeight;
          FunctionBaseWeight += InstructionEffectiveWeight;
        }
        mFunctionBaseWeights[F] = FunctionBaseWeight;
        std::cout << F->getName().str()
            << ": base weight " << FunctionBaseWeight
            << ", loops:"
            << std::endl;
        std::map<MDNode *, uint64_t> LoopIterationsOriginal;
        std::map<MDNode *, uint64_t> LoopIterationsAccumulated;
        bool *Changed = new bool();
        *Changed = false;
        auto &LI = getAnalysis<LoopInfoWrapperPass>(*F, Changed).getLoopInfo();
        auto Loops = LI.getLoopsInPreorder();
        for_each_loop(
            LI, [&CRs,
            &InstructionEffectiveWeights,
            &LoopIterationsOriginal,
            &LoopIterationsAccumulated,
            this](const Loop *L) {
              if (L->getLoopID()) {
                uint64_t LParentIterationsAccumulated = 1;
                if (Loop *LParent = L->getParentLoop()) {
                  LParentIterationsAccumulated = LoopIterationsAccumulated[LParent->getLoopID()];
                }
                uint64_t LExecutionCount = getLoopExecutionCount(L, CRs);
                LoopIterationsAccumulated[L->getLoopID()] = LExecutionCount;
                LoopIterationsOriginal[L->getLoopID()] =
                    LExecutionCount / LParentIterationsAccumulated;
                uint64_t LoopEffectiveWeight = 0;
                for (auto BBPtr = L->block_begin(), BBEndPtr = L->block_end(); BBPtr != BBEndPtr; ++BBPtr) {
                  BasicBlock *BB = *BBPtr;
                  for (Instruction &I : *BB) {
                    LoopEffectiveWeight += InstructionEffectiveWeights[&I];
                  }
                }
                mLoopEffectiveWeights[L->getLoopID()] = LoopEffectiveWeight;
                mLoopBaseWeights[L->getLoopID()] = LoopEffectiveWeight / LParentIterationsAccumulated;
                std::cout << "\t" << L->getName().str()
                    << ", start " << L->getStartLoc().getLine() << ":" << L->getStartLoc().getCol()
                    << ": base weight " << LoopEffectiveWeight / LParentIterationsAccumulated
                    << ": eff weight " << LoopEffectiveWeight
                    << std::endl;
              }
            });
        std::cout << std::endl;
      }
    }
  }
  return false;
}

const std::map<MDNode *, uint64_t> &
ClangLoopWeightsEstimator::getLoopWeights() const noexcept {
	return mLoopEffectiveWeights;
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
