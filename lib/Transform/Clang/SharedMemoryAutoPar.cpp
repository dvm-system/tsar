//===- SharedMemoryAutoPar.cpp - Shared Memory Parallelization ---*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a general abstract pass to perform auto parallelization
// for a shared memory.
//
//===----------------------------------------------------------------------===//

#include "SharedMemoryAutoPar.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/ExpressionMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Analysis/Reader/RegionWeights.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Verifier.h>
#include <algorithm>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-shared-parallel"

namespace {
template <typename... Analysis>
FunctionPassAAProvider<std::remove_pointer_t<Analysis>...>
functionAnalysisList(bcl::StaticTypeMap<Analysis...> &&);

using ClangSMParallelProvider =
    decltype(functionAnalysisList(std::declval<FunctionAnalysis>()));
}

void ClangSMParallelizationInfo::addBeforePass(
    legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  Passes.add(createGlobalsAccessStorage());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(createDIMemoryAnalysisServer());
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createAnalysisWaitServerPass());
}

void ClangSMParallelizationInfo::addAfterPass(
    legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}

bool ClangSMParallelization::findParallelLoops(
    Loop &L, const FunctionAnalysis &Provider,
    TransformationContextBase &TfmCtx, ParallelItem *PI) {
  auto &F = *L.getHeader()->getParent();
  if (!mRegions.empty() &&
    std::none_of(mRegions.begin(), mRegions.end(),
      [&L](const OptimizationRegion *R) { return R->contain(L); })) {
    assert(!PI && "Loop located inside a parallel item must be contained in an "
                  "optimization region!");
    return findParallelLoops(&L, L.begin(), L.end(), Provider, TfmCtx, PI);
  }
  auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
  auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
  auto &RI = Provider.value<DFRegionInfoPass *>()->getRegionInfo();
  auto &LM = Provider.value<LoopMatcherPass *>()->getMatcher();
  auto &SrcMgr = cast<ClangTransformationContext>(TfmCtx).getRewriter().getSourceMgr();
  auto &Diags = SrcMgr.getDiagnostics();
  if (!PL.count(&L)) {
    if (PI)
      PI->finalize();
    if (!PI || PI && PI->isChildPossible())
      return findParallelLoops(&L, L.begin(), L.end(), Provider, TfmCtx, PI);
    return false;
  }
  auto LMatchItr = LM.find<IR>(&L);
  if (LMatchItr != LM.end())
    toDiag(Diags, LMatchItr->get<AST>()->getBeginLoc(),
           tsar::diag::remark_parallel_loop);
  auto DFL = cast<DFLoop>(RI.getRegionFor(&L));
  auto CanonicalItr = CL.find_as(DFL);
  if (CanonicalItr == CL.end() || !(**CanonicalItr).isCanonical()) {
    toDiag(Diags, LMatchItr->get<AST>()->getBeginLoc(),
           tsar::diag::warn_parallel_not_canonical);
    if (PI)
      PI->finalize();
    if (!PI || PI && PI->isChildPossible())
      return findParallelLoops(&L, L.begin(), L.end(), Provider, TfmCtx, PI);
    return false;
  }
  auto Weights = &getAnalysis<RegionWeightsEstimator>();
  auto LoopW = Weights->getSelfWeight(L.getLoopID());
  if (LoopW.first.isValid() &&
      LoopW.first < mGlobalOpts->LoopParallelThreshold) {
    toDiag(Diags, LMatchItr->get<AST>()->getBeginLoc(),
           tsar::diag::note_parallel_loop_threshold)
        << mGlobalOpts->LoopParallelThreshold;
    return false;
  }
  auto &Socket = mSocketInfo->getActive()->second;
  auto RF =
      Socket.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(F);
  assert(RF && "Dependence analysis must be available for a parallel loop!");
  auto &DIAT = RF->value<DIEstimateMemoryPass *>()->getAliasTree();
  auto &DIDepInfo = RF->value<DIDependencyAnalysisPass *>()->getDependencies();
  auto RM = Socket.getAnalysis<AnalysisClientServerMatcherWrapper,
                                 ClonedDIMemoryMatcherWrapper>();
  assert(RM && "Client to server IR-matcher must be available!");
  auto &ClientToServer = **RM->value<AnalysisClientServerMatcherWrapper *>();
  assert(L.getLoopID() && "ID must be available for a parallel loop!");
  auto ServerLoopID = cast<MDNode>(*ClientToServer.getMappedMD(L.getLoopID()));
  auto DIDepSet = DIDepInfo[ServerLoopID];
  auto *ServerF = cast<Function>(ClientToServer[&F]);
  auto *DIMemoryMatcher =
      (**RM->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF];
  assert(DIMemoryMatcher && "Cloned memory matcher must not be null!");
  auto &ASTToClient =
      Provider.value<ClangDIMemoryMatcherPass *>()->getMatcher();
  auto *ForStmt = (**CanonicalItr).getASTLoop();
  assert(ForStmt && "Source-level representation of a loop must be available!");
  ClangDependenceAnalyzer RegionAnalysis(const_cast<clang::ForStmt *>(ForStmt),
    *mGlobalOpts, Diags, DIAT, DIDepSet, *DIMemoryMatcher, ASTToClient);
  if (!RegionAnalysis.evaluateDependency()) {
    if (PI)
      PI->finalize();
    if (!PI || PI && PI->isChildPossible())
      return findParallelLoops(&L, L.begin(), L.end(), Provider, TfmCtx, PI);
    return false;
  }
  bool InParallelItem = PI;
  PI = exploitParallelism(*DFL, *ForStmt, Provider, RegionAnalysis, PI);
  if (PI && !InParallelItem) {
    for (auto *BB : L.blocks())
      for (auto &I : *BB) {
        auto *Call = dyn_cast<CallBase>(&I);
        if (!Call)
          continue;
        if (isDbgInfoIntrinsic(Call->getIntrinsicID()) ||
            isMemoryMarkerIntrinsic(Call->getIntrinsicID()))
          continue;
        auto Callee = dyn_cast<Function>(
          Call->getCalledOperand()->stripPointerCasts());
        if (!Callee)
          continue;
        assert(mAdjacentList.count(Callee) && "Call to an unknown function!");
        mParallelCallees.try_emplace(Callee, mAdjacentList[Callee].get<Id>());
      }
  }
  bool Parallelized{PI != nullptr};
  if (!PI || !PI->isFinal())
    Parallelized |=
        findParallelLoops(&L, L.begin(), L.end(), Provider, TfmCtx, PI);
  return Parallelized;
}

