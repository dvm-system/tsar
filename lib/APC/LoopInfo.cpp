//===---- LoopInfo.cpp ----- APC Loop Graph Builder  --------*- C++ -*-===//
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

#include "AstWrapperImpl.h"
#include "LoopGraphTraits.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/APC/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Support/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include <apc/GraphLoop/graph_loops.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <bcl/utility.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Dominators.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-loop-info"

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
    mSocket = nullptr;
    mAPCContext = nullptr;
    mDIMemoryMatcher = nullptr;
    mAccessInfo = nullptr;
    mGlobalOpts = nullptr;
    mRegions = nullptr;
    mPerfect = nullptr;
    mTfmCtx = nullptr;
    mCanonical = nullptr;
    mSE = nullptr;
    mDT = nullptr;
    mAA = nullptr;
    mPI = nullptr;
#endif
  }

private:
  void runOnLoop(Loop &L, apc::LoopGraph &APCLoop);
  void traverseInnerLoops(const Loop &L, apc::LoopGraph &Outer);
  /// Evaluate induction variable mention in the loop head and its bounds.
  ///
  /// \post Do not override default values (star, end, step) if some of values
  /// can not be computed.
  std::tuple<bool, Optional<APSInt>, Optional<APSInt>, Optional<APSInt>>
  evaluateInduction(const Loop &L, const CanonicalLoopInfo &CI,
                    bool KnownMaxBackageCount, apc::LoopGraph &APCLoop);
  void evaluateExits(const Loop &L, const DFLoop &DFL, apc::LoopGraph &APCLoop);
  void evaluateCalls(const Loop &L, apc::LoopGraph &APCLoop);

  // Conservatively assign some flags, these flags should be checked
  // in subsequent passes.
  void initNoAnalyzed(apc::LoopGraph &L) noexcept;

  AnalysisSocket *mSocket{nullptr};
  APCContext *mAPCContext = nullptr;
  DFRegionInfo *mRegions = nullptr;
  ClangTransformationContext *mTfmCtx{nullptr};
  ClangDIMemoryMatcherPass *mDIMemoryMatcher{nullptr};
  ClangPerfectLoopPass *mPerfect = nullptr;
  CanonicalLoopPass *mCanonical = nullptr;
  DIArrayAccessInfo *mAccessInfo{nullptr};
  const GlobalOptions *mGlobalOpts{nullptr};
  ScalarEvolution *mSE = nullptr;
  DominatorTree *mDT = nullptr;
  AAResults *mAA = nullptr;
  ParallelLoopInfo *mPI = nullptr;

  std::vector<apc::LoopGraph *> mOuterLoops;
};
}

char APCLoopInfoBasePass::ID = 0;

INITIALIZE_PASS_BEGIN(APCLoopInfoBasePass, "apc-loop-info",
  "Loop Graph Builder (APC)", true, true)
  INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DIArrayAccessWrapper)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
  INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(APCLoopInfoBasePass, "apc-loop-info",
  "Loop Graph Builder (APC)", true, true)

FunctionPass * llvm::createAPCLoopInfoBasePass() {
  return new APCLoopInfoBasePass;
}

bool APCLoopInfoBasePass::runOnFunction(Function &F) {
  releaseMemory();
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  mTfmCtx = TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                          TfmInfo->getContext(*CU))
                    : nullptr;
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    F.getContext().emitError(
        "can not transform sources"
        ": transformation context is not available for the '" +
        F.getName() + "` function");
    return false;
  }
  mAPCContext = &getAnalysis<APCContextWrapper>().get();
  mDIMemoryMatcher = &getAnalysis<ClangDIMemoryMatcherPass>();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  mRegions = &getAnalysis<DFRegionInfoPass>().getRegionInfo();
  mPerfect = &getAnalysis<ClangPerfectLoopPass>();
  mSocket = getAnalysis<AnalysisSocketImmutableWrapper>()->getActiveSocket();
  mCanonical = &getAnalysis<CanonicalLoopPass>();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mAA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  mPI = &getAnalysis<ParallelLoopPass>().getParallelLoopInfo();
  mAccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
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
  LLVM_DEBUG(print(dbgs(), F.getParent()));
  return false;
}

void APCLoopInfoBasePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangPerfectLoopPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<ParallelLoopPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DIArrayAccessWrapper>();
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
  auto LoopID{L.getLoopID()};
  assert(LoopID && "ID must be available for a loop!");
  auto *M = L.getHeader()->getModule();
  auto *F = L.getHeader()->getParent();
  APCLoop.region = &mAPCContext->getDefaultRegion();
  auto LocRange = L.getLocRange();
  if (auto &Loc = LocRange.getStart()) {
    if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), APCLoop.lineNum))
      emitUnableShrink(M->getContext(), *F, Loc, DS_Warning);
    if (auto Scope{dyn_cast_or_null<DIScope>(LocRange.getStart().getScope())};
        Scope && Scope->getFile()) {
      SmallString<128> PathToFile;
      APCLoop.fileName = getAbsolutePath(*Scope, PathToFile).str();
    }
  }
  if (auto &Loc = LocRange.getEnd())
    if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), APCLoop.lineNumAfterLoop))
      emitUnableShrink(M->getContext(), *F, Loc, DS_Warning);
  if (APCLoop.fileName.empty()) {
    SmallString<128> PathToFile;
    APCLoop.fileName = sys::fs::real_path(M->getSourceFileName(), PathToFile)
                           ? PathToFile.str().str()
                           : M->getSourceFileName();
  }
  assert(mRegions->getRegionFor(&L) && "Loop region must not be null!");
  auto DFL = cast<DFLoop>(mRegions->getRegionFor(&L));
  APCLoop.perfectLoop =
    !L.isInnermost() && mPerfect->getPerfectLoopInfo().count(DFL) ? 1 : 0;
  bool KnownMaxBackageCount = false;
  auto MaxBackedgeCount = mSE->getConstantMaxBackedgeTakenCount(&L);
  if (MaxBackedgeCount && !isa<SCEVCouldNotCompute>(MaxBackedgeCount)) {
    if (!castSCEV(MaxBackedgeCount, /*IsSigned=*/ false, APCLoop.countOfIters)) {
      emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
        "unable to represent number of loop iterations", DS_Warning);
    } else {
      APCLoop.calculatedCountOfIters = APCLoop.countOfIters;
      KnownMaxBackageCount = true;
    }
  }
  auto CanonItr{mCanonical->getCanonicalLoopInfo().find_as(DFL)};
  bool KnownInductToCountDep{false};
  Optional<APSInt> Start, Step, End;
  if (CanonItr != mCanonical->getCanonicalLoopInfo().end()) {
    std::tie(KnownInductToCountDep, Start, Step, End) =
        evaluateInduction(L, **CanonItr, KnownMaxBackageCount, APCLoop);
    APCLoop.isFor = APCLoop.inCanonicalFrom = (**CanonItr).isCanonical();
    APCLoop.hasNonRectangularBounds =
        L.getParentLoop() && !(**CanonItr).isRectangular();
  }
  if (!KnownInductToCountDep) {
    // If we cannot represent induction variable (in a head of for-loop) as
    // a function of loop count we cannot represent array subscripts as
    // a function of induction variable.
    // Example:
    // for (int I = 2; I < 100; ++I)
    //   A[I] = ...
    // LLVM IR of this loop has been transformed to
    // for (int I1 = 0; I1 < 98; ++I1)
    //   A[I1 + 2] = ...
    // If we do not known relation between I and I1 we cannot transform
    // subscripts to an original form. This means that we cannot set axis in
    // `parallel on` directive for the original loop, so we disable
    // parallelization of this loop.
    APCLoop.hasUnknownArrayAssigns = true;
  }
  evaluateExits(L, *DFL, APCLoop);
  evaluateCalls(L, APCLoop);
  for (auto &A : mAccessInfo->scope_accesses(LoopID)) {
    auto *APCArray{mAPCContext->findArray(A.getArray()->getAsMDNode())};
    if (!APCArray)
      continue;
    if (!APCArray->IsNotDistribute()) {
      APCLoop.usedArrays.insert(APCArray);
      if (!A.isReadOnly())
        APCLoop.usedArraysWrite.insert(APCArray);
    }
    APCLoop.usedArraysAll.insert(APCArray);
    if (!A.isReadOnly())
      APCLoop.usedArraysWriteAll.insert(APCArray);
  }
  if (auto ParallelInfoItr{mPI->find(&L)};
      ParallelInfoItr != mPI->end() &&
      CanonItr != mCanonical->getCanonicalLoopInfo().end() &&
      (**CanonItr).isCanonical()) {
    auto RF{
        mSocket->getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(
            *F)};
    assert(RF && "Dependence analysis must be available for a parallel loop!");
    auto &DIAT{RF->value<DIEstimateMemoryPass *>()->getAliasTree()};
    auto &DIDepInfo{RF->value<DIDependencyAnalysisPass *>()->getDependencies()};
    auto RM{mSocket->getAnalysis<AnalysisClientServerMatcherWrapper,
                                 ClonedDIMemoryMatcherWrapper>()};
    assert(RM && "Client to server IR-matcher must be available!");
    auto &ClientToServer{**RM->value<AnalysisClientServerMatcherWrapper *>()};
    auto ServerLoopID{cast<MDNode>(*ClientToServer.getMappedMD(L.getLoopID()))};
    auto DIDepSet{DIDepInfo[ServerLoopID]};
    auto *ServerF{cast<Function>(ClientToServer[F])};
    auto *DIMemoryMatcher{
        (**RM->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF]};
    assert(DIMemoryMatcher && "Cloned memory matcher must not be null!");
    auto &ASTToClient = mDIMemoryMatcher->getMatcher();
    auto *ForStmt{(**CanonItr).getASTLoop()};
    assert(ForStmt && "Source-level representation of a loop must be available!");
    auto &SrcMgr{mTfmCtx->getRewriter().getSourceMgr()};
    auto &Diags{SrcMgr.getDiagnostics()};
    ClangDependenceAnalyzer RegionAnalysis(
        const_cast<clang::ForStmt *>(ForStmt), *mGlobalOpts, Diags, DIAT,
        DIDepSet, *DIMemoryMatcher, ASTToClient);
    if (RegionAnalysis.evaluateDependency() &&
        RegionAnalysis.getDependenceInfo().get<trait::Induction>().size() ==
            1) {
      auto &DepInfo{RegionAnalysis.getDependenceInfo()};
      dvmh::InductionInfo BaseInduct;
      BaseInduct.get<trait::Induction>() =
          DepInfo.get<trait::Induction>().begin()->first;
      BaseInduct.get<tsar::Begin>() =
          DepInfo.get<trait::Induction>().begin()->second.get<tsar::Begin>();
      BaseInduct.get<tsar::End>() =
          DepInfo.get<trait::Induction>().begin()->second.get<tsar::End>();
      BaseInduct.get<tsar::Step>() =
          DepInfo.get<trait::Induction>().begin()->second.get<tsar::Step>();
      APCLoop.loopSymbol =
          BaseInduct.get<trait::Induction>().get<AST>()->getName().str();
      APCLoop.hasUnknownScalarDep = false;
      APCLoop.hasUnknownArrayDep = false;
      auto *S{new apc::LoopStatement{F, LoopID, &APCLoop, BaseInduct}};
      APCLoop.loop = S;
      mAPCContext->addStatement(S);
      S->getTraits().get<trait::Private>() = DepInfo.get<trait::Private>();
      S->getTraits().get<trait::Local>() = DepInfo.get<trait::Local>();
      S->getTraits().get<trait::Reduction>() = DepInfo.get<trait::Reduction>();
      auto Localized{RegionAnalysis.evaluateDefUse()};
      S->setIsHostOnly(!Localized || ParallelInfoItr->second.isHostOnly());
      S->getTraits().get<trait::WriteOccurred>() =
          DepInfo.get<trait::WriteOccurred>();
      S->getTraits().get<trait::ReadOccurred>() =
          DepInfo.get<trait::ReadOccurred>();
      if (!Localized) {
        auto *NotLocalized{RegionAnalysis.hasDiagnostic(
            tsar::diag::note_parallel_localize_inout_unable)};
        assert(NotLocalized && "Source of a diagnostic must be available!");
        S->getTraits().get<trait::WriteOccurred>().insert(NotLocalized->begin(),
                                                          NotLocalized->end());
        S->getTraits().get<trait::ReadOccurred>() =
            S->getTraits().get<trait::WriteOccurred>();
      }
      if (!DepInfo.get<trait::Dependence>().empty()) {
        auto ConstStep{dyn_cast_or_null<SCEVConstant>((*CanonItr)->getStep())};
        if (!ConstStep || !mAccessInfo) {
          APCLoop.hasUnknownArrayDep = true;
        } else {
          unsigned PossibleAcrossDepth{dvmh::processRegularDependencies(
                  LoopID, ConstStep, DepInfo, *mAccessInfo,
                  S->getTraits().get<trait::Dependence>())};
          if (PossibleAcrossDepth == 0)
            APCLoop.hasUnknownArrayDep = true;
          S->setPossibleAcrossDepth(PossibleAcrossDepth);
        }
      }
    } else {
      LLVM_DEBUG(dbgs() << "[APC LOOP]: unable to yield loop ("
                        << APCLoop.lineNum
                        << ") traits for AST-based representation\n");
      APCLoop.loop = new apc::LoopStatement{F, LoopID, &APCLoop};
      mAPCContext->addStatement(APCLoop.loop);
      APCLoop.hasUnknownScalarDep = true;
      APCLoop.hasUnknownArrayDep = true;
    }
  } else {
    LLVM_DEBUG(
        dbgs() << "[APC LOOP]: unable to analyze not "
               << (ParallelInfoItr == mPI->end() ? "parallel" : "canonical")
               << " loop (" << APCLoop.lineNum << ")\n");
    APCLoop.loop = new apc::LoopStatement{ F, LoopID, &APCLoop };
    mAPCContext->addStatement(APCLoop.loop);
    APCLoop.hasUnknownScalarDep = true;
    APCLoop.hasUnknownArrayDep = true;
  }
}

