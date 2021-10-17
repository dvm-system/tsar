//===-RedundantEdgeElimination.cpp - Redundant Edge Elimination -*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// This file implements a pass to replace the always true conditional branch
// instructions with an unconditional branch instruction.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/IR/RedundantEdgeElimination.h"
#include <bcl/utility.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "red-edge"

using namespace llvm;

char RedundantEdgeEliminationPass::ID = 0;

INITIALIZE_PASS_BEGIN(RedundantEdgeEliminationPass, "red-edge",
  "Redundant Edge Elimination", true, false)
INITIALIZE_PASS_END(RedundantEdgeEliminationPass, "red-edge",
  "Redundant Edge Elimination", true, false)

FunctionPass * llvm::createRedundantEdgeEliminationPass() {
  return new RedundantEdgeEliminationPass;
}

bool RedundantEdgeEliminationPass::runOnFunction(Function &F) {
  LLVM_DEBUG(dbgs() << "[REDUNDANT EDGE]: analyze " << F.getName() << "\n");
  for (auto &BB : F.getBasicBlockList()) {
    if (auto *TI = BB.getTerminator()) {
      if (auto *BI = dyn_cast<BranchInst>(TI)) {
        LLVM_DEBUG(dbgs() << "[REDUNDANT EDGE] instruction: "; BI->dump());
        if (BI->isConditional() && isa<ConstantInt>(BI->getCondition())) {
          ReplaceInstWithInst(
              BI, cast<ConstantInt>(BI->getCondition())->equalsInt(0) ?
                  BranchInst::Create(BI->getSuccessor(1)) :
                  BranchInst::Create(BI->getSuccessor(0)));
          LLVM_DEBUG(dbgs() << "[REDUNDANT EDGE] after: "; BB.dump());
        }
      }
    }
  }
  LLVM_DEBUG(dbgs() << "[REDUNDANT EDGE]: leave " << F.getName() << "\n");
  return true;
}