void ClangSMParallelization::initializeProviderOnClient() {
  ClangSMParallelProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  ClangSMParallelProvider::initialize<AnalysisSocketImmutableWrapper>(
      [this](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*mSocketInfo);
      });
  ClangSMParallelProvider::initialize<TransformationEnginePass>(
      [this](TransformationEnginePass &Wrapper) {
        Wrapper.set(*mTfmInfo);
      });
  ClangSMParallelProvider::initialize<MemoryMatcherImmutableWrapper>(
      [this](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*mMemoryMatcher);
      });
  ClangSMParallelProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [this](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*mGlobalsAA);
      });
  ClangSMParallelProvider::initialize<DIMemoryEnvironmentWrapper>(
      [this](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(*mDIMEnv);
      });
  if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
    ClangSMParallelProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
}

std::size_t ClangSMParallelization::buildAdjacentList() {
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::size_t LastPostorderNum = 0;
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    bool HasCycle = I.hasCycle();
    auto CurrSize = mAdjacentList.size();
    for (auto *CGN : *I) {
      if (CGN->getFunction()) {
        auto &Node = mAdjacentList[CGN->getFunction()];
        Node.get<Id>() = LastPostorderNum;
        Node.get<InCycle>() = HasCycle;
      }
    }
    if (mAdjacentList.size() > CurrSize) {
      ++LastPostorderNum;
      auto FuncRange =
          make_range(mAdjacentList.begin() + CurrSize, mAdjacentList.end());
      for (auto *CGN : *I) {
        for (auto &Call : *I->front())
          if (Call.second == CG.getCallsExternalNode()) {
            for (auto &Info: FuncRange)
              Info.second.get<HasUnknownCalls>() = true;
          } else if (Call.second != CGN) {
            assert(Call.second->getFunction() &&
              "Call to an unknown function!");
            auto Itr = mAdjacentList.find(Call.second->getFunction());
            assert(Itr != mAdjacentList.end() && "Unknown function!");
            for (auto &Info : FuncRange)
              if (!Info.second.get<Adjacent>()
                       .insert(Itr->second.get<Id>())
                       .second)
                break;
            ;
          }
      }
    }
  }
  if (CG.getExternalCallingNode())
    for (auto &ExternalCall : *CG.getExternalCallingNode())
      if (auto *F = ExternalCall.second->getFunction())
        mExternalCalls.insert(mAdjacentList[F].get<Id>());
  LLVM_DEBUG(
    for (auto &SCC : mAdjacentList) {
      dbgs() << "[SHARED PARALLEL]: function " << SCC.first->getName()
             << " in SCC " << SCC.second.get<Id>()
             << ", has" << (SCC.second.get<InCycle>() ? " " : " no ") << "cycle"
             << ", has" << (SCC.second.get<HasUnknownCalls>() ? " " : " no ")
             << "unknown calls"
             << ", adjacent SCCs:";
      for (auto SCCId : SCC.second.get<Adjacent>())
        dbgs() << " " << SCCId;
      dbgs() << "\n";
    }
    dbgs() << "[SHARED PARALLEL]: external calls to SCCs:";
    for (auto SCCId : mExternalCalls)
      dbgs() << " " << SCCId;
    dbgs() << "\n");
  return LastPostorderNum;
}

