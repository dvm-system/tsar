//===---- LoopInfoBase.cpp ----- APC Loop Graph Builder  --------*- C++ -*-===//
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
// This file implements pass to obtain general information about loops. This
// pass perform low-level analysis, however may use results from other
// higher level passes without access underling AST structures. This pass does
// not check loop dependencies and it also does not collect memory accesses.
//
//===----------------------------------------------------------------------===//

#include "LoopGraphTraits.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/APC/Utils.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Support/Diagnostic.h"
#include "Attributes.h"
#include "DFRegionInfo.h"
#include "DIEstimateMemory.h"
#include "CanonicalLoop.h"
#include "KnownFunctionTraits.h"
#include "PerfectLoop.h"
#include "tsar_query.h"
#include "SourceUnparserUtils.h"
#include <apc/GraphLoop/graph_loops.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <bcl/utility.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-loop-base"

namespace {
class APCLoopInfoBasePass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  APCLoopInfoBasePass() : FunctionPass(ID) {
    initializeAPCLoopInfoBasePassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;
  void releaseMemory() override {
    mOuterLoops.clear();
#if !defined NDEBUG
    mAPCContext = nullptr;
    mRegions = nullptr;
    mPerfect = nullptr;
    mCanonical = nullptr;
    mSE = nullptr;
    mDT = nullptr;
    mAA = nullptr;
#endif
  }

private:
  void runOnLoop(Loop &L, apc::LoopGraph &APCLoop);
  void traverseInnerLoops(const Loop &L, apc::LoopGraph &Outer);
  /// Evaluate induction variable mention in the loop head and its bounds.
  ///
  /// \post Do not override default values (star, end, step) if some of values
  /// can not be computed.
  void evaluateInduction(const Loop &L, const DFLoop &DFL,
    bool KnownMaxBackageCount, apc::LoopGraph &APCLoop);
  void evaluateExits(const Loop &L, const DFLoop &DFL, apc::LoopGraph &APCLoop);
  void evaluateCalls(const Loop &L, apc::LoopGraph &APCLoop);

  // Conservatively assign some flags, these flags should be checked
  // in subsequent passes.
  void initNoAnalyzed(apc::LoopGraph &L) noexcept;

  APCContext *mAPCContext = nullptr;
  DFRegionInfo *mRegions = nullptr;
  ClangPerfectLoopPass *mPerfect = nullptr;
  CanonicalLoopPass *mCanonical = nullptr;
  ScalarEvolution *mSE = nullptr;
  DominatorTree *mDT = nullptr;
  AAResults *mAA = nullptr;

  std::vector<apc::LoopGraph *> mOuterLoops;
};
}

char APCLoopInfoBasePass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(APCLoopInfoBasePass, "apc-loop-info",
  "Loop Graph Builder (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
  INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(APCLoopInfoBasePass, "apc-loop-info",
  "Loop Graph Builder (APC)", true, true,
    DefaultQueryManager::PrintPassGroup::getPassRegistry())

FunctionPass * llvm::createAPCLoopInfoBasePass() {
  return new APCLoopInfoBasePass;
}

bool APCLoopInfoBasePass::runOnFunction(Function &F) {
  releaseMemory();
  mAPCContext = &getAnalysis<APCContextWrapper>().get();
  mRegions = &getAnalysis<DFRegionInfoPass>().getRegionInfo();
  mPerfect = &getAnalysis<ClangPerfectLoopPass>();
  mCanonical = &getAnalysis<CanonicalLoopPass>();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mAA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  for (auto I = LI.rbegin(), EI = LI.rend(); I != EI; ++I) {
    auto APCLoop = new apc::LoopGraph;
    assert((*I)->getLoopID() && "Loop ID must not be null, "
      "it seems that loop retriever pass has not been executed!");
    mAPCContext->addLoop((*I)->getLoopID(), APCLoop, true);
    runOnLoop(**I, *APCLoop);
    mOuterLoops.push_back(APCLoop);
    traverseInnerLoops(**I, *APCLoop);
  }
  return false;
}

void APCLoopInfoBasePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangPerfectLoopPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.setPreservesAll();
}

