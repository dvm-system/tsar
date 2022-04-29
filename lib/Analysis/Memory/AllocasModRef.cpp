//===- AllocasModRef.cpp - Simple Mod/Ref AA for Allocas --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a simple mod/ref and alias analysis over allocas.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/AllocasModRef.h"
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

static bool isNonAddressTaken(const Value *V) {
  for (auto &U : V->uses()) {
    auto *I = U.getUser();
    if (auto *SI = dyn_cast<StoreInst>(I)) {
      if (SI->getValueOperand() == V)
        return false;
    } else if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      if (!II->isLifetimeStartOrEnd())
        return false;
    } else if (auto *Call = dyn_cast<CallBase>(I)) {
      if (Call->isDataOperand(&U))
        return false;
    } else if (Operator::getOpcode(I) == Instruction::GetElementPtr ||
               Operator::getOpcode(I) == Instruction::BitCast) {
      if (!isNonAddressTaken(I))
        return false;
    } else if (!isa<LoadInst>(I)) {
      return false;
    }
  }
  return true;
}

void AllocasAAResult::analyzeFunction(const Function &F) {
  mNonAddressTakenAllocas.clear();
  for (auto &I : instructions(F))
    if (auto *AI = dyn_cast<AllocaInst>(&I); AI && isNonAddressTaken(AI))
      mNonAddressTakenAllocas.insert(AI);
}

AliasResult AllocasAAResult::alias(const MemoryLocation &LocA,
    const MemoryLocation &LocB, AAQueryInfo &AAQI) {
  auto P1 = getUnderlyingObject(LocA.Ptr, 0);
  auto P2 = getUnderlyingObject(LocB.Ptr, 0);
  if (P1 != P2 && isIdentifiedObject(P1) && isIdentifiedObject(P2))
    return AliasResult::NoAlias;
  return AAResultBase::alias(LocA, LocB, AAQI);
}

ModRefInfo AllocasAAResult::getModRefInfo(const CallBase *Call,
    const MemoryLocation &Loc, AAQueryInfo &AAQI) {
  auto P = getUnderlyingObject(Loc.Ptr, 0);
  if (auto *AI = dyn_cast<AllocaInst>(P);
      AI && mNonAddressTakenAllocas.count(AI))
    return ModRefInfo::NoModRef;
  return AAResultBase::getModRefInfo(Call, Loc, AAQI);
}

char AllocasAAWrapperPass::ID = 0;
INITIALIZE_PASS(AllocasAAWrapperPass, "allocas-aa", "Allocas Alias Analysis",
                false, true)

ImmutablePass *llvm::createAllocasAAWrapperPass() {
  return new AllocasAAWrapperPass;
}

bool AllocasAAWrapperPass::doInitialization(Module &M) {
  mResult.reset(new AllocasAAResult(M.getDataLayout()));
  return false;
}

bool AllocasAAWrapperPass::doFinalization(Module &M) {
  mResult.reset();
  return false;
}
