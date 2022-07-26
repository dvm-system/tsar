//===- RegionWeights.h ----- Region Weights Estimator  ----------*- C++ -*===//
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
// This file declares a pass to estimate weight of functions, loops and other
// regions in a source code.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_REGION_WEIGHTS_H
#define TSAR_REGION_WEIGHTS_H

#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Support/Tags.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Pass.h>
#include <llvm/Support/InstructionCost.h>

namespace llvm {
class Instruction;
class TargetTransformInfo;

namespace coverage {
class CoverageData;
}

/// This per-module pass performs estimation of loop weights
class RegionWeightsEstimator : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  RegionWeightsEstimator() : ModulePass(ID) {
    initializeRegionWeightsEstimatorPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  auto getTotalWeight(Value *V) const {
    assert(V && "Value must not be null!");
    auto I{mTotalWeights.find(V)};
    if (I != mTotalWeights.end())
      return I->second;
    return InstructionCost::getInvalid();
  }

  auto getTotalWeight(tsar::ObjectID LoopID) const {
    assert(LoopID && "Value must not be null!");
    auto I{mLoopTotalWeights.find(LoopID)};
    if (I != mLoopTotalWeights.end())
      return I->second;
    return std::pair{InstructionCost::getInvalid(), uint64_t{0}};
  }

  auto getSelfWeight(Value *V) const {
    assert(V && "Value must not be null!");
    auto I{mSelfWeights.find(V)};
    if (I != mSelfWeights.end())
      return I->second;
    return InstructionCost::getInvalid();
  }

  auto getSelfWeight(tsar::ObjectID LoopID) const {
    assert(LoopID && "Value must not be null!");
    auto I{mLoopSelfWeights.find(LoopID)};
    if (I != mLoopSelfWeights.end())
      return I->second;
    return std::pair{InstructionCost::getInvalid(), uint64_t{0}};
  }

private:
  std::pair<InstructionCost, InstructionCost>
  getWeight(const Instruction &I, const coverage::CoverageData &Coverage,
            TargetTransformInfo &TTI, unsigned UnknownFunctionWeight,
             unsigned UknownBuiltinWeight);

  DenseMap<Value *, InstructionCost> mTotalWeights;
  DenseMap<Value *, InstructionCost> mSelfWeights;
  DenseMap<tsar::ObjectID, std::pair<InstructionCost, uint64_t>>
      mLoopTotalWeights;
  DenseMap<tsar::ObjectID, std::pair<InstructionCost, uint64_t>>
      mLoopSelfWeights;
};
}
#endif//TSAR_REGION_WEIGHTS_H
