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

#include "tsar/Analysis/Attributes.h"
#include "tsar/Core/Query.h"
#include "tsar/Transform/IR/Passes.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/InitializePasses.h>
#include <llvm/Support/Debug.h>
#include <queue>

#undef DEBUG_TYPE
#define DEBUG_TYPE "nocapture"

using namespace llvm;
using namespace tsar;

namespace {
class NoCaptureAnalysis : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  NoCaptureAnalysis() : ModulePass(ID) {
    initializeNoCaptureAnalysisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<CallGraphWrapperPass>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    releaseMemory();
    CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
    for (scc_iterator<CallGraph *> SCCI = scc_begin(&CG); !SCCI.isAtEnd();
         ++SCCI) {
      const std::vector<CallGraphNode *> &NextSCC = *SCCI;
      if (NextSCC.size() != 1) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] met recursion, failed"
                          << "\n";);
        return false;
      }
      Function *F = NextSCC.front()->getFunction();
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
    for (auto &Arg : F->args()) {
      if (!(Arg.getType()->isPointerTy()))
        continue;
      if (isCaptured(&Arg)) {
        LLVM_DEBUG(dbgs() << "Arg may be captured: ";);
        LLVM_DEBUG(dbgs() << Arg.getName(); dbgs() << "\n";);
      } else {
        LLVM_DEBUG(dbgs() << "Proved to be not captured: ";
                   dbgs() << Arg.getName(); dbgs() << "\n";);
        Arg.addAttr(Attribute::AttrKind::NoCapture);
      }
    }
  }

  static void setNocaptureToAll(Function *F) {
    for (auto &Arg : F->args())
      if (Arg.getType()->isPointerTy())
        Arg.addAttr(Attribute::AttrKind::NoCapture);
  }

  static bool isCaptured(Argument *Arg) {
    if (Arg->hasNoCaptureAttr())
      return false;
    llvm::DenseMap<Value *, int> Registry;
    llvm::DenseSet<Instruction *> SeenInstrs;
    std::queue<Value *> Queue;
    Registry.insert(std::pair<Value *, int>(Arg, 0));
    Queue.push(Arg);
    while (!Queue.empty()) {
      auto CurValue = Queue.front();
      auto CurRang = Registry.find(CurValue)->second;
      Queue.pop();
      if (CurRang < 0)
        continue;
      auto [newNode, newRang, Success] =
          applyDefinitionConstraint(CurValue, CurRang);
      if (!Success)
        return true;
      if (newNode) {
        auto [iter, Success] =
            Registry.insert(std::pair<Value *, int>(newNode, newRang));
        if (Success)
          Queue.push(newNode);
        if (!Success && (iter->second != newRang)) {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE] application of the definition"
                               " lead to an already seen node, stop\n";);
          return true;
        }
      }
      for (Use &Use : CurValue->uses()) {
        auto Instr = dyn_cast<Instruction>(Use.getUser());
        auto OpNo = Use.getOperandNo();
        if (!Instr) {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE] usage is not an instruction"
                            << "\n";);
          return true;
        }
        if (SeenInstrs.find(Instr) != SeenInstrs.end()) {
          LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet seen Instr: ";
                     Instr->print(dbgs()); dbgs() << "\n";);
          continue;
        } else
          SeenInstrs.insert(Instr);
        auto [newNode, newRang, Success] =
            applyUsageConstraint(Instr, OpNo, CurRang);
        if (!Success)
          return true;
        if (newNode) {
          auto [iter, Success] =
              Registry.insert(std::pair<Value *, int>(newNode, newRang));
          if (Success)
            Queue.push(newNode);
          if (!Success && (iter->second != newRang)) {
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
  applyDefinitionConstraint(Value *CurValue, int CurRang) {
    LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurRang: "; dbgs() << CurRang;
               dbgs() << "\n";);
    if (isa<Argument>(CurValue)) {
      if (CurRang != 0) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue an argument of rang "
                             "!= 0: ";
                   CurValue->print(dbgs()); dbgs() << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, false);
      }
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is an argument of rang "
                           "== 0: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(nullptr, -1, true);
    } else if (isa<GlobalValue>(CurValue)) {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a global value: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(nullptr, -1, false);
    } else if (isa<LoadInst>(CurValue)) {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a load: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      auto LI = dyn_cast<LoadInst>(CurValue);
      return std::tuple<Value *, int, bool>(LI->getPointerOperand(),
                                            CurRang + 1, true);
    } else if (isa<CallBase>(CurValue)) {
      if (dyn_cast<CallBase>(CurValue)
              ->getCalledFunction()
              ->returnDoesNotAlias()) {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a no-alias call: ";
                   CurValue->print(dbgs()); dbgs() << "\n";);
        return std::tuple<Value *, int, bool>(nullptr, -1, true);
      }
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a may-alias call: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(nullptr, -1, false);
    } else if (isa<GetElementPtrInst>(CurValue)) {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a gep: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      auto BasePtr = dyn_cast<GetElementPtrInst>(CurValue)->getPointerOperand();
      return std::tuple<Value *, int, bool>(BasePtr, CurRang, true);
    } else if (isa<AllocaInst>(CurValue)) {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a alloca: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(nullptr, -1, true);
    } else if (isa<BitCastInst>(CurValue)) {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a bitcast: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      auto BasePtr = dyn_cast<BitCastInst>(CurValue)->getOperand(0);
      return std::tuple<Value *, int, bool>(BasePtr, CurRang, true);
    } else {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE] CurValue is a something unknown: ";
                 CurValue->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(nullptr, -1, false);
    }
  }

  static std::tuple<Value *, int, bool>
  applyUsageConstraint(Instruction *Instr, int OpNo, int CurRang) {
    switch (Instr->getOpcode()) {
    case Instruction::Store: {
      if (OpNo !=
          StoreInst::getPointerOperandIndex()) { // ptr is being stored itself
        LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet store (stored itself): ";
                   Instr->print(dbgs()); dbgs() << "\n";);
        auto *V = dyn_cast<StoreInst>(Instr)->getPointerOperand();
        return std::tuple<Value *, int, bool>(V, CurRang + 1, true);
      } else {
        LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet store (stored TO): ";
                   Instr->print(dbgs()); dbgs() << "\n";);
        auto *V = dyn_cast<StoreInst>(Instr)->getValueOperand();
        return std::tuple<Value *, int, bool>(V, CurRang - 1, true);
      }
    }
    case Instruction::Load: {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet load (loaded from): ";
                 Instr->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(Instr, CurRang - 1, true);
    }
    case Instruction::GetElementPtr: {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet gep (geped from): ";
                 Instr->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(Instr, CurRang, true);
    }
    case Instruction::BitCast: {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet bcast (bcasted from): ";
                 Instr->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(Instr, CurRang, true);
    }
    case Instruction::Call: {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet call "; Instr->print(dbgs());
                 dbgs() << "\n";);
      bool Success;
      auto CalledFun = dyn_cast<CallBase>(Instr)->getCalledFunction();
      if (!CalledFun)
        Success = false;
      else {
        auto Fun = dyn_cast<CallBase>(Instr)->getCalledFunction();
        if (Fun->isVarArg() && hasFnAttr(*Fun, AttrKind::LibFunc))
          Success = true;
        else if (Fun->isVarArg())
          Success = false;
        else
          Success = Fun->getArg(OpNo)->hasNoCaptureAttr();
      }
      return std::tuple<Value *, int, bool>(nullptr, -1, Success);
    }
    default: {
      LLVM_DEBUG(dbgs() << "[NOCAPTURE]\tMet unknown usage: ";
                 Instr->print(dbgs()); dbgs() << "\n";);
      return std::tuple<Value *, int, bool>(nullptr, -1, false);
    }
    }
  }

  void print(raw_ostream &OS, const Module *M) const override {
    for (const Function &FF : *M) {
      const Function *F = &FF;
      if (F->isIntrinsic() || tsar::hasFnAttr(*F, AttrKind::LibFunc))
        continue;
      OS << "[NOCAPTURE] [" << F->getName().begin() << "]";
      auto Separator{':'};
      for (auto &Arg : F->args()) {
        if (Arg.getType()->isPointerTy() && Arg.hasNoCaptureAttr()) {
          OS << Separator << " ";
          Separator = ',';
          printLocationSource(OS, Arg);
          OS << "(" << Arg.getArgNo() << ")";
        }
      }
      OS << "\n";
    }
  }

  void releaseMemory() override{};
};
} // namespace

char NoCaptureAnalysis::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(
    NoCaptureAnalysis, "nocapture", "nocapture", false, false,
    DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(
    NoCaptureAnalysis, "nocapture", "nocapture", false, false,
    DefaultQueryManager::PrintPassGroup::getPassRegistry())

Pass *llvm::createNoCaptureAnalysisPass() { return new NoCaptureAnalysis(); }
