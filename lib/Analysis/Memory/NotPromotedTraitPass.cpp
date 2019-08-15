//= NotPromotedTraitPass.cpp - Collect Not Promoted Memory (Metadata) * C++ *=//
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
// This file defines a pass which collects locations which have not been
// promoted yet. Actually this pass looks for `dbg.declare` metadata and if it
// exist the pass marks appropriate location as a not promoted.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "EstimateMemory.h"
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Transforms/Utils/Local.h>
#include <bcl/utility.h>

using namespace llvm;
using namespace tsar;

namespace {
class NotPromotedDIMemoryTraitPass : public LoopPass, private bcl::Uncopyable {
public:
  static char ID;

  NotPromotedDIMemoryTraitPass() : LoopPass(ID) {
    initializeNotPromotedDIMemoryTraitPassPass(
      *PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DIMemoryTraitPoolWrapper>();
    AU.setPreservesAll();
  }
};
}


char NotPromotedDIMemoryTraitPass::ID = 0;
INITIALIZE_PASS_BEGIN(NotPromotedDIMemoryTraitPass, "di-not-promoted",
  "Not Promoted Memory Collector", false, true)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
  INITIALIZE_PASS_END(NotPromotedDIMemoryTraitPass, "di-not-promoted",
    "Not Promoted Memory Collector", false, true)

  Pass * llvm::createNotPromotedDIMemoryTraitPass() {
  return new NotPromotedDIMemoryTraitPass;
}

bool NotPromotedDIMemoryTraitPass::runOnLoop(Loop *L, LPPassManager &LPM) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto LoopID = L->getLoopID();
  if (!LoopID)
    return false;
  auto PoolItr = TraitPool.find(LoopID);
  if (PoolItr == TraitPool.end())
    return false;
  auto &DL = L->getHeader()->back().getModule()->getDataLayout();
  for (auto &T : *PoolItr->get<Pool>()) {
    auto DIEM = dyn_cast<DIEstimateMemory>(T.getMemory());
    if (!DIEM)
      continue;
    auto DIVar = DIEM->getVariable();
    for (auto VH : *DIEM) {
      if (!VH || isa<UndefValue>(VH))
        continue;
      auto *AI = dyn_cast<AllocaInst>(stripPointer(DL, VH));
      if (!AI || AI->isArrayAllocation() ||
        AI->getType()->getElementType()->isArrayTy())
        continue;
      bool DbgDeclareFound = false;
      for (auto *U : FindDbgAddrUses(AI)) {
        if (isa<DbgDeclareInst>(U) || U->getVariable() == DIVar) {
          DbgDeclareFound = true;
          break;
        }
      }
      if (DbgDeclareFound) {
        T.set<trait::NoPromotedScalar>();
        break;
      }
    }
  }
  return false;
}
