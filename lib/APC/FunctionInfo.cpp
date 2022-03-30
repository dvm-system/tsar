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
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
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
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-function-info"

using namespace llvm;
using namespace tsar;

namespace {
using APCFunctionInfoPassProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, EstimateMemoryPass,
                           DominatorTreeWrapperPass, AAResultsWrapperPass>;

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
    if (!Loc || Loc.getLine() == 0 && Loc.getCol() == 0)
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

INITIALIZE_PROVIDER(APCFunctionInfoPassProvider, "apc-function-info-provider",
  "Function Collector (APC, Provider")

INITIALIZE_PASS_BEGIN(APCFunctionInfoPass, "apc-function-info",
  "Function Collector (APC)", true, true)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCFunctionInfoPassProvider)
INITIALIZE_PASS_END(APCFunctionInfoPass, "apc-function-info",
  "Function Collector (APC)", true, true)

ModulePass * llvm::createAPCFunctionInfoPass() {
  return new APCFunctionInfoPass;
}

void APCFunctionInfoPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCFunctionInfoPassProvider>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<APCContextWrapper>();
  AU.setPreservesAll();
}

bool APCFunctionInfoPass::runOnModule(Module &M) {
  releaseMemory();
  auto &GlobalsAA{getAnalysis<GlobalsAAWrapperPass>().getResult() };
  APCFunctionInfoPassProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [&GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAA);
      });
  auto &DIMEnv{getAnalysis<DIMemoryEnvironmentWrapper>()};
  APCFunctionInfoPassProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEnv](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(*DIMEnv);
      });
  if (auto &GAP{getAnalysis<GlobalsAccessWrapper>()})
    APCFunctionInfoPassProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  for (auto &Caller : CG) {
    /// TODO (kaniandr@gmail.com): should we collect functions without body?
    if (!Caller.first || Caller.first->isDeclaration())
      continue;
    auto Pair = registerFunction(const_cast<Function &>(*Caller.first), APCCtx);
    mFunctions.push_back(Pair.first);
    auto &FI = *Pair.first;
    auto DIFunc = Caller.first->getSubprogram();
    /// TODO (kaniandr@gmail.com): should we emit warning or error if filename
    /// is not available from debug information.
    if (DIFunc && DIFunc->getFile()) {
      SmallString<128> PathToFile;
      FI.fileName = getAbsolutePath(*DIFunc, PathToFile).str();
    } else {
      SmallString<128> PathToFile;
      FI.fileName = sys::fs::real_path(M.getSourceFileName(), PathToFile)
                        ? PathToFile.str().str()
                        : M.getSourceFileName();
    }
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
    auto &CallerF{const_cast<Function &>(*Caller.first)};
    auto &Provider{getAnalysis<APCFunctionInfoPassProvider>(CallerF)};
    auto &DT{Provider.get<DominatorTreeWrapperPass>().getDomTree()};
    auto &DIAT{Provider.get<DIEstimateMemoryPass>().getAliasTree()};
    SmallVector<std::pair<DIEstimateMemory *, apc::Array *>, 16> Args{
        CallerF.arg_size(), std::pair{nullptr, nullptr}};
    for (auto &DIM : make_range(DIAT.memory_begin(), DIAT.memory_end())) {
      auto *DIEM{dyn_cast<DIEstimateMemory>(&DIM)};
      if (!DIEM)
        continue;
      auto *DIVar{dyn_cast<DILocalVariable>(DIEM->getVariable())};
      if (!DIVar)
        continue;
      if (!DIVar->isParameter())
        continue;
      auto ArgNum{DIVar->getArg() - 1};
      assert(ArgNum < Args.size() && "Argument out of range!");
      if (Args[ArgNum].second)
        continue;
      if (auto A{APCCtx.findArray(DIEM->getAsMDNode())})
        Args[ArgNum] = std::pair{DIEM, A};
    }
    for (auto &Arg : Caller.first->args()) {
      auto [DIEM, A] = Args[Arg.getArgNo()];
      if (!DIEM) {
        FI.funcParams.identificators.push_back(Arg.getName().str());
        FI.funcParams.parameters.push_back(nullptr);
        FI.funcParams.parametersT.push_back(UNKNOWN_T);
        FI.funcParams.inout_types.push_back(
            Arg.hasStructRetAttr() ? OUT_BIT : IN_BIT | OUT_BIT);
      } else {
        FI.funcParams.identificators.push_back(
            DIEM->getVariable()->getName().str());
        FI.funcParams.parameters.push_back(nullptr);
        if (A) {
          FI.funcParams.parametersT.push_back(ARRAY_T);
          FI.funcParams.parameters.back() = A;
        } else {
          FI.funcParams.parametersT.push_back(UNKNOWN_T);
        }
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
          FI.linesOfIO.insert(ShrinkCallLoc);
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
          FI.linesOfStop.insert(ShrinkCallLoc);
      }
    }
  }
  // Store callers for each function.
  for (auto &Caller : CG) {
    if (!Caller.first || Caller.first->isDeclaration())
      continue;
    auto *FI = APCCtx.findFunction(*Caller.first);
    assert(FI && "Function should be registered in APC context!");
    auto &Provider{getAnalysis<APCFunctionInfoPassProvider>(
        const_cast<Function &>(*Caller.first))};
    auto &DT{Provider.get<DominatorTreeWrapperPass>().getDomTree()};
    auto &AT{Provider.get<EstimateMemoryPass>().getAliasTree()};
    auto &AA{Provider.get<AAResultsWrapperPass>().getAAResults()};
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
      FI->callsFromV.insert(CalleeFI);
      apc::FuncParam Param;
      auto *CB{cast<CallBase>(*Callee.first)};
      for (auto &Arg : Callee.second->getFunction()->args()) {
        Param.parameters.push_back(nullptr);
        Param.identificators.emplace_back();
        Param.parametersT.push_back(UNKNOWN_T);
        auto MRI = AA.getArgModRefInfo(CB, Arg.getArgNo());
        Param.inout_types.push_back(0);
        // TODO (kaniandr@gmail.com): implement interprocedural analysis
        // to determine in/out attributes for arguments.
        if (isModSet(MRI))
          Param.inout_types.back() |= OUT_BIT;
        if (isRefSet(MRI))
          Param.inout_types.back() |= IN_BIT;
        auto *ActualArg = CB->getArgOperand(Arg.getArgNo());
        if (ActualArg->getType()->isPointerTy()) {
          int64_t Offset = 0;
          // Strip 'getelementptr' with zero offset to recognize an array
          // parameter.
          auto BasePtr = GetPointerBaseWithConstantOffset(ActualArg, Offset,
            M.getDataLayout());
          if (Offset == 0)
            ActualArg = BasePtr;
          if (auto *EM{AT.find(MemoryLocation(ActualArg, 0))})
            if (auto RawDIM{getRawDIMemoryIfExists(*EM->getTopLevelParent(),
                                                   M.getContext(),
                                                   M.getDataLayout(), DT)})
              if (auto *A{APCCtx.findArray(RawDIM)}) {
                Param.identificators.back() = A->GetShortName();
                Param.parametersT.back() = ARRAY_T;
                Param.parameters.back() = A;
              }
        }
      }
      Param.countOfPars = Param.identificators.size();
      FI->actualParams.push_back(std::move(Param));
      decltype(FI->linesOfIO)::value_type ShrinkCallLoc = 0;
      if (auto &Loc = cast<Instruction>(*Callee.first)->getDebugLoc())
        if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), ShrinkCallLoc))
          emitUnableShrink(M.getContext(), *Caller.first, Loc, DS_Warning);
      FI->detailCallsFrom.emplace_back(CalleeFI->funcName, ShrinkCallLoc);
      FI->parentForPointer.push_back(*Callee.first);
    }
  }
  LLVM_DEBUG(print(dbgs(), &M));
  return false;
}

