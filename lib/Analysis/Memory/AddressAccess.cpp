//===--- PrivateAnalysis.cpp - Private Variable Analyzer --------*- C++ -*-===//
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
// This file defines passes to determine formal arguments which can be
// preserved by the function (e.g. saved to the global memory).
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassProvider.h"
#include <llvm/ADT/DenseSet.h>
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Debug.h>
#include "tsar/Analysis/Memory/AddressAccess.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/InitializePasses.h"
#include <queue>
#include <unordered_set>

#undef DEBUG_TYPE
#define DEBUG_TYPE "address-access"

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

char AddressAccessAnalyser::ID = 0;

namespace {
  using FunctionPassesProvider =
  FunctionPassProvider<MemoryDependenceWrapperPass>;
}

INITIALIZE_PROVIDER_BEGIN(FunctionPassesProvider,
                          "function-passes-provider",
                          "MemoryDependenceWrapperPass provider")
  INITIALIZE_PASS_DEPENDENCY(MemoryDependenceWrapperPass)
INITIALIZE_PROVIDER_END(FunctionPassesProvider, "function-passes-provider",
                        "MemoryDependenceWrapperPass provider")

template<> char AddressAccessAnalyserWrapper::ID = 0;

INITIALIZE_PASS(AddressAccessAnalyserWrapper, "address-access-wrapper",
                "address-access (Immutable Wrapper)", true, true)

INITIALIZE_PASS_IN_GROUP_BEGIN(AddressAccessAnalyser, "address-access",
                               "address-access", false, true,
                               DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(AddressAccessAnalyserWrapper)
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(FunctionPassesProvider)
INITIALIZE_PASS_IN_GROUP_END(AddressAccessAnalyser, "address-access",
                             "address-access", false, true,
                             DefaultQueryManager::PrintPassGroup::getPassRegistry())

Pass *llvm::createAddressAccessAnalyserPass() {
  return new AddressAccessAnalyser();
}

void AddressAccessAnalyser::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AddressAccessAnalyserWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<FunctionPassesProvider>();
  AU.setPreservesAll();
}

void AddressAccessAnalyser::initDepInfo(llvm::Function *F) {
  FunctionPassesProvider *Provider = &getAnalysis<FunctionPassesProvider>(*F);
  auto MDA = &Provider->get<MemoryDependenceWrapperPass>().getMemDep();
  if (mDepInfo) {
    for (auto pair : *mDepInfo)
      delete (pair.second);
    delete (mDepInfo);
  }
  mDepInfo = new DependentInfo();
  for (BasicBlock &BB : F->getBasicBlockList())
    for (Instruction &I: BB) {
      Instruction *Inst = &I;
      if (!Inst->mayReadFromMemory() && !Inst->mayWriteToMemory())
        continue;
      MemDepResult Res = MDA->getDependency(Inst);
      if (!Res.isNonLocal()) {
        Instruction *target = Res.getInst();
        if (mDepInfo->find(target) == mDepInfo->end())
          (*mDepInfo)[target] = new InstrDependencies();
        if (!Res.isDef())
          (*mDepInfo)[target]->is_invalid = true;
        else
          (*mDepInfo)[target]->deps.push_back(&I);
      } else if (auto CB = dyn_cast<CallBase>((Inst))) {
        const MemoryDependenceResults::NonLocalDepInfo &NLDI =
                MDA->getNonLocalCallDependency(CB);
        for (const NonLocalDepEntry &NLDE : NLDI) {
          const MemDepResult &Res = NLDE.getResult();
          Instruction *target = Res.getInst();
          if (mDepInfo->find(target) == mDepInfo->end())
            (*mDepInfo)[target] = new InstrDependencies(true);
          else
            (*mDepInfo)[target]->is_invalid = true;
        }
      } else {
        SmallVector<NonLocalDepResult, 4> NLDI;
        assert((isa<LoadInst>(Inst) || isa<StoreInst>(Inst) ||
                isa<VAArgInst>(Inst)) && "Unknown memory instruction!");
        MDA->getNonLocalPointerDependency(Inst, NLDI);
        for (const NonLocalDepResult &NLDR : NLDI) {
          const MemDepResult &Res = NLDR.getResult();
          Instruction *target = Res.getInst();
          if (mDepInfo->find(target) == mDepInfo->end())
            (*mDepInfo)[target] = new InstrDependencies(true);
          else
            (*mDepInfo)[target]->is_invalid = true;
        }
      }
    }
}

bool AddressAccessAnalyser::isStored(llvm::Value *v) {
  for (auto user : v->users()) {
    if (auto store = dyn_cast<StoreInst>(user))
      if (store->getValueOperand() == v)
        return true;
  }
  return false;
}

