//=== DistributionLimits.cpp - Limitation of Distribution Checker *- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements checkers to determine whether the distribution of arrays
// is possible.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include <apc/Distribution/Array.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-distribution-limits"


using namespace llvm;
using namespace tsar;

namespace {
class APCDistrLimitsChecker : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  APCDistrLimitsChecker() : FunctionPass(ID) {
    initializeAPCDistrLimitsCheckerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

char APCDistrLimitsChecker::ID = 0;
INITIALIZE_PASS_BEGIN(APCDistrLimitsChecker, "apc-distribution-limits",
                      "Distribution Limitation Checker (APC)", true, true)
INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(APCDistrLimitsChecker, "apc-distribution-limits",
                      "Distribution Limitation Checker (APC)", true, true)

FunctionPass *llvm::createAPCDistrLimitsChecker() {
  return new APCDistrLimitsChecker;
}

void APCDistrLimitsChecker::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

bool APCDistrLimitsChecker::runOnFunction(Function& F) {
  auto &APCCtx{*getAnalysis<APCContextWrapper>()};
  auto &DL{F.getParent()->getDataLayout()};
  auto &DT{getAnalysis<DominatorTreeWrapperPass>().getDomTree()};
  auto &AT{getAnalysis<EstimateMemoryPass>().getAliasTree()};
  auto &TLI{getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F)};
  for (auto &I : instructions(F)) {
    if (isa<StoreInst>(I) || isa<LoadInst>(I))
      continue;
    for_each_memory(
        I, TLI,
        [&APCCtx, &AT, &DL, &DT](Instruction &I, MemoryLocation &&Loc,
                                 unsigned OpIdx, AccessInfo IsRead,
                                 AccessInfo IsWrite) {
          auto *EM{AT.find(Loc)};
          assert(EM && "Estimate memory must be presented in alias tree!");
          auto *TopEM{EM->getTopLevelParent()};
          auto *RawDIM{getRawDIMemoryIfExists(*TopEM, I.getContext(), DL, DT)};
          if (!RawDIM)
            return;
          auto APCArray{APCCtx.findArray(RawDIM)};
          if (!APCArray || APCArray->IsNotDistribute())
            return;
          if (auto *II{dyn_cast<IntrinsicInst>(&I)}) {
            if (isMemoryMarkerIntrinsic(II->getIntrinsicID()))
              return;
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName() << " (intrinsic) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::NO_DISTR);
            return;
          }
          if (!isa<CallBase>(I) || EM != TopEM) {
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName()
                       << " (unsupported memory access) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::NO_DISTR);
            return;
          }
          auto *Callee{dyn_cast<Function>(
              cast<CallBase>(I).getCalledOperand()->stripPointerCasts())};
          if (!Callee || Callee->isDeclaration() ||
              hasFnAttr(*Callee, AttrKind::LibFunc)) {
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName() << " (unknown function) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::IO_PRIV);
            return;
          }
        },
        [](Instruction &I, AccessInfo IsRead, AccessInfo IsWrite) {});
  }
  return false;
}