void APCLoopInfoBasePass::initNoAnalyzed(apc::LoopGraph &APCLoop) noexcept {
  APCLoop.hasUnknownArrayAssigns = true;
  APCLoop.hasIndirectAccess = true;
  APCLoop.hasUnknownDistributedMap = true;
  APCLoop.withoutDistributedArrays = false;
}

std::tuple<bool, Optional<APSInt>, Optional<APSInt>, Optional<APSInt>>
APCLoopInfoBasePass::evaluateInduction(const Loop &L,
                                       const CanonicalLoopInfo &CI,
                                       bool KnownMaxBackageCount,
                                       apc::LoopGraph &APCLoop) {
  auto *M = L.getHeader()->getModule();
  auto *F = L.getHeader()->getParent();
  auto Start = CI.getStart();
  auto Step = CI.getStep();
  if (!(CI.isSigned() || CI.isUnsigned())) {
    return std::tuple{!(Start && Step &&
                        isa<SCEVConstant>(mSE->getSCEV(Start)) &&
                        isa<SCEVConstant>(Step)),
                      None, None, None};
  }
  auto End = CI.getEnd();
  if (!Start || !End || !Step) {
    return std::tuple{!(Start && Step &&
                        isa<SCEVConstant>(mSE->getSCEV(Start)) &&
                        isa<SCEVConstant>(Step)),
                      None, None, None};
  }
  auto StartVal = APCLoop.startVal;
  auto EndVal = APCLoop.endVal;
  auto StepVal = APCLoop.stepVal;
  if (!castSCEV(mSE->getSCEV(Start), CI.isSigned(), StartVal)) {
    emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
      "unable to represent lower bound of the loop", DS_Warning);
    return std::tuple{true, None, None, None};
  }
  APSInt StartAPSInt{cast<SCEVConstant>(mSE->getSCEV(Start))->getAPInt(),
                          CI.isSigned()};
  if (!castSCEV(Step, CI.isSigned(), StepVal)) {
    emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
                     "unable to represent step of the loop", DS_Warning);
    return std::tuple{true, StartAPSInt, None, None};
  }
  if (StepVal == 0)
    return std::tuple{true, StartAPSInt, None, None};
  APSInt StepAPSInt{cast<SCEVConstant>(Step)->getAPInt(), CI.isSigned()};
  auto Const = dyn_cast<SCEVConstant>(mSE->getSCEV(End));
  if (!Const)
    return std::tuple{true, StartAPSInt, StepAPSInt, None};
  auto Val = Const->getAPInt();
  // Try to convert value from <, >, != comparison to equivalent
  // value from <=, >= comparison.
  switch (CI.getPredicate()) {
  case CmpInst::ICMP_SLT: case CmpInst::ICMP_ULT: --Val; break;
  case CmpInst::ICMP_SGT: case CmpInst::ICMP_UGT: ++Val; break;
  case CmpInst::ICMP_NE: StepVal > 0 ?-Val : ++Val;
  case CmpInst::ICMP_EQ: case CmpInst::ICMP_SGE: case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_SLE: case CmpInst::ICMP_ULE: break;
  default: return std::tuple{true, StartAPSInt, StepAPSInt, None};
  }
  if (!castAPInt(Val, CI.isSigned(), EndVal)) {
    emitTypeOverflow(M->getContext(), *F, L.getStartLoc(),
      "unable to represent upper bound of the loop", DS_Warning);
      return std::tuple{true, StartAPSInt, StepAPSInt, None};
  }
  APSInt EndAPSInt{Val, CI.isSigned()};
  APCLoop.startVal = StartVal;
  APCLoop.endVal = EndVal;
  APCLoop.stepVal = StepVal;
  if (!KnownMaxBackageCount && CI.isCanonical())
    if (EndVal > StartVal && StepVal > 0 ||
        EndVal < StartVal && StepVal < 0)
      APCLoop.calculatedCountOfIters = APCLoop.countOfIters
        = (EndVal - StartVal) / StepVal + (EndVal - StartVal) % StepVal;
  return std::tuple{true, StartAPSInt, StepAPSInt, EndAPSInt};
}

