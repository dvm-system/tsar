//===---- FunctionInfo.cpp -- APC Function Collector ------------*- C++ -*-===//
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
// This file collects general information about functions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include <apc/GraphCall/graph_calls.h>
#include <bcl/utility.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-function-info"

using namespace llvm;
using namespace tsar;

namespace {
class APCFunctionInfoPassInfo final : public PassGroupInfo {
  bool isNecessaryPass(llvm::AnalysisID ID) const override {
    static llvm::AnalysisID Passes[] = {
      getPassIDAndErase(createAPCLoopInfoBasePass()),
      getPassIDAndErase(createAPCArrayInfoPass()),
    };
    return count(Passes, ID);
  }
};

class APCFunctionInfoPass: public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  APCFunctionInfoPass() : ModulePass(ID) {
    initializeAPCFunctionInfoPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void releaseMemory() override { mFunctions.clear(); }
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;

private:
  std::vector<apc::FuncInfo *> mFunctions;
  bool mMultipleLaunch = false;
};

/// Try to determine start and end source location for a specified function.
///
/// This function traverse all instructions in a specified function to obtain
/// instructions with the lowest and the largest location.
/// TODO (kaniandr@gmail.com): use Clang to add function bounds into debug
/// information.
/// TODO (kaniandr@gmail.com): what should we do if start and end are locations
/// in different files in case of #include.
std::pair<DebugLoc, DebugLoc> getFunctionRange(const Function &F) {
  DebugLoc StartLoc, EndLoc;
  auto isLess = [](DebugLoc LHS, DebugLoc RHS) {
    assert(LHS && RHS && "Source locations must not be null!");
    return LHS.getLine() < RHS.getLine() ||
      LHS.getLine() == RHS.getLine() && LHS.getCol() < RHS.getCol();
  };
  for (auto &I : instructions(F)) {
    auto Loc = I.getDebugLoc();
    if (!Loc)
      continue;
    if (!StartLoc || isLess(Loc, StartLoc))
      StartLoc = Loc;
    if (!EndLoc || isLess(EndLoc, Loc))
      EndLoc = Loc;
  }
  return std::make_pair(StartLoc, EndLoc);
}

/// Add new function to the APCContext.
std::pair<apc::FuncInfo *, bool> registerFunction(Function &F, APCContext &APCCtx) {
  auto Range = getFunctionRange(F);
  decltype(std::declval<apc::FuncInfo>().linesNum) Lines(0, 0);
  if (Range.first && !bcl::shrinkPair(
        Range.first.getLine(), Range.first.getCol(), Lines.first))
    emitUnableShrink(F.getContext(), F, Range.first, DS_Warning);
  if (Range.second && !bcl::shrinkPair(
        Range.second.getLine(), Range.second.getCol(), Lines.second))
    emitUnableShrink(F.getContext(), F, Range.second, DS_Warning);
  auto FI = new apc::FuncInfo(F.getName().str(), Lines);
  if (!APCCtx.addFunction(F, FI)) {
    delete FI;
    return std::make_pair(APCCtx.findFunction(F), false);
  }
  return std::make_pair(FI, true);
}
}

char APCFunctionInfoPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(APCFunctionInfoPass, "apc-function-info",
  "Function Collector (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_IN_GROUP_INFO(APCFunctionInfoPassInfo);
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(APCFunctionInfoPass, "apc-function-info",
  "Function Collector (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

ModulePass * llvm::createAPCFunctionInfoPass() {
  return new APCFunctionInfoPass;
}

void APCFunctionInfoPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<APCContextWrapper>();
  AU.setPreservesAll();
}

bool APCFunctionInfoPass::runOnModule(Module &M) {
  releaseMemory();
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  for (auto &Caller : CG) {
    /// TODO (kaniandr@gmail.com): should we collect functions without body?
    if (!Caller.first || Caller.first->isDeclaration())
      continue;
    auto Pair = registerFunction(const_cast<Function &>(*Caller.first), APCCtx);
    mFunctions.push_back(Pair.first);
    if (!Pair.second) {
      // This pass may be executed in analysis mode. It depends on -print-only
      // and -print-step options. In case of parallelization pass manager must
      // invokes this pass only once for each function.
      mMultipleLaunch = true;
      continue;
    }
    auto &FI = *Pair.first;
    auto DIFunc = Caller.first->getSubprogram();
    /// TODO (kaniandr@gmail.com): should we emit warning or error if filename
    /// is not available from debug information.
    FI.fileName = DIFunc && !DIFunc->getFilename().empty() ?
      DIFunc->getFilename().str() : M.getSourceFileName();
    // TODO (kaniandr@gmail.com): allow to use command line option to determine
    // entry point.
    if (auto DWLang = getLanguage(*Caller.first))
      FI.isMain = isC(*DWLang) && FI.funcName == "main";
    auto &LI = getAnalysis<LoopInfoWrapperPass>(
      const_cast<Function &>(*Caller.first)).getLoopInfo();
    for (auto *L : LI) {
      assert(L->getLoopID() && "Loop ID must not be null!");
      auto *APCLoop = APCCtx.findLoop(L->getLoopID());
      assert(APCLoop && "List of APC loops must be filled!");
      FI.loopsInFunc.push_back(APCLoop);
    }
    auto &DT = getAnalysis<DominatorTreeWrapperPass>(
      const_cast<Function &>(*Caller.first)).getDomTree();
    for (auto &Arg : Caller.first->args()) {
      if (Arg.hasStructRetAttr())
        continue;
      SmallVector<DIMemoryLocation, 1> DILocs;
      auto DIM = findMetadata(&Arg, DILocs);
      if (!DIM && Arg.getNumUses() == 1)
        if (auto SI = dyn_cast<StoreInst>(*Arg.user_begin()))
          if (auto *AI = dyn_cast<AllocaInst>(SI->getPointerOperand()))
            DIM = findMetadata(AI, DILocs);
      if (!DIM || !DIM->isValid() ||
          !cast<DILocalVariable>(DIM->Var)->isParameter()) {
        FI.funcParams.identificators.push_back(Arg.getName().str());
        FI.funcParams.parameters.push_back(nullptr);
        FI.funcParams.parametersT.push_back(UNKNOWN_T);
        FI.funcParams.inout_types.push_back(IN_BIT | OUT_BIT);
      } else {
        FI.funcParams.identificators.push_back(DIM->Var->getName().str());
        FI.funcParams.parameters.push_back(nullptr);
        auto RawDIM = getRawDIMemoryIfExists(M.getContext(), *DIM);
        if (RawDIM && APCCtx.findArray(RawDIM))
          FI.funcParams.parametersT.push_back(ARRAY_T);
        else
          FI.funcParams.parametersT.push_back(UNKNOWN_T);
        FI.funcParams.inout_types.push_back(IN_BIT | OUT_BIT);
      }
    }
    FI.funcParams.countOfPars = FI.funcParams.identificators.size();
    for (auto &Callee : *Caller.second) {
      if (!Callee.first)
        continue;
      if (auto II = dyn_cast<IntrinsicInst>(*Callee.first))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      decltype(FI.linesOfIO)::value_type ShrinkCallLoc = 0;
      if (auto &Loc = cast<Instruction>(*Callee.first)->getDebugLoc())
        if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), ShrinkCallLoc))
          emitUnableShrink(M.getContext(), *Caller.first, Loc, DS_Warning);
      SmallString<16> CalleeName;
      // TODO (kaniandr@gmail.com): should we add names for indirect calls,
      // for example if callee is a variable which is a pointer to a function?
      // Should we emit diagnostic if name is unknown?
      if (unparseCallee(*cast<CallBase>(*Callee.first), M, DT, CalleeName))
        FI.callsFrom.insert(CalleeName.str().str());
      auto CalleeFunc = Callee.second->getFunction();
      if (!CalleeFunc || !hasFnAttr(*CalleeFunc, AttrKind::NoIO)) {
        if (ShrinkCallLoc != 0 ||
            !std::count(FI.linesOfIO.begin(), FI.linesOfIO.end(), 0))
          FI.linesOfIO.push_back(ShrinkCallLoc);
      }
      if (!CalleeFunc ||
          !hasFnAttr(*CalleeFunc, AttrKind::AlwaysReturn) ||
          !CalleeFunc->hasFnAttribute(Attribute::NoUnwind) ||
          CalleeFunc->hasFnAttribute(Attribute::ReturnsTwice)) {
        static_assert(std::is_same<decltype(FI.linesOfIO)::value_type,
          decltype(FI.linesOfStop)::value_type>::value,
          "Different storage types of call locations!");
        if (ShrinkCallLoc != 0 ||
            !std::count(FI.linesOfIO.begin(), FI.linesOfIO.end(), 0))
          FI.linesOfStop.push_back(ShrinkCallLoc);
      }
    }
  }
  // Store callers for each function.
  for (auto &Caller : CG) {
    if (!Caller.first || Caller.first->isDeclaration())
      continue;
    auto *FI = APCCtx.findFunction(*Caller.first);
    assert(FI && "Function should be registered in APC context!");
    auto &DT = getAnalysis<DominatorTreeWrapperPass>(
      const_cast<Function &>(*Caller.first)).getDomTree();
    auto &AA = getAnalysis<AAResultsWrapperPass>(
      const_cast<Function &>(*Caller.first)).getAAResults();
    for (auto &Callee : *Caller.second) {
      if (!Callee.first)
        continue;
      if (auto II = dyn_cast<IntrinsicInst>(*Callee.first))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      if (!Callee.second->getFunction())
        continue;
      auto CalleeFI = APCCtx.findFunction(*Callee.second->getFunction());
      // Some functions are not registered, for example functions without body.
      if (!CalleeFI)
        continue;
      CalleeFI->callsTo.push_back(FI);
      apc::FuncParam Param;
      auto *CB{cast<CallBase>(*Callee.first)};
      for (auto &Arg : Callee.second->getFunction()->args()) {
        if (Arg.hasStructRetAttr())
          continue;
        Param.parameters.push_back(nullptr);
        auto MRI = AA.getArgModRefInfo(CB, Arg.getArgNo());
        Param.inout_types.push_back(0);
        // TODO (kaniandr@gmail.com): implement interprocedural analysis
        // to determine in/out attributes for arguments.
        if (isModSet(MRI))
          Param.inout_types.back() |= OUT_BIT;
        if (isRefSet(MRI))
          Param.inout_types.back() |= IN_BIT;
        auto *ActualArg = CB->getArgOperand(Arg.getArgNo());
        SmallVector<DIMemoryLocation, 1> DILocs;
        auto DIM = findMetadata(ActualArg, DILocs);
        if (DIM) {
          assert(DIM->isValid() && "Metadata memory location must be valid!");
          if (DIM->Expr->getNumElements() == 0)
            Param.identificators.push_back(DIM->Var->getName().str());
          else
            Param.identificators.emplace_back();
          auto RawDIM = getRawDIMemoryIfExists(M.getContext(), *DIM);
          if (RawDIM && APCCtx.findArray(RawDIM))
            Param.parametersT.push_back(ARRAY_T);
          else
            Param.parametersT.push_back(UNKNOWN_T);
        } else {
          Param.identificators.emplace_back();
          Param.parametersT.push_back(UNKNOWN_T);
        }
      }
      Param.countOfPars = Param.identificators.size();
      CalleeFI->actualParams.push_back(std::move(Param));
    }
  }
  return false;
}