template <typename Adjacent, typename HasUnknownCalls, typename AdjacentListT>
static void addToReachability(const AdjacentListT &AdjacentList,
                              const DenseSet<std::size_t> ExternalCalls,
                              const typename AdjacentListT::value_type &SCC,
                              SmallVectorImpl<std::size_t> &Path,
                              DenseSet<std::size_t> &Finished,
                              DenseSet<std::size_t> &Visited,
                              bcl::marray<bool, 2> &Reachability) {
  for (auto To : SCC.second.template get<Adjacent>()) {
    if (!Visited.insert(To).second)
      continue;
    for (auto From : Path)
      Reachability[From][To] = true;
    if (Finished.insert(To).second) {
      Path.push_back(To);
      addToReachability<Adjacent, HasUnknownCalls>(
          AdjacentList, ExternalCalls, *(AdjacentList.begin() + To), Path,
          Finished, Visited, Reachability);
      Path.pop_back();
    } else {
      addToReachability<Adjacent, HasUnknownCalls>(
          AdjacentList, ExternalCalls, *(AdjacentList.begin() + To), Path,
          Finished, Visited, Reachability);
    }
  }
  // It's not necessary to add call to external functions if there are
  // unknown calls from the current SCC. There are two possibility for unknown
  // calls. The first one is call to user-defined functions. However, these
  // calls prevent parallelization of a loop (DirectUserCallee attribute).
  // So, it's not important whether a function, which we consider to parallelize,
  // is reachable from this unknown callee. The second case is call to a library
  // function. We assume that there is no user-defined functions which are
  // reachable from library functions. So, this case can be also ignored.
}

