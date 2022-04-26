//===- InterprocAttr.cpp - Interprocedural Attributes Deduction -*- C++ -*-===//
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
// This file defines passes which perform interprocedural analysis in order to
// deduce some function and loop attributes.
//
//===----------------------------------------------------------------------===//
#include "tsar/Transform/IR/InterprocAttr.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/IRUtils.h"
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/InitializePasses.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Operator.h>
#include <vector>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "functionattrs"

STATISTIC(NumLibFunc, "Number of functions marked as sapfor.libfunc");
STATISTIC(NumNoIOFunc, "Number of functions marked as sapfor.noio");
STATISTIC(NumAlwaysRetFunc, "Number of functions marked as sapfor.alwaysreturn");
STATISTIC(NumDirectUserCalleFunc, "Number of funstions marked as sapfor.direct-user-callee");
STATISTIC(NumArgMemOnlyFunc, "Number of functions marked as argmemonly");
STATISTIC(NumNoCaptureArg, "Number of arguments marked as nocaputre");
STATISTIC(NumNoIOLoop, "Number of loops marked as sapfor.noio");
STATISTIC(NumAlwaysRetLoop, "Number of loops marked as sapfor.alwaysreturn");
STATISTIC(NumNoUnwindLoop, "Number of loops marked as nounwind");
STATISTIC(NumReturnsTwiceLoop, "Number of loops marked as returns_twice");

namespace {
/// This pass walks SCCs of the call graph in RPO to deduce and propagate
/// function attributes.
///
/// Currently it only handles synthesizing sapfor.libfunc attributes.
/// We consider a function as a library function if it is called from some
/// known by llvm::TargetLibraryInfo library function.
struct RPOFunctionAttrsAnalysis : public ModulePass, private bcl::Uncopyable {
  static char ID;
  RPOFunctionAttrsAnalysis() : ModulePass(ID) {
    initializeRPOFunctionAttrsAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<CallGraphWrapperPass>();
    AU.addPreserved<CallGraphWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
  }
};

/// This pass walks SCCs of the call graph in PO to deduce and propagate
/// function attributes.
class POFunctionAttrsAnalysis :
  public CallGraphSCCPass, private bcl::Uncopyable {
public:
  static char ID;
  POFunctionAttrsAnalysis() : CallGraphSCCPass(ID) {
    initializePOFunctionAttrsAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSCC(CallGraphSCC &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    CallGraphSCCPass::getAnalysisUsage(AU);
  }

private:
  /// Checks is it necessary to add an attribute to the whole functions from
  /// currently processed SCC and returns this attribute, otherwise return
  /// `not_attribute`.
  AttrKind addNoIOAttr();

  /// Checks is it necessary to add an attribute to the whole functions from
  /// currently processed SCC and returns this attribute, otherwise return
  /// `not_attribute`.
  AttrKind addAlwaysReturnAttr();

  /// Checks is it necessary to add an attribute to the whole functions from
  /// currently processed SCC and returns this attribute, otherwise return
  /// `not_attribute`.
  AttrKind addDirectUserCalleeAttr();

  SmallPtrSet<Function *, 4> mSCCFuncs;
  SmallPtrSet<Function *, 16> mCalleeFuncs;
};

bool addLibFuncAttrsTopDown(Function &F) {
  for (auto *U : F.users()) {
    if (auto *Call = dyn_cast<CallBase>(U))
      if (hasFnAttr(*Call->getParent()->getParent(), AttrKind::LibFunc)) {
        addFnAttr(F, AttrKind::LibFunc);
        ++NumLibFunc;
        return true;
      }
  }
  return true;
}
}

char RPOFunctionAttrsAnalysis::ID = 0;

INITIALIZE_PASS_BEGIN(RPOFunctionAttrsAnalysis, "rpo-sapfor-functionattrs",
  "Deduce function attributes in RPO", false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(RPOFunctionAttrsAnalysis, "rpo-sapfor-functionattrs",
  "Deduce function attributes in RPO", false, false)

ModulePass * llvm::createRPOFunctionAttrsAnalysis() {
  return new RPOFunctionAttrsAnalysis();
}

bool RPOFunctionAttrsAnalysis::runOnModule(llvm::Module &M) {
  for (auto &F : M)
    // Check if all calls of the function are direct.
    for (auto *U : F.users()) {
      SmallVector<User *, 4> Users;
      SmallPtrSet<User *, 4> Visited{U};
      SmallVector<User *, 4> Worklist{U};
      do {
        auto *U{Worklist.pop_back_val()};
        if (Operator::getOpcode(U) == Instruction::BitCast) {
          for (auto *U1 : U->users())
            if (Visited.insert(U1).second)
              Worklist.push_back(U1);
        } else {
          Users.push_back(U);
        }
      } while (!Worklist.empty());
      if (any_of(Users, [](auto *U) {
            return !isa<CallBase>(U) &&
                   (!isa<BitCastOperator>(U) || any_of(U->users(), [](auto *U) {
                     return !isa<CallBase>(U);
                   }));
          }))
        addFnAttr(F, AttrKind::IndirectCall);
    }
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::vector<Function *> Worklist;
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    auto CGNIdx = Worklist.size();
    bool HasLibFunc = false;
    for (auto *CGN : *I)
      if (auto F = CGN->getFunction()) {
        if (F->isIntrinsic())
          continue;
        auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);
        Worklist.push_back(F);
        LibFunc LibId;
        HasLibFunc |= TLI.getLibFunc(*F, LibId);
      }
    if (HasLibFunc)
      for (std::size_t EIdx = Worklist.size(); CGNIdx < EIdx; ++CGNIdx)
        addFnAttr(*Worklist[CGNIdx], AttrKind::LibFunc);
  }
  bool Changed = false;
  for (auto *F : llvm::reverse(Worklist))
    Changed |= addLibFuncAttrsTopDown(*F);
  return Changed;
}

