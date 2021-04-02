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
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include "tsar/Analysis/Memory/Utils.h"
#include <llvm/IR/InstIterator.h>
#include <vector>
#include "tsar/Support/PassAAProvider.h"
#include <algorithm>
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
#include "llvm/Analysis/MemorySSA.h"

#undef DEBUG_TYPE
#define DEBUG_TYPE "address-access"

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

char AddressAccessAnalyser::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(AddressAccessAnalyser, "address-access",
                               "address-access", false, true,
                               DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(AddressAccessAnalyser, "address-access",
                             "address-access", false, true,
                             DefaultQueryManager::PrintPassGroup::getPassRegistry())

Pass *llvm::createAddressAccessAnalyserPass() {
  return new AddressAccessAnalyser();
}

void AddressAccessAnalyser::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

bool AddressAccessAnalyser::runOnModule(Module &M) {
  releaseMemory();
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  for (scc_iterator<CallGraph *> SCCI = scc_begin(
          &CG); !SCCI.isAtEnd(); ++SCCI) {
    const std::vector<CallGraphNode *> &nextSCC = *SCCI;
    if (nextSCC.size() != 1) {
      LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] met recursion, failed"
                        << "\n";);
      return false;
    }
    Function *F = nextSCC.front()->getFunction();
    if (!F) {
      continue;
    }
    if (F->isIntrinsic()) {
      setNocaptureToAll(F);
      continue;
    }
    if (hasFnAttr(*F, AttrKind::LibFunc)) {
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

void AddressAccessAnalyser::runOnFunction(Function *F) {
  LLVM_DEBUG(errs() << "Analyzing function: "; F->getName(); errs() << "\n";);
  for (BasicBlock &BB : F->getBasicBlockList())
      for (Instruction &I: BB) {
          I.print(errs()); errs() << "\n";
      }

  for (auto &arg : F->args()) {
    if (!(arg.getType()->isPointerTy()))
      continue;
    if (isCaptured(&arg))
        LLVM_DEBUG(errs() << "Arg may be captured: "; errs() << arg.getName(); errs() << "\n";);
    else {
        LLVM_DEBUG(errs() << "Proved to be not captured: "; errs() << arg.getName(); errs() << "\n";);
        arg.addAttr(Attribute::AttrKind::NoCapture);
    }
  }
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

void AddressAccessAnalyser::setNocaptureToAll(Function *F) {
  for (auto &arg : F->args())
    if (arg.getType()->isPointerTy())
      arg.addAttr(Attribute::AttrKind::NoCapture);
}

bool AddressAccessAnalyser::isCaptured(Argument *arg) {
    if (arg->hasNoCaptureAttr())
        return false;
    std::map<Value*, int> registry;
    std::set<Instruction*> seenInstrs;
    std::queue<Value*> queue;
    registry.insert(std::pair<Value*, int>(arg, 0));
    queue.push(arg);
    while (!queue.empty()) {
        auto curValue = queue.front();
        queue.pop();
        auto [newNode, newRang, success] = applyDefinitionConstraint(curValue, registry);
        if (!success)
          return true;
        if (newNode) {
          auto [iter, success] = registry.insert(std::pair<Value*,int>(newNode, newRang));
          if (success)
            queue.push(newNode);
          if (!success && (iter->second != newRang)) {
            LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] application of the definition lead to an already seen node, stop\n";);
            return true;
          }
        }
        for (Use &use : curValue->uses()) {
            auto instr = dyn_cast<Instruction>(use.getUser());
            auto opNo = use.getOperandNo();
            if (!instr) {
                LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] usage is not an instruction"
                                  << "\n";);
                // TODO: I don't think that this branch in necessary, cause non-instr usages are BasicBlocks, Functions?
                return true;
            }
            if (seenInstrs.find(instr) != seenInstrs.end()) {
                LLVM_DEBUG(errs() << "Met seen instr: "; instr->print(errs()); errs() << "\n";);
                continue;
            } else
                seenInstrs.insert(instr);
            auto curRang = registry.find(curValue)->second;
            LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curRang: "; errs() << curRang; errs() << "\n";);
            auto [newNode, newRang, success] = applyUsageConstraint(instr, opNo, curRang);
            if (!success)
              return true;
            if (newNode) {
              auto [iter, success] = registry.insert(std::pair<Value*,int>(newNode, newRang));
              if (success)
                queue.push(newNode);
              if (!success && (iter->second != newRang)) {
                LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] application of the usage lead to an already seen node\n";);
                return true;
              }
            }
        }
    }
    return false;
}

