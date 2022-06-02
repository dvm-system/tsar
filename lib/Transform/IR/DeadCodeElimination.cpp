//===- DeadCodeElimination.cpp - Dead Code Elimination --------- *- C++ -*-===//
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
// This file implements additional passes which eliminates dead code. Note,
// that LLVM originally contains some passes to elimination dead code.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Transform/IR/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Pass.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "de-code"

using namespace llvm;
using namespace tsar;

namespace {
/// Eliminate dead stores which accesses values without attached metadata. It
/// also recursively eliminates operands of these stores if possible.
class NoMetadataDSEPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  NoMetadataDSEPass() : FunctionPass(ID) {
    initializeNoMetadataDSEPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
  }
};
}

char NoMetadataDSEPass::ID = 0;

INITIALIZE_PASS_BEGIN(NoMetadataDSEPass, "de-code",
  "No Metadata Dead Store Elimination", true, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(NoMetadataDSEPass, "de-code",
  "No Metadata Dead Store Elimination", true, false)

FunctionPass * llvm::createNoMetadataDSEPass() { return new NoMetadataDSEPass; }

bool NoMetadataDSEPass::runOnFunction(Function &F) {
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  AliasSetTracker Tracker(AA);
  for (auto &I : instructions(F))
      Tracker.add(&I);
  // Find stores which writes to a memory that is not used.
  DenseMap<AliasSet *, std::vector<Instruction *>> OnlyStores;
  for (auto &AS : Tracker)
    if (!AS.isForwardingAliasSet() && AS.isMod())
    OnlyStores.try_emplace(&AS);
  auto &DL = F.getParent()->getDataLayout();
  for (auto &I : instructions(F)) {
    for_each_memory(I, TLI,
      [&DL, &Tracker, &OnlyStores](Instruction &I, MemoryLocation &&Loc,
          unsigned, AccessInfo, AccessInfo) {
        auto *Ptr = const_cast<Value *>(Loc.Ptr);
        auto &AS = Tracker.getAliasSetFor(Loc);
        auto Itr = OnlyStores.find(&AS);
        if (Itr == OnlyStores.end())
          return;
        auto Info = GetUnderlyingObjectWithMetadata(Ptr, DL);
        if (!isa<AllocaInst>(Info.first) || Info.second || !isa<StoreInst>(I))
          OnlyStores.erase(Itr);
        else
          Itr->second.push_back(&I);
      },
      [&AA, &Tracker, &OnlyStores](Instruction &I, AccessInfo, AccessInfo) {
        for (auto &AS : Tracker) {
          if (!AS.isForwardingAliasSet() && AS.aliasesUnknownInst(&I, AA))
            OnlyStores.erase(&AS);
        }
    });
  }
  // Now, we eliminate all stores which writes to a memory that is not used.
  // We also recursively eliminate operands of each remove instruction which
  // become unused after instruction elimination.
  for (auto &ASToStore : OnlyStores)
    for (auto *SI : ASToStore.second) {
      SmallVector<Instruction *, 8> Worklist;
      SmallPtrSet<Instruction *, 8> HasUses, Visited;
      for (auto &Op : SI->operands())
        if (auto *I = dyn_cast<Instruction>(&Op))
          if (!I->mayReadOrWriteMemory())
            if (Visited.insert(I).second)
              Worklist.push_back(I);
      SI->eraseFromParent();
      for (;;) {
        bool IsChanged{false};
        while (!Worklist.empty()) {
          auto *Inst = Worklist.pop_back_val();
          Visited.erase(Inst);
          if (Inst->getNumUses() > 0) {
            HasUses.insert(Inst);
          } else {
            for (auto &Op : Inst->operands())
              if (auto *I = dyn_cast<Instruction>(&Op))
                if (!I->mayReadOrWriteMemory())
                  if (Visited.insert(I).second)
                    Worklist.push_back(I);
            HasUses.erase(Inst);
            Inst->eraseFromParent();
            IsChanged = true;
          }
        }
        if (!IsChanged)
          break;
        Visited = HasUses;
        Worklist.insert(Worklist.end(), HasUses.begin(), HasUses.end());
      }
    }
  return true;
}