void APCLoopInfoBasePass::traverseInnerLoops(
    const Loop &L, apc::LoopGraph &Outer) {
  for (auto I = L.rbegin(), EI = L.rend(); I != EI; ++I) {
    auto Inner = new apc::LoopGraph;
    assert((*I)->getLoopID() && "Loop ID must not be null, "
      "it seems that loop retriever pass has not been executed!");
    mAPCContext->addLoop((*I)->getLoopID(), Inner);
    Outer.children.push_back(Inner);
    Inner->parent = &Outer;
    runOnLoop(**I, *Inner);
    traverseInnerLoops(**I, *Inner);
  }
  if (Outer.perfectLoop > 0)
    Outer.perfectLoop += Outer.children.front()->perfectLoop;
  else
    Outer.perfectLoop = 1;
}

void APCLoopInfoBasePass::runOnLoop(Loop &L, apc::LoopGraph &APCLoop) {
  initNoAnalyzed(APCLoop);
  auto *M = L.getHeader()->getModule();
  auto *F = L.getHeader()->getParent();
  APCLoop.region = &mAPCContext->getDefaultRegion();
  auto LocRange = L.getLocRange();
  if (auto &Loc = LocRange.getStart()) {
    if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), APCLoop.lineNum))
      emitUnableShrink(M->getContext(), *F, Loc, DS_Warning);
    if (auto Scope = dyn_cast_or_null<DIScope>(LocRange.getStart().getScope()))
      APCLoop.fileName = Scope->getFilename();
  }
  if (auto &Loc = LocRange.getEnd())
    if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), APCLoop.lineNumAfterLoop))
      emitUnableShrink(M->getContext(), *F, Loc, DS_Warning);
  if (APCLoop.fileName.empty())
    APCLoop.fileName = M->getSourceFileName();
  assert(mRegions->getRegionFor(&L) && "Loop region must not be null!");
  auto DFL = cast<DFLoop>(mRegions->getRegionFor(&L));
  APCLoop.perfectLoop =
    !L.empty() && mPerfect->getPerfectLoopInfo().count(DFL) ? 1 : 0;
  bool KnownMaxBackageCount = false;
  auto MaxBackedgeCount = mSE->getMaxBackedgeTakenCount(&L);
  if (MaxBackedgeCount && !isa<SCEVCouldNotCompute>(MaxBackedgeCount)) {
    if (!castSCEV(MaxBackedgeCount, /*IsSigned=*/ false, APCLoop.countOfIters)) {
      emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
        "unable to represent number of loop iterations", DS_Warning);
    } else {
      APCLoop.calculatedCountOfIters = APCLoop.countOfIters;
      KnownMaxBackageCount = true;
    }
  }
  evaluateInduction(L, *DFL, KnownMaxBackageCount, APCLoop);
  evaluateExits(L, *DFL, APCLoop);
  evaluateCalls(L, APCLoop);
}

void APCLoopInfoBasePass::initNoAnalyzed(apc::LoopGraph &APCLoop) noexcept {
  APCLoop.hasNonRectangularBounds = true;
  APCLoop.hasUnknownArrayDep = true;
  APCLoop.hasUnknownArrayAssigns = true;
  APCLoop.hasIndirectAccess = true;
}

