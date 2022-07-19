//===--- TraitFilter.cpp ----- Filters Of Traits ----------------*- C++ -*-===//
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
// This file defines some filters which marks memory location if some
// traits are set.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/TraitFilter.h"
#include <llvm/IR/DebugInfo.h>

using namespace llvm;

namespace tsar {
void markIfNotPromoted(const DataLayout &DL, DIMemoryTrait &T) {
  auto DIEM = dyn_cast<DIEstimateMemory>(T.getMemory());
  if (!DIEM)
    return;
  auto DIVar = DIEM->getVariable();
  for (auto VH : *DIEM) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    auto *AI = dyn_cast<AllocaInst>(stripPointer(DL, VH));
    if (!AI || AI->isArrayAllocation() ||
      AI->getAllocatedType()->isArrayTy())
      continue;
    bool DbgDeclareFound = false;
    for (auto *U : FindDbgAddrUses(AI)) {
      if (isa<DbgDeclareInst>(U) || U->getVariable() == DIVar) {
        T.set<trait::NoPromotedScalar>();
        return;
      }
    }
  }
}
}
