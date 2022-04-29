//===- ProcessTraitPass.cpp - Pass To Process Traits (Metadata) -*- C++ -*-===//
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
// This file defines a pass which process all metadata-level memory traits in
// a pool related to a region. The pass uses a functor to process each trait.
// Type of a functor is `void(DIMemoryTrait &T)`.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <bcl/utility.h>
#include <functional>

using namespace llvm;
using namespace tsar;

namespace {
/// Process each trait in a pool related to a specified region.
class ProcessDIMemoryTraitPass : public LoopPass, private bcl::Uncopyable {
public:
  /// Function which process a trait.
  using FunctionT = std::function<void(DIMemoryTrait &T)>;

  static char ID;

  ProcessDIMemoryTraitPass(const FunctionT &F = [](DIMemoryTrait &) {}) :
    LoopPass(ID), mFunc(F) {
    initializeProcessDIMemoryTraitPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired< DIMemoryTraitPoolWrapper>();
    AU.setPreservesAll();
  }

private:
  FunctionT mFunc;
};
}

char ProcessDIMemoryTraitPass::ID = 0;
// Do not mark this pass as analysis (last argument) because legacy pass manager
// contains a single representation for each analysis pass. And it is not
// possible to create the same pass twice with different parameters.
INITIALIZE_PASS_BEGIN(ProcessDIMemoryTraitPass, "da-di-functor",
  "Metadata Level Trait Functor", false, false)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_END(ProcessDIMemoryTraitPass, "da-di-functor",
  "Metadata Level Trait Functor", false, false)

Pass * llvm::createProcessDIMemoryTraitPass(
    const ProcessDIMemoryTraitPass::FunctionT &F) {
  return new ProcessDIMemoryTraitPass(F);
}

bool ProcessDIMemoryTraitPass::runOnLoop(Loop *L, LPPassManager &LPM) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto LoopID = L->getLoopID();
  if (!LoopID)
    return false;
  auto PoolItr = TraitPool.find(LoopID);
  if (PoolItr == TraitPool.end())
    return false;
  for (auto &T : *PoolItr->get<Pool>())
    mFunc(T);
  return false;
}