void APCFunctionInfoPass::print(raw_ostream &OS, const Module *M) const {
  auto printParams = [&OS](const apc::FuncParam &Params, const Twine &Prefix) {
    for (std::size_t I = 0, EI = Params.countOfPars; I < EI; ++I) {
      OS << Prefix << "idx: " << I << "\n";
      OS << Prefix << "id: " << Params.identificators[I] << "\n";
      if (Params.parametersT[I] == ARRAY_T)
        OS << Prefix << "array\n";
      OS << Prefix << "access:";
      if (Params.isArgIn((int)I))
        OS << " input";
      if (Params.isArgOut((int)I))
        OS << " output";
      OS << "\n";
    }
  };
  auto print = [&OS](const Twine &Msg, const auto &Locs) {
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
    printParams(FI->funcParams, "    ");
    OS << "  calls from this function: ";
    for (auto &Callee : FI->callsFrom)
      OS << Callee << " ";
    OS << "\n";
    OS << "  calls to this function: ";
    for (auto &Callee : FI->callsTo)
      OS << Callee->funcName << " ";
    OS << "\n";
    assert(FI->detailCallsFrom.size() == FI->actualParams.size() &&
      "Inconsistent number of actual parameters and calls!");
    OS << "  registered calls from this function:\n";
    for (std::size_t I = 0, EI = FI->detailCallsFrom.size(); I < EI; ++I) {
      std::pair<unsigned, unsigned> Start, End;
      bcl::restoreShrinkedPair(FI->detailCallsFrom[I].second, Start.first,
                               Start.second);
      OS << "    " << FI->detailCallsFrom[I].first << " at " << Start.first
         << ":" << Start.second << " with " << FI->actualParams[I].countOfPars
         << " actual parameters (";
      assert(FI->parentForPointer[I] && "Call statement must not be null!");
      static_cast<Instruction *>(FI->parentForPointer[I])->print(OS);
      OS << ")\n";
      printParams(FI->actualParams[I], "      ");
    }
    OS << "\n";
    print("has input/output", FI->linesOfIO);
    print("has unsafe CFG", FI->linesOfStop);
  }
}
