//===--- NoCaptureAnalysis.cpp - Parameter capture analysis -----*- C++ -*-===//
//
//             Traits Static Analyzer (SAPFOR)
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
// This file defines passes to determine formal arguments which can be
// preserved by the function (e.g. saved to the global memory).
//
//===----------------------------------------------------------------------===//

#include <queue>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Debug.h>
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/InitializePasses.h"
#include "llvm/Analysis/MemorySSA.h"

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassProvider.h"

#undef DEBUG_TYPE
#define DEBUG_TYPE "nocapture"

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

namespace {
  class NoCaptureAnalysis :
    public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;

    NoCaptureAnalysis() : ModulePass(ID) {
      initializeNoCaptureAnalysisPass(*PassRegistry::getPassRegistry());
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<CallGraphWrapperPass>();
      AU.addRequired<GlobalOptionsImmutableWrapper>();
      AU.setPreservesAll();
    }

    bool runOnModule(Module &M) override {
      releaseMemory();
      CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
      for (scc_iterator<CallGraph *> SCCI = scc_begin(
        &CG); !SCCI.isAtEnd(); ++SCCI) {
        const std::vector<CallGraphNode *> &nextSCC = *SCCI;
        if (nextSCC.size() != 1) {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE] met recursion, failed"
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

    static void runOnFunction(Function *F) {
      LLVM_DEBUG(dbgs() << "Analyzing function: ";);
      LLVM_DEBUG(dbgs() << F->getName(); dbgs() << "\n";);
//  for (BasicBlock &BB : F->getBasicBlockList())
//    for (Instruction &I: BB) {
//      I.print(dbgs()); dbgs() << "\n";
//    }

      for (auto &arg : F->args()) {
        if (!(arg.getType()->isPointerTy()))
          continue;
        if (isCaptured(&arg)) {
          LLVM_DEBUG(dbgs() << "Arg may be captured: ";);
          LLVM_DEBUG(dbgs() << arg.getName(); dbgs() << "\n";);
        } else {
          LLVM_DEBUG(
            dbgs() << "Proved to be not captured: "; dbgs() << arg.getName();
            dbgs() << "\n";);
          arg.addAttr(Attribute::AttrKind::NoCapture);
        }
      }
    }

    static bool isNonTrivialPointerType(Type *Ty) {
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

    static void setNocaptureToAll(Function *F) {
      for (auto &arg : F->args())
        if (arg.getType()->isPointerTy())
          arg.addAttr(Attribute::AttrKind::NoCapture);
    }

    static bool isCaptured(Argument *arg) {
      if (arg->hasNoCaptureAttr())
        return false;
      std::map<Value *, int> registry;
      std::set<Instruction *> seenInstrs;
      std::queue<Value *> queue;
      registry.insert(std::pair<Value *, int>(arg, 0));
      queue.push(arg);
      while (!queue.empty()) {
        auto curValue = queue.front();
        auto curRang = registry.find(curValue)->second;
        queue.pop();
        if (curRang < 0)
          continue;
        auto[newNode, newRang, success] = applyDefinitionConstraint(curValue,
                                                                    registry);
        if (!success)
          return true;
        if (newNode) {
          auto[iter, success] = registry.insert(
            std::pair<Value *, int>(newNode, newRang));
          if (success)
            queue.push(newNode);
          if (!success && (iter->second != newRang)) {
            LLVM_DEBUG(
              dbgs() << "[NOCAPTURE] application of the definition"
                        " lead to an already seen node, stop\n";);
            return true;
          }
        }
        for (Use &use : curValue->uses()) {
          auto instr = dyn_cast<Instruction>(use.getUser());
          auto opNo = use.getOperandNo();
          if (!instr) {
            LLVM_DEBUG(dbgs() << "[NOCAPTURE] usage is not an instruction"
                              << "\n";);
            return true;
          }
          if (seenInstrs.find(instr) != seenInstrs.end()) {
            LLVM_DEBUG(
              dbgs() << "[NOCAPTURE]\tMet seen instr: "; instr->print(
              dbgs()); dbgs() << "\n";);
            continue;
          } else
            seenInstrs.insert(instr);
          auto[newNode, newRang, success] = applyUsageConstraint(instr, opNo,
                                                                 curRang);
          if (!success)
            return true;
          if (newNode) {
            auto[iter, success] = registry.insert(
              std::pair<Value *, int>(newNode, newRang));
            if (success)
              queue.push(newNode);
            if (!success && (iter->second != newRang)) {
              LLVM_DEBUG(dbgs() << "[NOCAPTURE] application of the usage "
                                   "lead to an already seen node\n";);
              return true;
            }
          }
        }
      }
      return false;
    }

    static std::tuple<Value *, int, bool>
    applyDefinitionConstraint(
      Value *curValue,
      std::map<Value *, int> &registry
    ) {
      auto curRang = registry.find(curValue)->second;
      LLVM_DEBUG(
        dbgs() << "[NOCAPTURE] curRang: "; dbgs() << curRang; dbgs()
        << "\n";);
      if (isa<Argument>(curValue)) {
        if (registry.find(curValue)->second != 0) {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue an argument of rang "
                               "!= 0: "; curValue->print(dbgs()); dbgs()
            << "\n";);
          return std::tuple<Value *, int, bool>(nullptr, -1, false);
        }
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is an argument of rang "
                             "== 0: "; curValue->print(dbgs()); dbgs()
          << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, true);
      } else if (isa<GlobalValue>(curValue)) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a global value: ";
                     curValue->print(dbgs()); dbgs() << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, false);
      } else if (isa<LoadInst>(curValue)) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a load: ";
                     curValue->print(dbgs()); dbgs() << "\n";);
        auto LI = dyn_cast<LoadInst>(curValue);
        return std::tuple<Value *, int, bool>(LI->getPointerOperand(),
                                              curRang + 1,
                                              true);
      } else if (isa<CallBase>(curValue)) {
        if (dyn_cast<CallBase>(
          curValue)->getCalledFunction()->returnDoesNotAlias()) {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a no-alias call: ";
                       curValue->print(dbgs()); dbgs() << "\n";);
          return std::tuple<Value *, int, bool>(nullptr, -1, true);
        }
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a may-alias call: ";
                     curValue->print(dbgs()); dbgs() << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, false);
      } else if (isa<GetElementPtrInst>(curValue)) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a gep: ";
                     curValue->print(dbgs()); dbgs() << "\n";);
        auto basePtr = dyn_cast<GetElementPtrInst>(
          curValue)->getPointerOperand();
        return std::tuple<Value *, int, bool>(basePtr, curRang, true);
      } else if (isa<AllocaInst>(curValue)) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a alloca: ";
                     curValue->print(dbgs()); dbgs() << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, true);
      } else if (isa<BitCastInst>(curValue)) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] curValue is a bitcast: ";
                     curValue->print(dbgs()); dbgs() << "\n";);
        auto basePtr = dyn_cast<BitCastInst>(curValue)->getOperand(0);
        return std::tuple<Value *, int, bool>(basePtr, curRang, true);
      } else {
        LLVM_DEBUG(
          dbgs() << "[NOCAPTURE] curValue is a something unknown: ";
          curValue->print(dbgs()); dbgs() << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, false);
      }
    }

    static std::tuple<Value *, int, bool>
    applyUsageConstraint(
      Instruction *instr,
      int opNo,
      int curRang
    ) {
      switch (instr->getOpcode()) {
        case Instruction::Store: {
          if (opNo !=
              StoreInst::getPointerOperandIndex()) {  // ptr is being stored itself
            LLVM_DEBUG(
              dbgs() << "[NOCAPTURE]\tMet store (stored itself): ";
              instr->print(dbgs()); dbgs() << "\n";);
            auto *V = dyn_cast<StoreInst>(instr)->getPointerOperand();
            return std::tuple<Value *, int, bool>(V, curRang + 1, true);
          } else {
            LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet store (stored TO): ";
                         instr->print(dbgs()); dbgs() << "\n";);
            auto *V = dyn_cast<StoreInst>(instr)->getValueOperand();
            return std::tuple<Value *, int, bool>(V, curRang - 1, true);
          }
        }
        case Instruction::Load: {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet load (loaded from): ";
                       instr->print(dbgs()); dbgs() << "\n";);
          return std::tuple<Value *, int, bool>(instr, curRang - 1, true);
        }
        case Instruction::GetElementPtr: {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet gep (geped from): ";
                       instr->print(dbgs()); dbgs() << "\n";);
          return std::tuple<Value *, int, bool>(instr, curRang, true);
        }
        case Instruction::BitCast: {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet bcast (bcasted from): ";
                       instr->print(dbgs()); dbgs() << "\n";);
          return std::tuple<Value *, int, bool>(instr, curRang, true);
        }
        case Instruction::Call: {
          LLVM_DEBUG(
            dbgs() << "[NOCAPTURE]\tMet call "; instr->print(dbgs());
            dbgs() << "\n";);
          bool success;
          auto calledFun = dyn_cast<CallBase>(instr)->getCalledFunction();
          if (!calledFun)
            success = false;
          else {
            auto fun = dyn_cast<CallBase>(instr)->getCalledFunction();
            if (fun->isVarArg() && hasFnAttr(*fun, AttrKind::LibFunc))
              success = true;
            else if (fun->isVarArg())
              success = false;
            else
              success = fun->getArg(opNo)->hasNoCaptureAttr();
          }
          return std::tuple<Value *, int, bool>(nullptr, -1, success);
        }
        default: {
          LLVM_DEBUG(
            dbgs() << "[NOCAPTURE]\tMet unknown usage: "; instr->print(
            dbgs()); dbgs() << "\n";);
          return std::tuple<Value *, int, bool>(nullptr, -1, false);
        }
      }
    }

    void print(raw_ostream &OS, const Module *M) const override {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] Printing pointer, nocapture args"
                           "for all analyzed functions\n";);
      for (const Function &FF: *M) {
        const Function *F = &FF;
        if (F->isIntrinsic() || tsar::hasFnAttr(*F, AttrKind::LibFunc))
          continue;
        LLVM_DEBUG(
          dbgs() << "[NOCAPTURE] [" << F->getName().begin() << "]: ";);
        for (auto &arg : F->args()) {
          if (arg.getType()->isPointerTy() && arg.hasNoCaptureAttr())
            LLVM_DEBUG(dbgs() << arg.getName().begin() << ", ";);
        }
        LLVM_DEBUG(dbgs() << "\n";);
      }
    }

    void releaseMemory() override {};
  };
}

char NoCaptureAnalysis::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(
  NoCaptureAnalysis, "nocapture",
  "nocapture", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(
  NoCaptureAnalysis, "nocapture",
  "nocapture", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

Pass *llvm::createNoCaptureAnalysisPass() {
  return new NoCaptureAnalysis();
}
