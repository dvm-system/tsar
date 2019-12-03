//===--- DefinedMemory.cpp --- Defined Memory Analysis ----------*- C++ -*-===//
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
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <tsar/Transform/IR/SplitBasicBlocksWithCallInst.h>

using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "Split-BB"

char SplitBasicBlocksWithCallInstPass::ID = 0;
INITIALIZE_PASS_BEGIN(SplitBasicBlocksWithCallInstPass, "Split-BB",
  "Split BB in call instraction", false, true)
INITIALIZE_PASS_END(SplitBasicBlocksWithCallInstPass, "Split-BB",
    "Split BB in call instraction", false, true)

void SplitBasicBlocksWithCallInstPass::getAnalysisUsage(AnalysisUsage & AU) const {
}

FunctionPass * llvm::createSplitBasicBlocksWithCallInstPass() {
  return new SplitBasicBlocksWithCallInstPass();
}

bool SplitBasicBlocksWithCallInstPass::runOnFunction(Function& F) {
  LLVM_DEBUG(
    dbgs() << "[SPLIT_BASIC_BLOCK_WITH_CALL_INST]: "
      << "Begin of SplitBasicBlocksWithCallInstPass\n";
    dbgs() << "[SPLIT_BASIC_BLOCK_WITH_CALL_INST]: " << F.getName();
  );
  if (F.hasName() && !F.empty()) {
    for (auto currBB = F.begin(), lastBB = F.end(); currBB != lastBB; ++currBB) {
      LLVM_DEBUG(
        dbgs() << "[SPLIT_BASIC_BLOCK_WITH_CALL_INST]: "
          << "Current BBname: " << currBB->getName() << "\n";
      );
      TerminatorInst* ti = currBB->getTerminator();

      for (auto currInstr = currBB->begin(), lastInstr = currBB->end();
        currInstr != lastInstr; ++currInstr) {
        if (ti != nullptr ) {
          Instruction* i = &*currInstr;
          if (i == ti)
            break;
          BasicBlock* newBB;
          if (auto* callInst = dyn_cast<CallInst>(i)) {
            if (i != ti) {
              if (i == &*(currBB->begin())) {
                newBB = &*currBB;
              }
              else {
                newBB = currBB->splitBasicBlock(callInst);
              }
            }
            auto nextInstr = callInst->getNextNonDebugInstruction();
            if (nextInstr != ti) {
              newBB->splitBasicBlock(nextInstr);
              currBB++;
              break;
            }
          }
        }
      }
    }
  }

  return true;
}