std::tuple<Value*,int,bool> AddressAccessAnalyser::applyDefinitionConstraint(Value *curValue, std::map<Value*, int> &registry) {
  auto curRang = registry.find(curValue)->second;
  LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curRang: "; errs() << curRang; errs() << "\n";);
  if (isa<Argument>(curValue)) {
    if (registry.find(curValue)->second != 0) {
      LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue an argument of rang != 0: "; curValue->print(errs()); errs() << "\n";);
      return std::tuple<Value*,int,bool>(nullptr, -1, false);
    }
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is an argument of rang == 0: "; curValue->print(errs()); errs() << "\n";);
    return std::tuple<Value*,int,bool>(nullptr, -1, true);
  }
  else if (isa<GlobalValue>(curValue)) {
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a global value: "; curValue->print(errs()); errs() << "\n";);
    return std::tuple<Value*,int,bool>(nullptr, -1, false);
  }
  else if (isa<LoadInst>(curValue)) {
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a load: "; curValue->print(errs()); errs() << "\n";);
    auto LI = dyn_cast<LoadInst>(curValue);
    return std::tuple<Value*,int,bool>(LI->getPointerOperand(), curRang + 1, true);
  }
  else if (isa<CallBase>(curValue)) {
    // todo: replace this hardcode with an attribute of some kind
    if (dyn_cast<CallBase>(curValue)->getCalledFunction()->getName() != "malloc") {
      LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a may-alias call: "; curValue->print(errs()); errs() << "\n";);
      return std::tuple<Value*,int,bool>(nullptr, -1, false);
    }
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a no-alias call: "; curValue->print(errs()); errs() << "\n";);
    return std::tuple<Value*,int,bool>(nullptr, -1, true);
  }
  else if (isa<GetElementPtrInst>(curValue)) {
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a gep: "; curValue->print(errs()); errs() << "\n";);
    // todo: can it be a bad case here?
    return std::tuple<Value*,int,bool>(nullptr, -1, true);
  }
  else if (isa<AllocaInst>(curValue)) {
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a alloca: "; curValue->print(errs()); errs() << "\n";);
    return std::tuple<Value*,int,bool>(nullptr, -1, true);
  }
  else if (isa<BitCastInst>(curValue)) {
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a bitcast: "; curValue->print(errs()); errs() << "\n";);
    // todo: add to lists
    return std::tuple<Value*,int,bool>(nullptr, -1, true);
  }
  else {
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] curValue is a something unknown: "; curValue->print(errs()); errs() << "\n";);
    return std::tuple<Value*,int,bool>(nullptr, -1, false);
  }
}

std::tuple<Value*,int,bool> AddressAccessAnalyser::applyUsageConstraint(Instruction *instr, int opNo, int curRang) {
  switch (instr->getOpcode()) {
    case Instruction::Store: {
      if (opNo == 0) {  // ptr is being stored itself
        LLVM_DEBUG(errs() << "Met store (stored itself): "; instr->print(errs()); errs() << "\n";);
        auto *V = dyn_cast<StoreInst>(instr)->getPointerOperand();
        return std::tuple<Value*,int,bool>(V, curRang + 1, true);
      } else {
        LLVM_DEBUG(errs() << "Met store (stored TO): "; instr->print(errs()); errs() << "\n";);
        auto *V = dyn_cast<StoreInst>(instr)->getValueOperand();
        return std::tuple<Value*,int,bool>(V, curRang - 1, true);
      }
    }
    case Instruction::Load: {
      LLVM_DEBUG(errs() << "Met load (loaded from): "; instr->print(errs()); errs() << "\n";);
      return std::tuple<Value*,int,bool>(instr, curRang - 1, true);
    }
    case Instruction::GetElementPtr: {
      LLVM_DEBUG(errs() << "Met gep (geped from): "; instr->print(errs()); errs() << "\n";);
      return std::tuple<Value*,int,bool>(instr, curRang, true);
    }
    case Instruction::BitCast: {
      LLVM_DEBUG(errs() << "Met bcast (bcasted from): "; instr->print(errs()); errs() << "\n";);
      return std::tuple<Value*,int,bool>(instr, curRang, true);
    }
    case Instruction::Call: {
      LLVM_DEBUG(errs() << "Met call "; instr->print(errs()); errs() << "\n";);
      // todo: process interprocedure
      return std::tuple<Value*,int,bool>(nullptr, -1, true);
    }
    default: {
      LLVM_DEBUG(errs() << "Met unknown usage: "; instr->print(errs()); errs() << "\n";);
      return std::tuple<Value*,int,bool>(nullptr, -1, false);
    }
  }
}

void AddressAccessAnalyser::print(raw_ostream &OS, const Module *m) const {
  for (const Function &FF: *m) {
    const Function *F = &FF;
    if (F->isIntrinsic() || tsar::hasFnAttr(*F, AttrKind::LibFunc))
      continue;
    LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] Function [" << F->getName().begin()
                      << "]:\n";);
    for (auto &arg : F->args()) {
      LLVM_DEBUG(dbgs() << "\t"; dbgs() << arg.getName().begin() << ": ";);
      if (arg.hasNoCaptureAttr())
        LLVM_DEBUG(dbgs() << "NoCapture\n";);
      else
        LLVM_DEBUG(dbgs() << "May be captured\n";);
    }
  }
}