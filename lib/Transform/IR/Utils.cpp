//===----- Utils.h -------- Utility Methods and Classes ---------*- C++ -*-===//
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
// This file defines helper methods for IR-level transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/IR/Utils.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace tsar;

namespace {
Value * cloneChainImpl(Value *From, Instruction *BoundInst, DominatorTree *DT,
    std::size_t BeforeCloneIdx, SmallVectorImpl<Instruction *> &CloneList) {
  assert(From && "Cloned value must not be null!");
  if (auto *AI = dyn_cast<AllocaInst>(From)) {
    assert(&AI->getFunction()->getEntryBlock() == AI->getParent() &&
      "Alloca instructions must be presented in an entry block only!");
    return From;
  }
  auto *I = dyn_cast<Instruction>(From);
  if (!I)
    return From;
  if (BoundInst && DT && DT->dominates(I, BoundInst))
    return From;
  if (isa<PHINode>(I)) {
    for (auto Idx = BeforeCloneIdx + 1, IdxE = CloneList.size();
         Idx != IdxE; ++Idx)
      CloneList[Idx]->deleteValue();
    CloneList.resize(BeforeCloneIdx + 1);
    return nullptr;
  }
  auto *Clone = I->clone();
  CloneList.push_back(Clone);
  for (unsigned OpIdx = 0, OpIdxE = Clone->getNumOperands();
       OpIdx < OpIdxE; ++OpIdx) {
    auto *Op = Clone->getOperand(OpIdx);
    auto *OpClone =
      cloneChainImpl(Op, BoundInst, DT, BeforeCloneIdx, CloneList);
    if (!OpClone)
      return nullptr;
    if (OpClone != Op)
      Clone->setOperand(OpIdx, OpClone);
  }
  return Clone;
}
}

namespace tsar {
bool cloneChain(Instruction *From,
    SmallVectorImpl<Instruction *> &CloneList,
    Instruction *BoundInst, DominatorTree *DT) {
  return cloneChainImpl(From, BoundInst, DT, CloneList.size(), CloneList);
}

bool findNotDom(Instruction *From, Instruction *BoundInst, DominatorTree *DT,
    SmallVectorImpl<Use *> &NotDom) {
  assert(From && "Instruction must not be null!");
  assert(BoundInst && "Bound instruction must not be null!");
  assert(DT && "Dominator tree must not be null!");
  if (!DT->dominates(From, BoundInst))
    return true;
  for (auto &Op : From->operands())
    if (auto *I = dyn_cast<Instruction>(&Op))
      if (findNotDom(I, BoundInst, DT, NotDom))
        NotDom.push_back(&Op);
  return false;
}
}