void APCLoopInfoBasePass::evaluateInduction(const Loop &L, const DFLoop &DFL,
  bool KnownMaxBackageCount, apc::LoopGraph &APCLoop) {
  auto *M = L.getHeader()->getModule();
  auto *F = L.getHeader()->getParent();
  auto CanonItr = mCanonical->getCanonicalLoopInfo().find_as(&DFL);
  if (CanonItr == mCanonical->getCanonicalLoopInfo().end())
    return;
  if (auto Induct = (*CanonItr)->getInduction()) {
    auto DIM = buildDIMemory(MemoryLocation(Induct),
      M->getContext(), M->getDataLayout(), *mDT);
    if (DIM && DIM->isValid())
      if (auto DWLang = getLanguage(*DIM->Var)) {
        SmallString<16> DIName;
        if (unparseToString(*DWLang, *DIM, DIName))
          APCLoop.loopSymbol = DIName.str();
      }
  }

  if (!((*CanonItr)->isSigned() || (*CanonItr)->isUnsigned()))
    return;
  auto Start = (*CanonItr)->getStart();
  auto End = (*CanonItr)->getEnd();
  auto Step = (*CanonItr)->getStep();
  if (!Start || !End || !Step)
    return;
  auto StartVal = APCLoop.startVal;
  auto EndVal = APCLoop.endVal;
  auto StepVal = APCLoop.stepVal;
  if (!castSCEV(mSE->getSCEV(Start), (*CanonItr)->isSigned(), StartVal)) {
    emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
      "unable to represent lower bound of the loop", DS_Warning);
    return;
  }
  if (!castSCEV(Step, (*CanonItr)->isSigned(), StepVal)) {
    emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
      "unable to represent step of the loop", DS_Warning);     
    return;
  }
  if (StepVal == 0)
    return;
  auto Const = dyn_cast<SCEVConstant>(mSE->getSCEV(End));
  if (!Const)
    return;
  auto Val = Const->getAPInt();
  // Try to convert value from <, >, != comparison to equivalent
  // value from <=, >= comparison.
  switch ((*CanonItr)->getPredicate()) {
  case CmpInst::ICMP_SLT: case CmpInst::ICMP_ULT: --Val; break;
  case CmpInst::ICMP_SGT: case CmpInst::ICMP_UGT: ++Val; break;
  case CmpInst::ICMP_NE: StepVal > 0 ?-Val : ++Val;
  case CmpInst::ICMP_EQ: case CmpInst::ICMP_SGE: case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_SLE: case CmpInst::ICMP_ULE: break;
  default: return;
  }
  if (!castAPInt(Val, (*CanonItr)->isSigned(), EndVal)) {
    emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
      "unable to represent upper bound of the loop", DS_Warning);
      return;
  }
  APCLoop.startVal = StartVal;
  APCLoop.endVal = EndVal;
  APCLoop.stepVal = StepVal;
  if (!KnownMaxBackageCount && (*CanonItr)->isCanonical())
    if (EndVal > StartVal && StepVal > 0 ||
        EndVal < StartVal && StepVal < 0)
      APCLoop.calculatedCountOfIters = APCLoop.countOfIters
        = (EndVal - StartVal) / StepVal + (EndVal - StartVal) % StepVal;
}

void APCLoopInfoBasePass::evaluateExits(const Loop &L, const DFLoop &DFL,
    apc::LoopGraph &APCLoop) {
  if (DFL.getExitNode()->numberOfPredecessors() < 2) {
    APCLoop.hasGoto = false;
  } else {
    auto M = L.getHeader()->getModule();
    auto F = L.getHeader()->getParent();
    APCLoop.hasGoto = true;
    SmallVector<BasicBlock *, 4> Exiting;
    L.getExitingBlocks(Exiting);
    for (auto *BB : Exiting) {
      auto I = BB->getTerminator();
      if (!I || !I->getDebugLoc())
        continue;
      decltype(APCLoop.linesOfExternalGoTo)::value_type ShrinkLoc;
      if (!bcl::shrinkPair(
            I->getDebugLoc().getLine(), I->getDebugLoc().getCol(), ShrinkLoc))
        emitUnableShrink(M->getContext(), *F, I->getDebugLoc(), DS_Warning);
      else
        APCLoop.linesOfExternalGoTo.push_back(ShrinkLoc);
    }
  }
}

