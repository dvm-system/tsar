//===- NotInitializedMemory.cpp - Not Initialized Memory Checker *- C++ -*-===//
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
// This file implements analysis pass which looks for a memory which has not
// been initialized before uses.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/Passes.h"
#include <DIEstimateMemory.h>
#include <DefinedMemory.h>
#include <DFRegionInfo.h>
#include <EstimateMemory.h>
#include <tsar_query.h>
#include <bcl/utility.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>

#include "tsar_dbg_output.h"

using namespace llvm;
using namespace tsar;

namespace {
class NotInitializedMemoryAnalysis : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  NotInitializedMemoryAnalysis() : FunctionPass(ID) {
    initializeNotInitializedMemoryAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const;

  void releaseMemory() override {
    mNotInitScalars.clear();
    mNotInitAggregates.clear();
    mDWLang.reset();
    mFunc = nullptr;
  }
private:
  DenseSet<DIMemory *> mNotInitScalars;
  DenseSet<DIMemory *> mNotInitAggregates;
  Optional<unsigned> mDWLang;
  Function *mFunc = nullptr;
};
}

char NotInitializedMemoryAnalysis::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(NotInitializedMemoryAnalysis, "di-no-init",
  "Not Initialized Memory Checker (Metadata)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_IN_GROUP_END(NotInitializedMemoryAnalysis, "di-no-init",
  "Not Initialized Memory Checker (Metadata)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

void NotInitializedMemoryAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.setPreservesAll();
}

bool NotInitializedMemoryAnalysis::runOnFunction(Function &F) {
  releaseMemory();
  mFunc = &F;
  if (!(mDWLang = getLanguage(F)))
    return false;  
  auto &DL = F.getParent()->getDataLayout();
  auto &DFI = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &DU = getAnalysis<DefinedMemoryPass>().getDefInfo();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  auto FuncItr = DU.find(DFI.getTopLevelRegion());
  for (auto &Loc : FuncItr->get<DefUseSet>()->getUses()) {
    auto *EM = AT.find(Loc);
    assert(EM && "Estimate memory must not be null!");
    auto *Root = EM->getTopLevelParent();
    auto Object = GetUnderlyingObject(Root->front(), DL, 0);
    if (isa<Function>(Object) || isa<GlobalIFunc>(Object) ||
        isa<ConstantData>(Object) || isa<ConstantAggregate>(Object))
      continue;
    if (isa<GlobalVariable>(Object) &&
        cast<GlobalVariable>(Object)->isConstant())
      continue;
    auto RawDIM = getRawDIMemoryIfExists(*EM, F.getContext(), DL, DT);
    while (!RawDIM && EM->getParent()) {
      EM = EM->getParent();
      RawDIM = getRawDIMemoryIfExists(*EM, F.getContext(), DL, DT);
    }
    // If there is not metadata attached to a memory then generated DIMemory
    // will be always differs from the previously generated memory.
    // Before memory promotion estimate memory is created for function arguments
    // which are pointers. However, there are no metadata attached to these
    // arguments (metadata are attached to a related 'alloca' instructions).
    if (!RawDIM)
      continue;
    auto DIMItr = DIAT.find(*RawDIM);
    if (DIMItr == DIAT.memory_end())
      continue;
    if (EM->isLeaf())
      mNotInitScalars.insert(&*DIMItr);
    else
      mNotInitAggregates.insert(&*DIMItr);
  }
  return false;
}

void NotInitializedMemoryAnalysis::print(
    raw_ostream &OS, const Module *M) const {
  if (!mDWLang) {
    DiagnosticInfoUnsupported Diag(*mFunc,
      "function has not been analyzed due to absence of debug information",
      findMetadata(mFunc), DS_Warning);
    M->getContext().diagnose(Diag);
    return;
  }
  if (!mNotInitScalars.empty()) {
    OS << "  scalar memory:\n   ";
      for (auto *M : mNotInitScalars) {
        printDILocationSource(*mDWLang, *M, OS);
        OS << " ";
      }
    OS << "\n";
  }
  if (!mNotInitAggregates.empty()) {
    OS << "  aggregate memory:\n   ";
    for (auto *M : mNotInitAggregates) {
      printDILocationSource(*mDWLang, *M, OS);
      OS << " ";
    }
    OS << "\n";
  }
}