char POFunctionAttrsAnalysis::ID = 0;

INITIALIZE_PASS_BEGIN(POFunctionAttrsAnalysis, "sapfor-functionattrs",
  "Deduce function attributes", false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(POFunctionAttrsAnalysis, "sapfor-functionattrs",
  "Deduce function attributes", false, false)

Pass * llvm::createPOFunctionAttrsAnalysis() {
  return new POFunctionAttrsAnalysis();
}

bool POFunctionAttrsAnalysis::runOnSCC(CallGraphSCC &SCC) {
  // DbgInfoIntrinsics are excluded from CallGraph, so traverse them manually.
  for (auto &F : SCC.getCallGraph().getModule())
    if (isDbgInfoIntrinsic(F.getIntrinsicID())) {
      addFnAttr(F, AttrKind::AlwaysReturn);
      addFnAttr(F, AttrKind::NoIO);
      addFnAttr(F, AttrKind::DirectUserCallee);
    }
  mSCCFuncs.clear();
  mCalleeFuncs.clear();
  for (auto *CGN : SCC)
    if (auto F = CGN->getFunction())
      mSCCFuncs.insert(F);
    else
      return false;
  for (auto *CGN : SCC) {
    auto F = CGN->getFunction();
    for (auto &CFGTo : *CGN)
      if (auto FTo = CFGTo.second->getFunction()) {
        if (!mSCCFuncs.count(FTo))
          mCalleeFuncs.insert(FTo);
      } else {
        return false;
      }
  }
  SmallVector<AttrKind, 4> AddAttrs;;
  auto Kind = addNoIOAttr();
  if (Kind != AttrKind::not_attribute)
    AddAttrs.push_back(Kind);
  Kind = addAlwaysReturnAttr();
  if (Kind != AttrKind::not_attribute)
    AddAttrs.push_back(Kind);
  Kind = addDirectUserCalleeAttr();
  if (Kind != AttrKind::not_attribute)
    AddAttrs.push_back(Kind);
  for (auto *SCCF : mSCCFuncs)
    for (auto Attr : AddAttrs)
      addFnAttr(*SCCF, Attr);
  return !AddAttrs.empty();
}

AttrKind POFunctionAttrsAnalysis::addNoIOAttr() {
  for (auto *SCCF : mSCCFuncs) {
    if (SCCF->isIntrinsic())
      continue;
    LibFunc LibId;
    // We can not use here 'sapfor.libfunc' attribute to check whether a
    // function is a library function because set of in/out functions contains
    // functions which treated as library by TargetLibraryInfo only. So, the
    // mentioned set may not contain some 'safpor.libfunc' functions which are
    // in/out functions.
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*SCCF);
    bool IsLibFunc = TLI.getLibFunc(*SCCF, LibId);
    if ((IsLibFunc && isIOLibFuncName(SCCF->getName())) ||
        (!IsLibFunc && SCCF->isDeclaration()))
      return AttrKind::not_attribute;
  }
  for (auto *CF : mCalleeFuncs)
    if (!hasFnAttr(*CF, AttrKind::NoIO))
      return AttrKind::not_attribute;
  NumNoIOFunc += mSCCFuncs.size();
  return AttrKind::NoIO;
}

AttrKind POFunctionAttrsAnalysis::addDirectUserCalleeAttr() {
  for (auto *SCCF : mSCCFuncs) {
    if (SCCF->isIntrinsic())
      continue;
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*SCCF);
    LibFunc LibId;
    if (TLI.getLibFunc(*SCCF, LibId))
      continue;
  }
  for (auto *CF : mCalleeFuncs)
    if (!hasFnAttr(*CF, AttrKind::DirectUserCallee))
      return AttrKind::not_attribute;
  NumDirectUserCalleFunc += mSCCFuncs.size();
  return AttrKind::DirectUserCallee;
}