void APCLoopInfoBasePass::evaluateCalls(const Loop &L, apc::LoopGraph &APCLoop) {
  auto F = L.getHeader()->getParent();
  auto M = L.getHeader()->getModule();
  APCLoop.hasPrints = false;
  APCLoop.hasStops = false;
  APCLoop.hasNonPureProcedures = false;
  for (auto *BB : L.blocks()) 
    for (auto &I : *BB) {
      CallSite CS(&I);
      if (!CS)
        continue;
      auto Callee = CS.getCalledValue()->stripPointerCasts();
      if (auto II = dyn_cast<IntrinsicInst>(Callee))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      decltype(APCLoop.calls)::value_type::second_type ShrinkLoc = 0;
      if (auto &Loc = I.getDebugLoc())
        if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), ShrinkLoc))
          emitUnableShrink(M->getContext(), *F, Loc, DS_Warning);
      SmallString<16> CalleeName;
      unparseCallee(CS, *M, *mDT, CalleeName);
      APCLoop.calls.emplace_back(CalleeName.str(), ShrinkLoc);
      static_assert(std::is_same<decltype(APCLoop.linesOfIO)::value_type,
        decltype(APCLoop.calls)::value_type::second_type>::value,
        "Different storage types of call locations!");
      static_assert(std::is_same<decltype(APCLoop.linesOfIO)::value_type,
        decltype(APCLoop.linesOfStop)::value_type>::value,
        "Different storage types of call locations!");
      auto CalleeFunc = dyn_cast<Function>(Callee);
      if (!mAA->doesNotAccessMemory(CS) &&
          !AAResults::onlyAccessesArgPointees(mAA->getModRefBehavior(CS)))
        APCLoop.hasNonPureProcedures = true;
      if (!CalleeFunc || !hasFnAttr(*CalleeFunc, AttrKind::NoIO)) {
        APCLoop.hasPrints = true;
        if (ShrinkLoc != 0)
          APCLoop.linesOfIO.push_back(ShrinkLoc);
      }
      if (!CalleeFunc ||
          !hasFnAttr(*CalleeFunc, AttrKind::AlwaysReturn) ||
          !CalleeFunc->hasFnAttribute(Attribute::NoUnwind) ||
          CalleeFunc->hasFnAttribute(Attribute::ReturnsTwice)) {
        APCLoop.hasStops = true;
        if (ShrinkLoc != 0)
          APCLoop.linesOfStop.push_back(ShrinkLoc);
      } 
    }
}

void APCLoopInfoBasePass::print(raw_ostream &OS, const Module *M) const {
  for (auto &Root : mOuterLoops)
    for (auto *L : depth_first(Root)) {
      std::pair<unsigned, unsigned> StartLoc, EndLoc;
      bcl::restoreShrinkedPair(L->lineNum, StartLoc.first, StartLoc.second);
      bcl::restoreShrinkedPair(L->lineNumAfterLoop, EndLoc.first, EndLoc.second);
      OS << "APC loop at " << L->fileName <<
        format(":[%d:%d,%d:%d](shrink:[%d,%d])\n",
          StartLoc.first, StartLoc.second,
          EndLoc.first, EndLoc.second,
          L->lineNum, L->lineNumAfterLoop);
      OS << "  parallel region: " << L->region->GetName() << "\n";
      OS << "  size of perfect nest: " << L->perfectLoop << "\n";
      OS << "  number of iterations: " << L->calculatedCountOfIters << "\n";
      OS << "  induction";
      if (!L->loopSymbol.empty())
        OS << " variable: " << L->loopSymbol << ",";
      OS << format(" bounds: [%d,%d), step %d\n",
        L->startVal, L->endVal, L->stepVal);
      if (!L->calls.empty()) {
        OS << "  calls from loop: ";
        for (auto &Call : L->calls) {
          std::pair<unsigned, unsigned> Loc;
          bcl::restoreShrinkedPair(Call.second, Loc.first, Loc.second);
          OS << Call.first << " (";
          OS << format("%d:%d(shrink %d)", Loc.first, Loc.second, Call.second);
          OS << ") ";
        }
        OS << "\n";
      }
      auto print = [&OS](bool Flag, const Twine &Msg,
          ArrayRef<decltype(L->linesOfExternalGoTo)::value_type> Locs) {
        if (!Flag)
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
      print(L->hasGoto, "has additional exits", L->linesOfExternalGoTo);
      print(L->hasPrints, "has input/output", L->linesOfIO);
      print(L->hasStops, "has unsafe CFG", L->linesOfStop);
      if (L->hasNonPureProcedures)
        OS << "  has calls of non pure functions\n";
      if (L->hasNonRectangularBounds)
        OS << "  has non rectangular bounds\n";
      if (L->hasUnknownArrayDep)
        OS << "  has unknown array dependencies\n";
      if (L->hasUnknownArrayAssigns)
        OS << "  has unknown writes to an array\n";
      if (L->hasIndirectAccess)
        OS << "  has indirect memory assesses\n";
    }
}