std::vector<Value *> AddressAccessAnalyser::useMovesTo(
        Value *value, Instruction *instr, bool &undefined) {
  auto res = std::vector<Value *>();
  if (auto store = dyn_cast<StoreInst>(instr)) {
    Value *storedTo = store->getPointerOperand();
    if (storedTo == value)
      return res;
    if (!dyn_cast<AllocaInst>(storedTo)) {
      undefined = true;
      return res;
    }
    if (isStored(storedTo)) {
      undefined = true;
      return res;
    }
    auto deps = mDepInfo->find(store);
    if (deps == mDepInfo->end()) {
      return res;
    }
    if (deps->second->is_invalid) {
      undefined = true;
      return res;
    }
    for (auto &I : deps->second->deps)
      res.push_back(I);
    return res;
  } else if (auto load = dyn_cast<LoadInst>(instr)) {
    // res.push_back(load);
    return res;
  } else if (auto gep = dyn_cast<GetElementPtrInst>(instr)) {
    res.push_back(gep);
    return res;
  } else if (auto call = dyn_cast<CallInst>(instr)) {
    Function *called = call->getCalledFunction();
    if (called->isIntrinsic() || hasFnAttr(*called, AttrKind::LibFunc))
      return res;

    printf("%s\n", called->getName().begin());
    int argIdx = 0;
    for (Use &arg_use : call->args()) {
      Value *arg_value = arg_use.get();
      if (arg_value ==
          value) {
        if (!called->getArg(argIdx)->hasNoCaptureAttr()) {
          undefined = true;
          return res;
        }
      }
      argIdx++;
    }
    return res;
  } else {
    undefined = true;
    return res;
  }
}

bool AddressAccessAnalyser::constructUsageTree(Argument *arg) {
  auto workList = std::queue<Value *>();
  workList.push(arg);
  auto seen = std::unordered_set<Value *>();
  while (!workList.empty()) {
    Value *v = workList.front();
    workList.pop();
    // analyze users
    for (User *user : v->users()) {
      auto instr = dyn_cast<Instruction>(user);
      if (!instr) {
        LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] usage is not an instruction"
                          << "\n";);
        return false;
      }
      bool undefined = false;
      auto dsts = useMovesTo(v, instr, undefined);
      if (undefined)
        return false;
      for (Value *dst : dsts) {
        if (seen.find(dst) != seen.end()) {
          LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] met already seen instruction"
                            << "\n";);
          return false;
        }
        seen.insert(dst);
        workList.push(dst);
      }
    }
  }
  return true;
}

void AddressAccessAnalyser::runOnFunction(Function *F) {
  mParameterAccesses.addFunction(
          F);  // if argument is here => ptr held by it may be stored somewhere
  initDepInfo(F);
  int argIdx = 0;
  for (Argument *Arg = F->arg_begin(); Arg != F->arg_end(); Arg++) {
    if (Arg->getType()->isPointerTy() && !Arg->hasReturnedAttr()) {
      if (Arg->hasNoCaptureAttr() || !constructUsageTree(
              Arg)) {// if usage tree exists then we have only local usages
        Arg->addAttr(Attribute::AttrKind::NoCapture);
        mParameterAccesses.infoByFun[F]->insert(argIdx);
      }
    }
    argIdx++;
  }
}

bool AddressAccessAnalyser::runOnModule(Module &M) {
  releaseMemory();
  getAnalysis<AddressAccessAnalyserWrapper>().set(mParameterAccesses);
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  for (scc_iterator<CallGraph *> SCCI = scc_begin(
          &CG); !SCCI.isAtEnd(); ++SCCI) {
    const std::vector<CallGraphNode *> &nextSCC = *SCCI;
    if (nextSCC.size() != 1) {
      LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] met recursion, failed"
                        << "\n";);
      mParameterAccesses.failed = true;
      return false;
    }
    Function *F = nextSCC.front()->getFunction();
    if (!F) {
      continue;
    }
    if (F->isIntrinsic()) {
      mParameterAccesses.addFunction(F);
      setNocaptureToAll(F);
      continue;
    }
    if (hasFnAttr(*F, AttrKind::LibFunc)) {
      mParameterAccesses.addFunction(F);
      setNocaptureToAll(F);
      continue;
    }
    if (F->isDeclaration()) {
      continue;
    }
    runOnFunction(F);
  }
  return false;
}

bool AddressAccessAnalyser::isNonTrivialPointerType(llvm::Type *Ty) {
  assert(Ty && "Type must not be null!");
  if (Ty->isPointerTy())
    return hasUnderlyingPointer(Ty->getPointerElementType());
  if (Ty->isArrayTy())
    return hasUnderlyingPointer(Ty->getArrayElementType());
  if (Ty->isStructTy())
    for (unsigned I = 0, EI = Ty->getStructNumElements(); I < EI; ++I)
      return hasUnderlyingPointer(Ty->getStructElementType(I));
  return false;
}

void AddressAccessAnalyser::print(raw_ostream &OS, const Module *m) const {
  for (const Function &FF: *m) {
    const Function *F = &FF;
    if (F->isIntrinsic() || tsar::hasFnAttr(*F, AttrKind::LibFunc))
      continue;
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] Function [" << F->getName().begin()
                      << "]: ";);
    for (auto &arg : F->args())
      if (arg.hasNoCaptureAttr())
        LLVM_DEBUG(
                dbgs() << arg.getName().begin() << ",";);
    LLVM_DEBUG(dbgs() << "\n";);
  }
}

void AddressAccessAnalyser::setNocaptureToAll(Function* F) {
  for (auto &arg : F->args())
    if (arg.getType()->isPointerTy())
      arg.addAttr(Attribute::AttrKind::NoCapture);
}
