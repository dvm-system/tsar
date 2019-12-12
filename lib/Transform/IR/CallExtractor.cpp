//===--- SplitBasicBlocksWithCallInst.cpp --- Split Basic Block Tranform ----------*- C++ -*-===//
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
// This file implements a pass to extract each call instruction (except debug instructions) into its own new basic block.
//
//===----------------------------------------------------------------------===//
#include "tsar/Transform/IR/Passes.h"
#include <bcl/utility.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
using namespace llvm;

namespace {
  class CallExtractorPass : public FunctionPass, private bcl::Uncopyable {
  public:
    static char ID;
    CallExtractorPass() : FunctionPass(ID) {
      initializeCallExtractorPassPass(*PassRegistry::getPassRegistry());
    }

    bool runOnFunction(Function& F) override;
  };
}

#undef DEBUG_TYPE
#define DEBUG_TYPE "extract-call"

char CallExtractorPass::ID = 0;
INITIALIZE_PASS_BEGIN(CallExtractorPass, "extract-call",
  "Extract calls into new basic block", false, false)
INITIALIZE_PASS_END(CallExtractorPass, "extract-call",
  "Extract calls into new basic block", false, false)

FunctionPass* llvm::createCallExtractorPass() {
  return new CallExtractorPass();
}

bool CallExtractorPass::runOnFunction(Function& F) {
  LLVM_DEBUG(
    dbgs() << "[EXTRACT CALL]: "
    << "Start processing of the function \n" << F.getName();
    dbgs() << "[EXTRACT CALL]: Befor transformation:\n";
    F.dump();
  );
  if (F.hasName() && !F.empty()) {
    for (auto CurrBB = F.begin(), LastBB = F.end();
      CurrBB != LastBB; ++CurrBB) {
      TerminatorInst* TermInst = CurrBB->getTerminator();
      if (TermInst == nullptr)
        continue;
      auto CurrInstr = CurrBB->begin();
      if (auto CallCurrInst = dyn_cast<CallInst>(&*CurrInstr)) {
        auto NextInstr = CallCurrInst->getNextNonDebugInstruction();
        if (NextInstr != TermInst)
          CurrBB->splitBasicBlock(NextInstr);
      }
      else {
        for (Instruction* i = &*(++CurrInstr); i != TermInst;
          ++CurrInstr, i = &*CurrInstr) {
          if (auto* CallCurrInst = dyn_cast<CallInst>(i)) {
            BasicBlock* NewBB = CurrBB->splitBasicBlock(CallCurrInst);
            auto NextInstr = CallCurrInst->getNextNonDebugInstruction();
            if (NextInstr != TermInst)
              NewBB->splitBasicBlock(NextInstr);
            break;
          }
        }
      }
    }
  }
  LLVM_DEBUG(
    dbgs() << "[EXTRACT CALL]: " << F.getName() 
    << " After transformation:\n";
    F.dump();
    dbgs() << "[EXTRACT CALL]: "
      << "End processing of the function \n" << F.getName();
  );
  return true;
}

