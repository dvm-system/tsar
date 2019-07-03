//===- LockTraitPass.cpp - Pass To Lock Traits (Metadata) -------*- C++ -*-===//
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
// This file defines a pass which lock some metadata-level memory traits
// according to a result of a specified functor which returns true if a trait
// should be locked. Type of a functor is `bool(const DIMemoryTrait &T)`.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/Analysis/LoopPass.h>
#include <bcl/utility.h>
#include <functional>

using namespace llvm;
using namespace tsar;

namespace {
/// Mark traits in a loop as locked if a specified functor returns `true`.
class LockDIMemoryTraitPass : public LoopPass, private bcl::Uncopyable {
public:
  /// Functor which check whether a trait should be locked.
  using FilterT = std::function<bool(const DIMemoryTrait &T)>;

  static char ID;

  LockDIMemoryTraitPass(
      const FilterT &Lock = [](const DIMemoryTrait &) { return false; }):
    LoopPass(ID), mLock(Lock) {
    initializeLockDIMemoryTraitPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired< DIMemoryTraitPoolWrapper>();
    AU.setPreservesAll();
  }

private:
  FilterT mLock;
};
}

char LockDIMemoryTraitPass::ID = 0;
INITIALIZE_PASS_BEGIN(LockDIMemoryTraitPass, "da-di-lock",
  "Metadata Level Trait Locker", false, true)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_END(LockDIMemoryTraitPass, "da-di-lock",
  "Metadata Level Trait Locker", false, true)

Pass * llvm::createLockDIMemoryTraitPass(
    const LockDIMemoryTraitPass::FilterT &Lock) {
  return new LockDIMemoryTraitPass(Lock);
}

bool LockDIMemoryTraitPass::runOnLoop(Loop *L, LPPassManager &LPM) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto LoopID = L->getLoopID();
  if (!LoopID)
    return false;
  auto PoolItr = TraitPool.find(LoopID);
  if (PoolItr == TraitPool.end())
    return false;
  for (auto &T : *PoolItr->get<Pool>()) {
    if (mLock(T))
      T.set<trait::Lock>();
  }
  return false;
}
