//===--- Utils.cpp ------------ Memory Utils ---------------------*- C++ -*===//
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
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/Utils.h"
#include "DefinedMemory.h"
#include "EstimateMemory.h"
#include "MemoryAccessUtils.h"
#include "SpanningTreeRelation.h"
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>

using namespace llvm;
using namespace tsar;

bool tsar::isLoopInvariant(const SCEV *Expr, const Loop *L,
    TargetLibraryInfo &TLI, ScalarEvolution &SE, const DefUseSet &DUS,
    const AliasTree &AT, const SpanningTreeRelation<const AliasTree *> &STR) {
  assert(Expr && "Expression must not be null!");
  assert(L && "Loop must not be null!");
  if (isa<SCEVCouldNotCompute>(Expr))
    return false;
  if (!SE.isLoopInvariant(Expr, L)) {
    if (auto *Cast = dyn_cast<SCEVCastExpr>(Expr))
      Expr = Cast->getOperand();
    if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(Expr)) {
      if (!L || AddRec->getLoop() == L || L->contains(AddRec->getLoop()))
        return false;
      if (AddRec->getLoop()->contains(L))
        return true;
      for (auto *Op : AddRec->operands())
        if (!isLoopInvariant(Op, L, TLI, SE, DUS, AT, STR))
          return false;
    } else if (auto *NAry = dyn_cast<SCEVNAryExpr>(Expr)) {
      for (auto *Op : NAry->operands())
        if (!isLoopInvariant(Op, L, TLI, SE, DUS, AT, STR))
          return false;
    } else if (auto *UDiv = dyn_cast<SCEVUDivExpr>(Expr)) {
      return isLoopInvariant(UDiv->getLHS(), L, TLI, SE, DUS, AT, STR) &&
        isLoopInvariant(UDiv->getRHS(), L, TLI, SE, DUS, AT, STR);
    } else {
      if (auto *I = dyn_cast<Instruction>(cast<SCEVUnknown>(Expr)->getValue()))
        return (L && isLoopInvariant(*I, *L, TLI, DUS, AT, STR)) ||
               (!L && isFunctionInvariant(*I, TLI, DUS, AT, STR));
      return true;
    }
  }
  return true;
}

template<class FunctionT>
static inline bool isRegionInvariant(Instruction &I,
    TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR, FunctionT &&Contains) {
  if (!Contains(I))
    return true;
  if (isa<PHINode>(I))
    return false;
  if (!accessInvariantMemory(I, TLI, DUS, AT, STR))
    return false;
  for (auto &Op : I.operands()) {
    if (auto *I = dyn_cast<Instruction>(Op)) {
      if (!isRegionInvariant(*I, TLI, DUS, AT, STR, Contains))
        return false;
    }
  }
  return true;
}

namespace tsar {
bool isLoopInvariant(llvm::Instruction &I, const llvm::Loop &L,
    llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  return isRegionInvariant(I, TLI, DUS, AT, STR,
    [&L](const Instruction &I) {return L.contains(&I); });
}

bool isFunctionInvariant(llvm::Instruction &I,
    llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  return isRegionInvariant(I, TLI, DUS, AT, STR,
    [](const Instruction &) {return true; });
}

bool isBlockInvariant(llvm::Instruction &I, const BasicBlock &BB,
    llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  return isRegionInvariant(I, TLI, DUS, AT, STR,
    [&BB](const Instruction &I) {return I.getParent() == &BB; });
}

bool accessInvariantMemory(Instruction &I, TargetLibraryInfo &TLI,
    const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  bool Result = true;
  for_each_memory(I, TLI, [&DUS, &Result](
      Instruction &, MemoryLocation &&Loc,
      unsigned Idx, AccessInfo R, AccessInfo W) {
    Result &= !(DUS.hasDef(Loc) || DUS.hasMayDef(Loc));
  }, [&DUS, &AT, &STR, &Result](Instruction &I, AccessInfo, AccessInfo) {
    if (!Result)
      return;
    ImmutableCallSite CS(&I);
    Result &= CS && AT.getAliasAnalysis().onlyReadsMemory(CS);
    if (!Result)
      return;
    auto *AN = AT.findUnknown(I);
    for (auto &Loc : DUS.getExplicitAccesses()) {
      if (!DUS.hasDef(Loc) && !DUS.hasMayDef(Loc))
        continue;
      auto EM = AT.find(Loc);
      assert(EM && "Memory location must be presented in alias tree!");
      if (!STR.isUnreachable(EM->getAliasNode(AT), AN)) {
        Result = false;
        return;
      }
    }
    for (auto *Loc : DUS.getExplicitUnknowns()) {
      ImmutableCallSite CS(Loc);
      if (CS && AT.getAliasAnalysis().onlyReadsMemory(CS))
        continue;
      auto UN = AT.findUnknown(*Loc);
      assert(UN &&
        "Unknown memory location must be presented in alias tree!");
      if (!STR.isUnreachable(UN, AN)) {
        Result = false;
        return;
      }
    }
  });
  return Result;
}
}