void APCLoopInfoBasePass::evaluateExits(const Loop &L, const DFLoop &DFL,
    apc::LoopGraph &APCLoop) {
    APCLoop.hasGoto = false;
  if (DFL.getExitNode()->numberOfPredecessors() > 1) {
    auto M = L.getHeader()->getModule();
    auto F = L.getHeader()->getParent();
    SmallVector<BasicBlock *, 4> Exiting;
    L.getExitingBlocks(Exiting);
    unsigned NumberOfExits{0};
    for (auto *BB : Exiting) {
      if (all_of(successors(BB), [&L](llvm::BasicBlock *SuccBB) {
            return L.contains(SuccBB) ||
                   llvm::isa<llvm::UnreachableInst>(SuccBB->front());
          }))
        continue;
      ++NumberOfExits;
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
    if (NumberOfExits == 1)
      APCLoop.linesOfExternalGoTo.clear();
    else
      APCLoop.hasGoto = true;
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
      auto *CB{dyn_cast<CallBase>(&I)};
      if (!CB)
        continue;
      auto Callee = CB->getCalledOperand()->stripPointerCasts();
      if (auto II = dyn_cast<IntrinsicInst>(Callee))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      decltype(APCLoop.calls)::value_type::second_type ShrinkLoc = 0;
      if (auto &Loc = I.getDebugLoc())
        if (!bcl::shrinkPair(Loc.getLine(), Loc.getCol(), ShrinkLoc))
          emitUnableShrink(M->getContext(), *F, Loc, DS_Warning);
      SmallString<16> CalleeName;
      unparseCallee(*CB, *M, *mDT, CalleeName);
      APCLoop.calls.emplace_back(CalleeName.str(), ShrinkLoc);
      static_assert(std::is_same<decltype(APCLoop.linesOfIO)::value_type,
        decltype(APCLoop.calls)::value_type::second_type>::value,
        "Different storage types of call locations!");
      static_assert(std::is_same<decltype(APCLoop.linesOfIO)::value_type,
        decltype(APCLoop.linesOfStop)::value_type>::value,
        "Different storage types of call locations!");
      auto CalleeFunc = dyn_cast<Function>(Callee);
      if (!mAA->doesNotAccessMemory(CB) &&
          !AAResults::onlyAccessesArgPointees(mAA->getModRefBehavior(CB)))
        APCLoop.hasNonPureProcedures = true;
      if (!CalleeFunc || !hasFnAttr(*CalleeFunc, AttrKind::NoIO)) {
        APCLoop.hasPrints = true;
        if (ShrinkLoc != 0)
          APCLoop.linesOfIO.insert(ShrinkLoc);
      }
      if (!CalleeFunc ||
          !hasFnAttr(*CalleeFunc, AttrKind::AlwaysReturn) ||
          !CalleeFunc->hasFnAttribute(Attribute::NoUnwind) ||
          CalleeFunc->hasFnAttribute(Attribute::ReturnsTwice)) {
        APCLoop.hasStops = true;
        if (ShrinkLoc != 0)
          APCLoop.linesOfStop.insert(ShrinkLoc);
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
      auto print = [&OS](bool Flag, const Twine &Msg, const auto &Locs) {
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
      if (L->hasUnknownScalarDep)
        OS << "  has unknown scalar dependencies\n";
      if (L->hasUnknownArrayAssigns)
        OS << "  has unknown writes to an array\n";
      if (L->hasIndirectAccess)
        OS << "  has indirect memory assesses\n";
    }
}
