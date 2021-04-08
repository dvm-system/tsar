//===- AllocasModRef.h --- Simple Mod/Ref AA for Allocas --------*- C++ -*-===//
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

#ifndef TSAR_ALLOCAS_MODREF_H
#define TSAR_ALLOCAS_MODREF_H

#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/AliasAnalysis.h>

namespace llvm {
class AllocaInst;
class DataLayout;

/// An alias result set for allocas.
class AllocasAAResult : public AAResultBase<AllocasAAResult> {
public:
  explicit AllocasAAResult(const DataLayout &DL) : mDL(DL) {}

  AliasResult alias(const MemoryLocation &LocA, const MemoryLocation &LocB,
                    AAQueryInfo &AAQI);

  using AAResultBase::getModRefInfo;
  ModRefInfo getModRefInfo(const CallBase *Call, const MemoryLocation &Loc,
                           AAQueryInfo &AAQI);

  void analyzeFunction(const Function &F);

private:
  SmallPtrSet<const AllocaInst *, 16> mNonAddressTakenAllocas;
  const DataLayout &mDL;
};

/// Analysis pass that provides the AllocasAAResult object.
class AllocasAAWrapperPass : public ImmutablePass {
public:
  static char ID;

  explicit AllocasAAWrapperPass() : ImmutablePass(ID) {
    initializeAllocasAAWrapperPassPass(*PassRegistry::getPassRegistry());
  }

  AllocasAAResult &getResult() { return *mResult; }
  const AllocasAAResult &getResult() const { return *mResult; }

  bool doInitialization(Module &M) override;
  bool doFinalization(Module &M) override;

private:
  std::unique_ptr<AllocasAAResult> mResult;
};
}
#endif//TSAR_ALLOCA_ALIAS_ANALYSIS_H