AttrKind POFunctionAttrsAnalysis::addAlwaysReturnAttr() {
  for (auto *SCCF : mSCCFuncs) {
    if (!SCCF->hasFnAttribute(Attribute::WillReturn) &&
        (SCCF->hasFnAttribute(Attribute::NoReturn) ||
         (SCCF->isDeclaration() && !SCCF->isIntrinsic() &&
          !hasFnAttr(*SCCF, AttrKind::LibFunc))))
      return AttrKind::not_attribute;
  }
  for (auto *CF : mCalleeFuncs)
    if (!hasFnAttr(*CF, AttrKind::AlwaysReturn))
      return AttrKind::not_attribute;
  NumAlwaysRetFunc += mSCCFuncs.size();
  return AttrKind::AlwaysReturn;
}

char LoopAttributesDeductionPass::ID = 0;

INITIALIZE_PASS_BEGIN(LoopAttributesDeductionPass, "loopattrs",
  "Deduce function attributes in RPO", false, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(LoopAttributesDeductionPass, "loopattrss",
  "Deduce function attributes in RPO", false, true)

FunctionPass * llvm::createLoopAttributesDeductionPass() {
  return new LoopAttributesDeductionPass();
}

void LoopAttributesDeductionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

bool LoopAttributesDeductionPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  for_each_loop(LI, [this](const Loop *L) {
    bool MayNoReturn = false, MayIO = false;
    bool MayUnwind = false, MayReturnsTwice = false;
    for (auto *BB : L->blocks())
      for (auto &I : *BB) {
        auto *Call = dyn_cast<CallBase>(&I);
        if (!Call)
          continue;
        auto Callee = dyn_cast<Function>(
          Call->getCalledOperand()->stripPointerCasts());
        if (!Callee) {
          MayIO = MayNoReturn = MayUnwind = MayReturnsTwice = true;
          break;
        }
        MayIO |= !hasFnAttr(*Callee, AttrKind::NoIO);
        MayNoReturn |= !hasFnAttr(*Callee, AttrKind::AlwaysReturn);
        MayUnwind |= !Callee->hasFnAttribute(Attribute::NoUnwind);
        MayReturnsTwice |= Callee->hasFnAttribute(Attribute::ReturnsTwice);
      }
    auto Itr = mAttrs.end();
    if (!MayIO) {
      Itr = mAttrs.try_emplace(L).first;
      Itr->second.get<AttrKind>().insert(AttrKind::NoIO);
      ++NumNoIOLoop;
    }
    if (!MayNoReturn) {
      if (Itr == mAttrs.end())
        Itr = mAttrs.try_emplace(L).first;
      Itr->second.get<AttrKind>().insert(AttrKind::AlwaysReturn);
      ++NumAlwaysRetLoop;
    }
    if (!MayUnwind) {
      if (Itr == mAttrs.end())
        Itr = mAttrs.try_emplace(L).first;
      Itr->second.get<Attribute::AttrKind>().insert(Attribute::NoUnwind);
      ++NumNoUnwindLoop;
    }
    if (MayReturnsTwice) {
      if (Itr == mAttrs.end())
        Itr = mAttrs.try_emplace(L).first;
      Itr->second.get<Attribute::AttrKind>().insert(Attribute::ReturnsTwice);
      ++NumReturnsTwiceLoop;
    }
  });
  return false;
}

namespace {
class FunctionMemoryAttrsAnalysis : public FunctionPass, bcl::Uncopyable {
public:
  static char ID;
  FunctionMemoryAttrsAnalysis() : FunctionPass(ID) {
    initializeFunctionMemoryAttrsAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    auto &DefInfo = getAnalysis<DefinedMemoryPass>().getDefInfo();
    auto &RegInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
    auto DefItr = DefInfo.find(RegInfo.getTopLevelRegion());
    assert(DefItr != DefInfo.end() &&
           "Defined memory analysis must be available for function!");
    /// TODO (kaniandr@gmail.com): deduce other useful attributes, such as,
    /// readonly, writeonly, readnone and speculatable.
    if (isPure(F, *DefItr->get<DefUseSet>()).first) {
      F.setOnlyAccessesArgMemory();
      /// TODO (kaniandr@gmail.com): traverse call graph to increase
      /// quality of deduction of 'nocaputre' attributes.
      for (auto &Arg : F.args())
        if (isa<PointerType>(Arg.getType())) {
          Arg.addAttr(Attribute::NoCapture);
          ++NumNoCaptureArg;
        }
      ++NumArgMemOnlyFunc;
      return true;
    }
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<DefinedMemoryPass>();
    AU.addRequired<DFRegionInfoPass>();
  }
};
}

FunctionPass *llvm::createFunctionMemoryAttrsAnalysis() {
  return new FunctionMemoryAttrsAnalysis;
}

char FunctionMemoryAttrsAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(FunctionMemoryAttrsAnalysis, "memory-functionattr",
  "Deduce Function Memory Attributes", true, false)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_END(FunctionMemoryAttrsAnalysis, "memory-functionattr",
  "Deduce Function Memory Attributes", true, false)
