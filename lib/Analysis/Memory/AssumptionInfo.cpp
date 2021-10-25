//===- AssumptionInfo.cpp --- Assumption Information ------------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file provides assumption information for memory locations with variable
// bounds.
//
//===----------------------------------------------------------------------===/

#include "tsar/Analysis/Memory/AssumptionInfo.h"
#include "tsar/Core/Query.h"
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/InitializePasses.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "assumption-info"

char AssumptionInfoPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(AssumptionInfoPass, "assumption-info",
  "Assumption information extractor", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_IN_GROUP_END(AssumptionInfoPass, "assumption-info",
  "Assumption information extractor", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

namespace {
/// Fills AssumptionMap AM with information extracted from AssumptionCache AC.
/// AssumptionMap maps llvm::Value* to AssumptionBounds - a pair of two optional
/// integers: Lower and Upper. AC stores expressions collected from arguments of
/// the intrinsic `__builtin_assume'. Consider the following example:
///
/// __builtin_assume(x > 0);
/// __builtin_assume(x <= 100);
///
/// For the llvm::Value *V corresponding to variable X, the following
/// AssumptionBounds will be set: Lower = 1, Upper = 100. In essence,
/// AssumptionBounds is a segment that symbolizes the set of values ​​that X
/// can take. If Lower or Upper are not set, it is considered that the
/// corresponding segment boundary becomes equal to infinity. If several values
/// are defined for the lower boundary of the segment, the maximum is selected
/// from them. For the upper bound, the minimum is similarly selected.
void initializeAssumptions(AssumptionMap &AM, AssumptionCache &AC) {
  auto UpdateBound = [](ConstantInt *C, Value *V, CmpInst::Predicate Pred,
                        AssumptionBounds &Bounds) {
    auto CV = C->getSExtValue();
    switch (Pred)
    {
    case CmpInst::Predicate::ICMP_UGE:
    case CmpInst::Predicate::ICMP_SGE:
      Bounds.Upper = Bounds.Upper ? std::min(CV, *Bounds.Upper) : CV;
      break;
    case CmpInst::Predicate::ICMP_UGT:
    case CmpInst::Predicate::ICMP_SGT:
      Bounds.Upper = Bounds.Upper ? std::min(CV - 1, *Bounds.Upper) :
                                             CV - 1;
      break;
    case CmpInst::Predicate::ICMP_ULE:
    case CmpInst::Predicate::ICMP_SLE:
      Bounds.Lower = Bounds.Lower ? std::max(CV, *Bounds.Lower) : CV;
      break;
    case CmpInst::Predicate::ICMP_ULT:
    case CmpInst::Predicate::ICMP_SLT:
      Bounds.Lower = Bounds.Lower ? std::max(CV + 1, *Bounds.Lower) :
                                             CV + 1;
      break;
    default:
      llvm_unreachable("Predicate must be relational!");
    }
  };
  for (auto &AssumeVH : AC.assumptions()) {
    if (!AssumeVH)
      continue;
    if (auto *I = dyn_cast<CallInst>(AssumeVH.Assume)) {
      auto *CI = dyn_cast<CmpInst>(I->getArgOperand(0));
      if (CI && CI->isIntPredicate() && CI->isRelational()) {
        assert(CI->getNumOperands() == 2 &&
            "Number of operands for CmpInst must be 2!");
        auto *Op0 = CI->getOperand(0);
        auto *Op1 = CI->getOperand(1);
        auto *C0 = dyn_cast<ConstantInt>(Op0);
        auto *C1 = dyn_cast<ConstantInt>(Op1);
        if (C0 && !C1) {
          auto &Bounds = AM.insert(
              std::make_pair(Op1, AssumptionBounds())).
              first->second;
          UpdateBound(C0, Op1, CI->getPredicate(), Bounds);
        } else if (!C0 && C1) {
          auto &Bounds = AM.insert(
              std::make_pair(Op0, AssumptionBounds())).
              first->second;
          UpdateBound(C1, Op0, CI->getSwappedPredicate(), Bounds);
        }
      }
    }
  }
  LLVM_DEBUG(
    dbgs() << "[ASSUME] Bounds: ";
    for (auto &Pair : AM) {
      dbgs() << "<";
      Pair.first->print(dbgs(), true);
      dbgs() << ", [" << Pair.second.Lower << ", " << Pair.second.Upper << "]> ";
    }
    dbgs() << "\n";
  );
}

#ifdef LLVM_DEBUG
void dumpAssumptions(llvm::AssumptionCache &AC, llvm::Value *For=nullptr) {
  dbgs() << "[ASSUME] AssumptionCache: ";
  auto AL = For ? AC.assumptionsFor(For) : AC.assumptions();
  for (auto &AssumeVH : AL) {
    if (!AssumeVH)
      continue;
    AssumeVH.Assume->dump();
  }
}
#endif
}

bool AssumptionInfoPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  LLVM_DEBUG(dumpAssumptions(AC));
  initializeAssumptions(mAM, AC);
  return false;
}

void AssumptionInfoPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AssumptionCacheTracker>();
  AU.setPreservesAll();
}
