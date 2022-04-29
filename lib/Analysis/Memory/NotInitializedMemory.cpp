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

#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Unparse/Utils.h"
#include <bcl/utility.h>
#include <bcl/tagged.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "no-init-di"

namespace {
#define TSAR_TRAIT_DECL(name_, string_) \
struct name_ { \
static llvm::StringRef toString() { \
  static std::string Str(string_); \
  return Str; \
} \
};

TSAR_TRAIT_DECL(Argument, "argument")
TSAR_TRAIT_DECL(Unknown, "unknown")
TSAR_TRAIT_DECL(Global, "global")
TSAR_TRAIT_DECL(Local, "local")

#undef TSAR_DRAIT_DECL

/// This pass which looks for a memory which has not been initialized
/// before uses.
class NotInitializedMemoryAnalysis :
    public FunctionPass, private bcl::Uncopyable {
  using Variables = bcl::tagged_tuple<
    bcl::tagged<DenseSet<DIMemory *>, Local>,
    bcl::tagged<DenseSet<DIMemory *>, Global>,
    bcl::tagged<SmallPtrSet<DIMemory *, 4>, Argument>,
    bcl::tagged<SmallPtrSet<DIMemory *, 1>, Unknown>>;

  struct ClearFunctor {
    template<class T> void operator()(typename T::type &El) { El.clear(); }
  };

  struct EraseFunctor {
    template<class T> void operator()(typename T::type &El) { El.erase(DIM); }
    DIMemory *DIM;
  };

  struct PrintFunctor {
    template<class T> void operator()(const typename T::type &El) {
      if (!El.empty()) {
        OS << Offset << Prefix << T::tag::toString() << ":\n" << Offset << " ";
        for (auto *M : El) {
          printDILocationSource(DWLang, *M, OS);
          OS << " ";
        }
        OS << "\n";
      }
    }

    StringRef Prefix;
    StringRef Offset;
    unsigned DWLang;
    raw_ostream &OS;
  };

public:
  static char ID;

  NotInitializedMemoryAnalysis() : FunctionPass(ID) {
    initializeNotInitializedMemoryAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;

  void releaseMemory() override {
    clear(mNotInitScalars);
    clear(mNotInitAggregates);
    mDWLang.reset();
    mFunc = nullptr;
    mNoMetadataLocs = false;
  }
private:
  /// Remove all variables from a specified tuple.
  void clear(Variables &Vars) { bcl::for_each(Vars, ClearFunctor{}); }

  /// Remove a specified variable from a specified tuple.
  void erase(DIMemory *DIM, Variables &Vars) {
    bcl::for_each(Vars, EraseFunctor{ DIM });
  }

  /// Insert variable into an appropriate list into a specified tuple.
  void insert(DIMemory *M, Variables &Vars);

  void print(StringRef Prefix, const Variables &Vars, raw_ostream &OS) const {
    assert(mDWLang && "Language must be specified!");
    bcl::for_each(Vars, PrintFunctor{ Prefix, "  ", *mDWLang, OS });
  }

  Variables mNotInitScalars;
  Variables mNotInitAggregates;
  Optional<unsigned> mDWLang;
  Function *mFunc = nullptr;
  bool mNoMetadataLocs = false;
};
}

char NotInitializedMemoryAnalysis::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(NotInitializedMemoryAnalysis, "no-init-di",
  "Not Initialized Memory Checker (Metadata)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_IN_GROUP_END(NotInitializedMemoryAnalysis, "no-init-di",
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
  DenseMap<EstimateMemory *, DIMemory *> NotInitializedLocs;
  for (auto &Loc : FuncItr->get<DefUseSet>()->getUses()) {
    auto *EM = AT.find(Loc);
    assert(EM && "Estimate memory must not be null!");
    auto *Root = EM->getTopLevelParent();
    auto Object = getUnderlyingObject(Root->front(), 0);
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
    if (!RawDIM) {
      if (!isa<llvm::Argument>(Object))
        mNoMetadataLocs = true;
      continue;
    }
    auto DIMItr = DIAT.find(*RawDIM);
    if (DIMItr == DIAT.memory_end())
      continue;
    if (EM->isLeaf())
      insert(&*DIMItr, mNotInitScalars);
    else
      insert(&*DIMItr, mNotInitAggregates);
    NotInitializedLocs.try_emplace(EM, &*DIMItr);
  }
  // We want to remember the largest memory only. So, check whether a parent
  // for a not initialized memory is not initialized also.
  for (auto &EMToM : NotInitializedLocs) {
    auto *Parent = EMToM.first->getParent();
    while (Parent) {
      auto Itr = NotInitializedLocs.find(Parent);
      if (Itr != NotInitializedLocs.end()) {
        erase(EMToM.second, mNotInitScalars);
        erase(EMToM.second, mNotInitAggregates);
        break;
      }
      Parent = Parent->getParent();
    }
  }
  return false;
}

void NotInitializedMemoryAnalysis::insert(DIMemory *M, Variables &Vars) {
  if (auto *DIEM = dyn_cast<DIEstimateMemory>(M)) {
    if (auto *DILocalVar = dyn_cast<DILocalVariable>(DIEM->getVariable())) {
      if (DILocalVar->isParameter())
        Vars.get<Argument>().insert(M);
      else
        Vars.get<Local>().insert(M);
    } else {
      Vars.get<Global>().insert(M);
    }
  } else {
    Vars.get<Unknown>().insert(M);
  }
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
  if (mNoMetadataLocs) {
    DiagnosticInfoUnsupported Diag(*mFunc,
      "some memory locations without attached metadata have not been analyzed",
      findMetadata(mFunc), DS_Warning);
    M->getContext().diagnose(Diag);
  }
  print("scalar ", mNotInitScalars, OS);
  print("aggregate ", mNotInitAggregates, OS);
}
