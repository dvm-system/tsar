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

namespace {
    using FunctionPassesProvider =
    FunctionPassAAProvider<MemoryDependenceWrapperPass, MemorySSAWrapperPass, TargetLibraryInfoWrapperPass, AAResultsWrapperPass>;
}

INITIALIZE_PROVIDER_BEGIN(FunctionPassesProvider,
                          "function-passes-provider",
                          "MemoryDependenceWrapperPass provider")
  INITIALIZE_PASS_DEPENDENCY(MemoryDependenceWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(MemorySSAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
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

//void AddressAccessAnalyser::runOnFunction(Function *F) {
//  mParameterAccesses.addFunction(
//          F);  // if argument is here => ptr held by it may be stored somewhere
//  initDepInfo(F);
//  int argIdx = 0;
//  for (Argument *Arg = F->arg_begin(); Arg != F->arg_end(); Arg++) {
//    if (Arg->getType()->isPointerTy() && !Arg->hasReturnedAttr()) {
//      if (Arg->hasNoCaptureAttr() || !constructUsageTree(
//              Arg)) {// if usage tree exists then we have only local usages
//        Arg->addAttr(Attribute::AttrKind::NoCapture);
//        mParameterAccesses.infoByFun[F]->insert(argIdx);
//      }
//    }
//    argIdx++;
//  }
//}
//class AliasSetWithTraits;
//
//class AliasSetWithTraits : public AliasSet {
//public:
//  bool isExposed = false;
//  std::vector<bool> storesArgNo = std::vector<bool>();
//  AliasSetWithTraits* pointsTo = nullptr;
//};


/// \brief Returns true if location satisfies at least one condition:
/// 1. Location is global (TODO: or aliases some global location? think of an example);
/// 2. Location is returned from function *not* marked as noalias;
/// 3. Location is passed in some call as captured argument;
/// 4. Location retrieved as argument (TODO: or aliases some? think of an example);
bool AddressAccessAnalyser::isTriviallyExposed(Instruction &I, int OpNo) {
  auto Op = I.getOperand(OpNo);
  if (isa<GlobalVariable>(Op))
    return true;
  if (CallBase *callBase = dyn_cast<CallBase>(Op))
    return !callBase->getCalledFunction()->returnDoesNotAlias();
  if (isa<Argument>(Op))
    return true; // location retrieved as an argument is exposed
  if (CallBase *callBase = dyn_cast<CallBase>(&I)) {
    auto use = &I.getOperandUse(OpNo);
    if (callBase->isArgOperand(use)) {
      int ArgNo = callBase->getArgOperandNo(use);
      auto Arg = callBase->getCalledFunction()->getArg(ArgNo);
      return !Arg->hasAttribute(Attribute::NoCapture);
    }
  }
  return false;
}

bool AddressAccessAnalyser::aliasesExposed(Value *QueryV, std::set<Value *> &Exposed) {
  auto DL = CurFunction->getParent()->getDataLayout();
  auto QueryTy = cast<PointerType>(QueryV->getType())->getElementType();
  auto QueryML = MemoryLocation(QueryV,
                                QueryTy->isSized() ?
                                DL.getTypeStoreSize(QueryTy) : MemoryLocation::UnknownSize);
  for (auto &ExposedV : Exposed) {
    auto ExposedTy = cast<PointerType>(ExposedV->getType())->getElementType();
    auto ExposedML = MemoryLocation(ExposedV,
                                    ExposedTy->isSized() ?
                                    DL.getTypeStoreSize(ExposedTy) : MemoryLocation::UnknownSize);
    if (AA->isNoAlias(QueryML, ExposedML))
      LLVM_DEBUG(dbgs() << "noalias\n";);
    else if (AA->isMustAlias(QueryML, ExposedML))
      LLVM_DEBUG(dbgs() << "mustalias\n";);
    else
      LLVM_DEBUG(dbgs() << "mayalias\n";);
    if (QueryV == ExposedV || !AA->isNoAlias(QueryML, ExposedML))
      return true;
  }
  return false;
}

/// \brief Supplies exposed set with new references. Return true on changes. Locations to become exposed:
/// 1. locations stored to memory known to be exposed (e.g store i32** %ptr, i32*** %glob);
/// 2. locations loaded from memory known to be exposed (e.g. %1 = load i32*** %glob);
/// 3. modified locations aliasing exposed locations, so we don't need to use AA further;
bool AddressAccessAnalyser::transitExposedProperty(Instruction &I, MemoryLocation &Loc, int OpNo, AccessInfo isRead,
                                                   AccessInfo isWrite, std::set<Value *> &Exposed) {
  auto *Ptr = const_cast<Value *>(Loc.Ptr);
  bool modified = false;
  if (isWrite != AccessInfo::No && aliasesExposed(Ptr, Exposed)) {
    switch (I.getOpcode()) {
      default:
        LLVM_DEBUG(dbgs() << "Met unknown write: "; I.print(dbgs()); dbgs() << "\n"; break;);
        llvm_unreachable("Met unknown write");  // TODO: process conservatively
        break;
      case Instruction::Store: {
        auto SI = dyn_cast<StoreInst>(&I);
        auto inserted = Exposed.insert(SI->getPointerOperand());  // case 3.
        modified |= inserted.second;
        if (isa<PointerType>(SI->getValueOperand()->getType()))  // case 1.
          inserted = Exposed.insert(SI->getValueOperand());
        modified |= inserted.second;
      }
    }
  }
  if (isRead != AccessInfo::No && aliasesExposed(Ptr, Exposed)) {
    switch (I.getOpcode()) {
      default:
        LLVM_DEBUG(dbgs() << "Met unknown read: "; I.print(dbgs()); dbgs() << "\n"; break;);
        llvm_unreachable("Met unknown read");  // TODO: process conservatively
        break;
      case Instruction::Load: {
        auto LI = dyn_cast<LoadInst>(&I);
        if (isa<PointerType>(LI->getType())) {  // case 2.
          auto inserted = Exposed.insert(LI);
          modified |= inserted.second;
        }
      }
    }
  }
  return modified;
}

ExposedTracker::ExposedTracker(Function *F, TargetLibraryInfo &TLI) {
  for_each_memory(*F, TLI, [this](Instruction &I,
                                  MemoryLocation &&Loc,
                                  unsigned OpNo,
                                  AccessInfo r,
                                  AccessInfo w) {
                    addUnknown(Loc.Ptr);
                  },
                  [this](Instruction &I, AccessInfo IsRead,
                         AccessInfo isWrite) {
                      LLVM_DEBUG(errs() << "Met unknown memory: "; I.print(errs()); errs() << "\n"; return;);
                  });
}

void ExposedTracker::print() {
  LLVM_DEBUG(dbgs() << "Exposed:\n";);
  for (auto V : memLocs) {
    if (V.second != ExposedResult::Yes)
      continue;
    V.first->print(dbgs());
    LLVM_DEBUG(dbgs() << "\n";);
  }
  LLVM_DEBUG(dbgs() << "Local:\n";);
  for (auto V : memLocs) {
    if (V.second != ExposedResult::No)
      continue;
    V.first->print(dbgs());
    LLVM_DEBUG(dbgs() << "\n";);
  }
  LLVM_DEBUG(dbgs() << "Unknown:\n";);
  for (auto V : memLocs) {
    if (V.second != ExposedResult::May)
      continue;
    V.first->print(dbgs());
    LLVM_DEBUG(dbgs() << "\n";);
  }
}
/// \brief Return all values referencing memory locations which are modified by
/// this function and could be referenced from other functions
std::set<Value *> AddressAccessAnalyser::getExposedMemLocs(Function *F) {
  auto &Provider = getAnalysis<FunctionPassesProvider>(*F);
  auto &TLI = Provider.get<TargetLibraryInfoWrapperPass>().getTLI(*F);
  ExposedTracker(F, TLI).print();
  return std::set<Value *>();
}

//void AddressAccessAnalyser::runOnFunction(Function *F) {
//  auto &Provider = getAnalysis<FunctionPassesProvider>(*F);
//  AA = &Provider.get<AAResultsWrapperPass>().getAAResults();
//  CurFunction = F;
//
//  for (auto V : getExposedMemLocs(F)) {
//    V->print(dbgs());
//    LLVM_DEBUG(dbgs() << "\n";);
//  }
//  LLVM_DEBUG(dbgs() << "-----------------\n\n";);
//}


//void AddressAccessAnalyser::runOnFunction(Function *F) {
//  auto &Provider = getAnalysis<FunctionPassesProvider>(*F);
//  AA = &Provider.get<AAResultsWrapperPass>().getAAResults();
//  auto &TLI = Provider.get<TargetLibraryInfoWrapperPass>().getTLI(*F);
//  CurFunction = F;
//
//  LLVM_DEBUG(errs() << "Whole function: "; F->getName(); errs() << "\n";);
//  for (BasicBlock &BB : F->getBasicBlockList())
//    for (Instruction &I: BB) {
//      Instruction *Inst = &I;
//      LLVM_DEBUG(I.print(errs()); errs() << "\n";);
//    }
//  int i = 0;
//  LLVM_DEBUG(errs() << "\nMem locs: "; F->getName(); errs() << "\n";);
//  for_each_memory(*F, TLI, [this, &i](Instruction &I,
//                                  MemoryLocation &&Loc,
//                                  unsigned OpNo,
//                                  AccessInfo r,
//                                  AccessInfo w) {
//                      i += 1;
//                      LLVM_DEBUG(errs() << "Met known memory: "; I.print(errs()); errs() << "\n";);
////                      if (i!=10)
////                        return;
////                      auto SI = dyn_cast<StoreInst>(&I);
////                      LLVM_DEBUG(SI->getValueOperand()->print(errs()); errs() << "\n";);
////                      Argument* a_arg = nullptr;
////                      for (auto& par : I.getFunction()->args())
////                        if (par.getName() == "a") {
////                          a_arg = &par;
////                          LLVM_DEBUG(par.print(errs()); errs() << "\n";);
////                          break;
////                        }
////                      LLVM_DEBUG(a_arg->print(errs()); errs() << "\n";);
////                      std::set<Value*> set = {a_arg};
////                      if (aliasesExposed(SI->getValueOperand(), set))
////                        LLVM_DEBUG(errs() << "da \n";);
////                      else
////                        LLVM_DEBUG(errs() << "net \n";);
////                      exit(0);
//                      if (i!=8)
//                        return;
//                      auto LI = dyn_cast<LoadInst>(&I);
//                      LLVM_DEBUG(LI->getPointerOperand()->print(errs()); errs() << "\n";);
//                      std::set<Value*> set = {LI->getPointerOperand()};
//                      if (aliasesExposed(LI->getPointerOperand(), set))
//                        LLVM_DEBUG(errs() << "da \n";);
//                      else
//                        LLVM_DEBUG(errs() << "net \n";);
//                      exit(0);
//                  },
//                  [this](Instruction &I, AccessInfo IsRead,
//                         AccessInfo isWrite) {
//                      LLVM_DEBUG(errs() << "Met unknown memory: "; I.print(errs()); errs() << "\n";);
//                  });
//}

void AddressAccessAnalyser::runOnFunction(Function *F) {
  LLVM_DEBUG(errs() << "Analyzing function: "; F->getName(); errs() << "\n";);
  for (auto &arg : F->args()) {
    if (!(arg.getType()->isPointerTy()))
      continue;
    if (isCaptured(&arg) == ExposedResult::No) {
      LLVM_DEBUG(errs() << "Proved to be not captured: "; errs() << arg.getName(); errs() << "\n";);
      arg.addAttr(Attribute::AttrKind::NoCapture);
    }
    else
      LLVM_DEBUG(errs() << "Arg may be captured: "; errs() << arg.getName(); errs() << "\n";);
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
  exit(0);
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

void AddressAccessAnalyser::setNocaptureToAll(Function *F) {
  for (auto &arg : F->args())
    if (arg.getType()->isPointerTy())
      arg.addAttr(Attribute::AttrKind::NoCapture);
}

/// \brief Sets traits for given ptr-value used to store,
/// may return ptr-value "siblings" in future, thus it returns vector
std::vector<AddressAccessAnalyser::EValue> AddressAccessAnalyser::analyzeDst(Value* value, EValue parent) {
  return std::vector<AddressAccessAnalyser::EValue>();
}

/// \brief Returns vector of ptr-values, where parent may be "moved", sets traits for these values
/// unknown is set if there is no rule for given branch
std::vector<AddressAccessAnalyser::EValue> AddressAccessAnalyser::analyzeBranch(Instruction* branch, EValue parent, int opNo, bool &unknown) {
  auto result = std::vector<AddressAccessAnalyser::EValue>();
  auto ptr = branch->getOperand(opNo);
  switch (branch->getOpcode()) {
    case Instruction::Store: {
      LLVM_DEBUG(errs() << "Met store: "; branch->print(errs()); errs() << "\n";);
      if (opNo == 0) {  // ptr is being stored itself
        result = analyzeDst(branch->getOperand(opNo), parent);
      } // ptr is being stored to, not interesting in any case
      break;
    }
    case Instruction::Load: {
      LLVM_DEBUG(errs() << "Met load: "; branch->print(errs()); errs() << "\n";);
      if (parent.second.rang != 0)  // loads from values of rang 0 don't pass info about parent pointer
        result.push_back(EValue(branch, Traits(parent.second.rang - 1)));
      break;
    }
    case Instruction::GetElementPtr: {
      LLVM_DEBUG(errs() << "Met gep: "; branch->print(errs()); errs() << "\n";);
      result.push_back(EValue(branch, parent.second));
      break;
    }
    case Instruction::BitCast: {  // todo: which bitcasts are unsafe?
      LLVM_DEBUG(errs() << "Met bitcast: "; branch->print(errs()); errs() << "\n";);
      result.push_back(EValue(branch, parent.second));
      break;
    }
    case Instruction::Call: {
      LLVM_DEBUG(errs() << "Met call: "; branch->print(errs()); errs() << "\n";);
      unknown = !(dyn_cast<CallBase>(branch)->getCalledFunction()->getArg(opNo)->hasNoCaptureAttr());
      break;
    }
    default: {  // todo: process returns somehow
      LLVM_DEBUG(errs() << "Met unknown usage: "; branch->print(errs()); errs() << "\n";);
      unknown = true;
      break;
    }
  }
  return result;
}

ExposedResult AddressAccessAnalyser::isCaptured(Argument *arg) {
  if (arg->hasNoCaptureAttr())
    return ExposedResult::No;
  std::set<Value*> seen;
  std::vector<EValue> workList = {EValue(arg, Traits(0, false))};
  while (!workList.empty()) {
    auto&[value, v_traits] = workList.back();
    workList.pop_back();
    for (Use &use : value->uses()) {
      auto instr = dyn_cast<Instruction>(use.getUser());
      if (!instr) {
        LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] usage is not an instruction"
                          << "\n";);
        // TODO: I don't think that this branch in necessary, cause non-instr usages are BasicBlocks, Functions?
        return ExposedResult::May;
      }
      bool unknown = false;
      auto result = analyzeBranch(instr, EValue(value, v_traits), use.getOperandNo(), unknown);
      if (unknown)
        return ExposedResult::May;
      for (auto &child : result) {
        if (child.second.exposed)
          return ExposedResult::Yes;
        bool inserted;
        std::tie(std::ignore, inserted) = seen.insert(child.first);
        if (!inserted) {
          LLVM_DEBUG(dbgs() << "[ADDRESS-ACCESS] value was already seen"
                            << "\n";);
          // TODO: I need to think this through, cause now i only see a few cases when this is possible, namely:
          // TODO: 1) when we store value to one place more than once, e.g.:
          // TODO:     - %ptr = alloca; store %v, %ptr; store %ptr, %ptr2; %alias = load %ptr2; store %v, %alias;
          // TODO: And this case is ok, but i don't see the big picture now, and what consequences of ignoring this are possible
          return ExposedResult::May;
        }
      }
    }
  }
  return ExposedResult::No;
}
