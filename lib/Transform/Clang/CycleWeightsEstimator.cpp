//=== CycleWeightsEstimator.cpp - Cycle Weights Estimator (Clang) --*- C++ -*===//
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
// This file defines a pass to estimate cycle weights in a source code.
//
//===---------------------------------------------------------------------===//

#include "tsar/Transform/Clang/CycleWeightsEstimator.h"
#include "tsar/Analysis/Clang/CountedRegionsExtractor.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include "llvm/IR/Instruction.h"
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/Debug.h>
#include <map>
#include <vector>

#include <iostream>
#include <sstream>


using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-cycle-weights"

class ClangCopyPropagationInfo final : public PassGroupInfo {
  void addBeforePass(llvm::legacy::PassManager &PM) const override {
    PM.add(createClangCountedRegionsExtractor());
  }
};

char ClangCycleWeightsEstimator::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangCycleWeightsEstimator, "clang-cycle-weights",
  "Cycle Weights Estimator (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangCopyPropagationInfo);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangCountedRegionsExtractor)
INITIALIZE_PASS_IN_GROUP_END(ClangCycleWeightsEstimator, "clang-cycle-weights",
  "Cycle Weights Estimator (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {
unsigned getInstWeight(Instruction *I) {
  return 0;  // TODO!
}
}

ClangCycleWeightsEstimator::ClangCycleWeightsEstimator() : ModulePass(ID) {
  initializeClangCycleWeightsEstimatorPass(*PassRegistry::getPassRegistry());
}

bool ClangCycleWeightsEstimator::runOnModule(Module &M) {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::map<Function *, unsigned> FunctionWeights;
  std::map<MDNode *, unsigned> LoopWeights;
  for (scc_iterator<CallGraph *> SCCIt = scc_begin(&CG); !SCCIt.isAtEnd(); ++SCCIt) {
    for (auto *CGN : *SCCIt) {
      if (Function *F = CGN->getFunction()) {
        unsigned FunctionWeight = 0;
        for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
          FunctionWeight += getInstWeight(I);
        }
        FunctionWeights[F] = FunctionWeight;
        auto &LM = getAnalysis<LoopMatcherPass>().getMatcher();
        for (auto It = LM.begin(), E = LM.end(); It != E; ++It) {
          Loop *L = It->get<IR>;
          unsigned LoopBodyWeight = 0;
          for (BasicBlock *BB = L->block_begin(), E = L->block_end(); BB != E; ++BB) {
            unsigned BBWeight = 0;
            (for Instruction &I : BB) {
              BBWeight += getInstWeight(&I);
            }
            LoopBodyWeight += BBWeight;
          }
          unsigned LoopExecutionCount = 0;  // TODO!
          unsigned LoopWeight = LoopExecutionCount * LoopBodyWeight;
          LoopWeights[L->getLoopID()] = LoopWeight;
        }
      }
    }
  }

  return false;
}

void ClangCycleWeightsEstimator::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<ClangCountedRegionsExtractor>();
  AU.setPreservesAll();
}

ModulePass *llvm::createClangCycleWeightsEstimator() {
  return new ClangCycleWeightsEstimator();
}