void APCFunctionInfoPass::print(raw_ostream &OS, const Module *M) const {
  if (mMultipleLaunch)
    OS << "warning: possible multiple launches of the pass for the same "
          "module: print merged results\n";
  auto printParams = [&OS](const apc::FuncParam &Params) {
    for (std::size_t I = 0, EI = Params.countOfPars; I < EI; ++I) {
      OS << "    idx: " << I << "\n";
      OS << "    id: " << Params.identificators[I] << "\n";
      if (Params.parametersT[I] == ARRAY_T)
        OS << "    array\n";
      OS << "    access:";
      if (Params.isArgIn((int)I))
        OS << " input";
      if (Params.isArgOut((int)I))
        OS << " output";
      OS << "\n";
    }
  };
  auto print = [&OS](const Twine &Msg,
      ArrayRef<
        decltype(std::declval<apc::FuncInfo>().linesOfIO)::value_type> Locs) {
    if (Locs.empty())
      return;
    OS << "  " << Msg;
    if (!Locs.empty()) {
      OS << " at ";
      for (auto Shrink : Locs) {
        std::pair<unsigned, unsigned> Loc;
        bcl::restoreShrinkedPair(Shrink, Loc.first, Loc.second);
        OS << format("%d:%d(shrink %d) ", Loc.first, Loc.second, Shrink);
      }
    }
    OS << "\n";
  };
  for (auto *FI : mFunctions) {
    std::pair<unsigned, unsigned> Start, End;
    bcl::restoreShrinkedPair(FI->linesNum.first, Start.first, Start.second);
    bcl::restoreShrinkedPair(FI->linesNum.second, End.first, End.second);
    OS << FI->funcName << " at " << FI->fileName <<
      format(":[%d:%d,%d:%d](shrink [%d,%d])\n",
        Start.first, Start.second, End.first, End.second,
        FI->linesNum.first, FI->linesNum.second);
    if (FI->isMain)
      OS << "  entry point\n";
    OS << "  arguments:\n";
    printParams(FI->funcParams);
    OS << "  calls from this function: ";
    for (auto &Callee : FI->callsFrom)
      OS << Callee << " ";
    OS << "\n";
    assert(FI->callsTo.size() == FI->actualParams.size() &&
      "Inconsistent number of actual parameters and calls!");
    OS << "  calls to this function:\n";
    for (std::size_t I = 0, EI = FI->callsTo.size(); I < EI; ++I) {
      OS << "    " << FI->callsTo[I]->funcName << " with "
         << FI->actualParams[I].countOfPars << " actual parameters\n";
      printParams(FI->actualParams[I]);
    }
    OS << "\n";
    print("has input/output", FI->linesOfIO);
    print("has unsafe CFG", FI->linesOfStop);
  }
}