bool ClangSMParallelization::runOnModule(Module &M) {
  releaseMemory();
  auto &TfmInfoPass{ getAnalysis<TransformationEnginePass>() };
  mTfmInfo = TfmInfoPass ? &TfmInfoPass.get() : nullptr;
  if (!mTfmInfo) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  mSocketInfo = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  mMemoryMatcher = &getAnalysis<MemoryMatcherImmutableWrapper>().get();
  mGlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  mDIMEnv = &getAnalysis<DIMemoryEnvironmentWrapper>().get();
  initializeProviderOnClient();
  auto &RegionInfo = getAnalysis<ClangRegionCollector>().getRegionInfo();
  if (mGlobalOpts->OptRegions.empty()) {
    transform(RegionInfo, std::back_inserter(mRegions),
              [](const OptimizationRegion &R) { return &R; });
  } else {
    auto *CUs{M.getNamedMetadata("llvm.dbg.cu")};
    auto CXXCUItr{find_if(CUs->operands(), [](auto *MD) {
      auto *CU{dyn_cast<DICompileUnit>(MD)};
      return CU &&
             (isC(CU->getSourceLanguage()) || isCXX(CU->getSourceLanguage()));
    })};
    if (CXXCUItr == CUs->op_end()) {
      M.getContext().emitError(
          "cannot transform sources"
          ": transformation of C/C++ sources are only possible now");
      return false;
    }
    auto *TfmCtx{dyn_cast_or_null<ClangTransformationContext>(
        mTfmInfo->getContext(cast<DICompileUnit>(**CXXCUItr)))};
    if (!TfmCtx || !TfmCtx->hasInstance()) {
      M.getContext().emitError("cannot transform sources"
                               ": transformation context is not available");
      return false;
    }
    for (auto &Name : mGlobalOpts->OptRegions)
      if (auto *R = RegionInfo.get(Name))
        mRegions.push_back(R);
      else
        toDiag(TfmCtx->getContext().getDiagnostics(),
               tsar::diag::warn_region_not_found) << Name;
  }
  auto NumberOfSCCs = buildAdjacentList();
  bcl::marray<bool, 2> Reachability({NumberOfSCCs, NumberOfSCCs});
  for (std::size_t I = 0, EI = NumberOfSCCs; I < EI; ++I)
    for (std::size_t J = 0, EJ = NumberOfSCCs; J < EJ; ++J)
      Reachability[I][J] = false;
  DenseSet<std::size_t> Finished;
  std::size_t PrevSCCId = NumberOfSCCs;
  // Build reachability matrix for SCCs.
  for (auto &SCC : mAdjacentList) {
    if (SCC.second.get<Id>() == PrevSCCId)
      continue;
    PrevSCCId = SCC.second.get<Id>();
    if (!Finished.insert(SCC.second.get<Id>()).second)
      continue;
    SmallVector<std::size_t, 8> Path;
    Path.push_back(SCC.second.get<Id>());
    DenseSet<std::size_t> Visited;
    addToReachability<Adjacent, HasUnknownCalls>(mAdjacentList, mExternalCalls,
                                                 SCC, Path, Finished, Visited,
                                                 Reachability);
  }
  LLVM_DEBUG(dbgs() << "[SHARED PARALLEL]: reachability matrix:\n";
             for (std::size_t I = 0, EI = NumberOfSCCs; I < EI; ++I) {
               for (std::size_t J = 0, EJ = NumberOfSCCs; J < EJ; ++J)
                 dbgs() << Reachability[I][J] << " ";
               dbgs() << "\n";
             });
  for (auto &Current : llvm::reverse(mAdjacentList)) {
    auto *F = Current.first;
    auto &Node = Current.second;
    if (Node.get<InCycle>() || !F || F->isIntrinsic() || F->isDeclaration() ||
        hasFnAttr(*F, AttrKind::LibFunc))
      continue;
    if (!mRegions.empty() && std::all_of(mRegions.begin(), mRegions.end(),
                                         [F](const OptimizationRegion *R) {
                                           return R->contain(*F) ==
                                                  OptimizationRegion::CS_No;
                                         }))
      continue;
    // Check that current function is not reachable from any parallel region.
    if (isParallelCallee(*F, Node.get<Id>(), Reachability)) {
      LLVM_DEBUG(dbgs() << "[SHARED PARALLEL]: ignore function reachable from "
                           "parallel region "
                        << F->getName() << "\n");
      continue;
    }
    LLVM_DEBUG(dbgs() << "[SHARED PARALLEL]: process function " << F->getName()
                      << "\n");
    if (auto *DISub{findMetadata(F)})
      if (auto *CU{DISub->getUnit()}; CU && (isC(CU->getSourceLanguage()) ||
                                             isCXX(CU->getSourceLanguage()))) {
        auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
        auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                                   TfmInfo->getContext(*CU))
                             : nullptr};
        if (TfmCtx && TfmCtx->hasInstance()) {
          auto Provider = analyzeFunction(*F);
          auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
          findParallelLoops(F, LI.begin(), LI.end(), Provider, *TfmCtx,
                            nullptr);
          continue;
        }
      }
    F->getContext().emitError(
        "cannot transform sources"
        ": transformation context is not available for the '" +
        F->getName() + "' function");
    return false;
  }
  finalize(M, Reachability);
  return false;
}

FunctionAnalysis
ClangSMParallelization::analyzeFunction(llvm::Function &F) {
  auto &Provider = getAnalysis<ClangSMParallelProvider>(F);
  FunctionAnalysis Results;
  Results.for_each([&Provider](auto &T) {
    T = &Provider.get<std::remove_pointer_t<std::decay_t<decltype(T)>>>();
  });
  return Results;
}

std::pair<bool, bool>
ClangSMParallelization::needToOptimize(const Function &F) const {
  if (mRegions.empty())
    return std::pair{true, true};
  bool Optimize{false}, OptimizeChildren{false};
  for (const auto *R : mRegions) {
    switch (R->contain(F)) {
    case OptimizationRegion::CS_No:
      continue;
    case OptimizationRegion::CS_Always:
    case OptimizationRegion::CS_Condition:
      Optimize = true;
      [[fallthrough]];
    case OptimizationRegion::CS_Child:
      OptimizeChildren = true;
      break;
    default:
      llvm_unreachable("Unkonwn region contain status!");
    }
    if (Optimize)
      break;
  }
  return std::pair{Optimize, OptimizeChildren};
}

bool ClangSMParallelization::needToOptimize(const Loop &L) const {
  if (mRegions.empty())
    return true;
  return any_of(mRegions, [&L](auto *R) { return R->contain(L); });
}

void ClangSMParallelization::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangSMParallelProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<ClangRegionCollector>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<DIArrayAccessWrapper>();
  AU.addRequired<RegionWeightsEstimator>();
  AU.setPreservesAll();
}

INITIALIZE_PROVIDER(ClangSMParallelProvider,
                    "clang-shared-parallel-provider",
                    "Shared Memory Parallelization (Clang, Provider)")

ClangSMParallelization::ClangSMParallelization(char &ID) : ModulePass(ID) {
  initializeClangSMParallelProviderPass(*PassRegistry::getPassRegistry());
}
