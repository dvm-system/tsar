//===- AssumptionInfo.h ----- Assumption Information ------------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
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
//===----------------------------------------------------------------------===//
//
// This file provides assumption information for memory locations with variable
// bounds.
//
//===----------------------------------------------------------------------===/

#ifndef TSAR_ASSUMPTION_INFO_H
#define TSAR_ASSUMPTION_INFO_H

#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
class Value;
}

namespace tsar {
  typedef llvm::Optional<int64_t> AssumptionBound;
  struct AssumptionBounds {
    AssumptionBound Lower = llvm::None;
    AssumptionBound Upper = llvm::None;
  };

  typedef llvm::DenseMap<llvm::Value *, AssumptionBounds> AssumptionMap; 
}

namespace llvm {
class AssumptionInfoPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  AssumptionInfoPass() : FunctionPass(ID) {
    initializeAssumptionInfoPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override { mAM.clear(); }

  const tsar::AssumptionMap & getAssumptionMap() const {
    return mAM;
  }
private:
  tsar::AssumptionMap mAM; 
};
}

#endif //TSAR_ASSUMPTION_INFO_H
