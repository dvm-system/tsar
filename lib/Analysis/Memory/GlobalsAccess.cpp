//===- GlobalsAccess.cpp - Globals Access Collector -------------*- C++ -*-===//
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
// This file implements a pass to collect explicit accesses to global values in
// a function.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Attributes.h"
#include <bcl/utility.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

namespace {
class GlobalsAccessCollector : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  explicit GlobalsAccessCollector() : ModulePass(ID) {
    initializeGlobalsAccessCollectorPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
class GlobalsAccessStorage : public ImmutablePass, private bcl::Uncopyable {

public:
  static char ID;

  GlobalsAccessStorage() : ImmutablePass(ID) {
    initializeGlobalsAccessStoragePass(*PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    getAnalysis<GlobalsAccessWrapper>().set(mAccesses);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GlobalsAccessWrapper>();
    AU.setPreservesAll();
  }

  auto &getAccesses() noexcept { return mAccesses; }
  const auto &getAccesses() const noexcept { return mAccesses; }
private:
  GlobalsAccessMap mAccesses;
};
}

char GlobalsAccessStorage::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalsAccessStorage, "globals-accesses-is",
  "Globals Access Collector", true, true)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_END(GlobalsAccessStorage, "globals-accesses-is",
  "Globals Access Collector", true, true)

template<> char GlobalsAccessWrapper::ID = 0;
INITIALIZE_PASS(GlobalsAccessWrapper, "globals-accesses-iw",
  "Globals Access Collector (Immutable Wrapper)", true, true)

char GlobalsAccessCollector::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalsAccessCollector, "globals-accesses",
  "Globals Access Collector", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(GlobalsAccessCollector, "globals-accesses",
  "Globals Access Collector", true, true)

void GlobalsAccessCollector::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.setPreservesAll();
}

bool GlobalsAccessCollector::runOnModule(Module &M) {
  auto &CG{getAnalysis<CallGraphWrapperPass>().getCallGraph()};
  auto &GAP{getAnalysis<GlobalsAccessWrapper>()};
  if (!GAP)
    return false;
  auto &Accesses{GAP.get()};
  for (auto SCC{scc_begin(&CG)}; !SCC.isAtEnd(); ++SCC) {
    bool IsChanged{false}, IsUnknown{false};
    DenseSet<GlobalVariable *> SCCAccesses;
    do {
      IsChanged = false;
      for (auto *CGN : *SCC) {
        auto *F{CGN->getFunction()};
        if (!F ||
            (F->empty() && !F->isIntrinsic() &&
             hasFnAttr(*F, AttrKind::LibFunc) && !F->onlyAccessesArgMemory())) {
          IsChanged = false;
          IsUnknown = true;
          break;
        }
        for (auto &Callee : *CGN) {
          if (Callee.second && Callee.second->getFunction()) {
            if (auto I{Accesses.find(Callee.second->getFunction())};
                I != Accesses.end())
              for (auto &G: I->second)
                IsChanged |= SCCAccesses.insert(cast<GlobalVariable>(G)).second;
          } else {
            IsChanged = false;
            IsUnknown = true;
            break;
          }
        }
        if (IsUnknown)
          break;
      }
    } while (IsChanged);
    auto storeAccesses = [&Accesses, SCC](auto I, auto EI) {
      for (auto *CGN : *SCC) {
        auto *F{ CGN->getFunction() };
        if (F && !F->isIntrinsic() && !hasFnAttr(*F, AttrKind::LibFunc) &&
            !F->onlyAccessesArgMemory())
          std::copy(I, EI, std::back_inserter(Accesses[F]));
      }
    };
    if (IsUnknown) {
      storeAccesses(pointer_iterator{ M.global_begin() },
        pointer_iterator{ M.global_end() });
    } else {
      for (auto *CGN : *SCC) {
        auto *F{CGN->getFunction()};
        assert(F && "Function must not be null!");
        if (F->empty())
          continue;
        auto &TLI{getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F)};
        auto &DL{M.getDataLayout()};
        for (auto &I : instructions(F))
          for (auto &Op : I.operands()) {
            Value *Prev{Op}, *Curr{Op};
            do {
              Prev = Curr;
              Curr = getUnderlyingObject(Op, 0);
              if (Operator::getOpcode(Curr) == Instruction::PtrToInt)
                Curr = getUnderlyingObject(cast<Operator>(Curr)->getOperand(0),
                                           0);
            } while (Curr != Prev);
            if (auto *GV{dyn_cast<GlobalVariable>(Curr)})
              SCCAccesses.insert(const_cast<GlobalVariable *>(GV));
          }
      }
      storeAccesses(SCCAccesses.begin(), SCCAccesses.end());
    }
  }
  return false;
}

ModulePass *llvm::createGlobalsAccessCollector() {
  return new GlobalsAccessCollector;
}

ImmutablePass *llvm::createGlobalsAccessStorage() {
  return new GlobalsAccessStorage;
}
