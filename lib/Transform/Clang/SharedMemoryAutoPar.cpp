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
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Verifier.h>
#include <algorithm>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-shared-parallel"

void ClangSMParallelizationInfo::addBeforePass(
    legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
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
    Loop &L, Function &F, ClangSMParallelProvider &Provider) {
  if (!mRegions.empty() &&
      std::none_of(mRegions.begin(), mRegions.end(),
                   [&L](const OptimizationRegion *R) { return R->contain(L); }))
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  auto &PL = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
  auto &CL = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &RI = Provider.get<DFRegionInfoPass>().getRegionInfo();
  auto &LM = Provider.get<LoopMatcherPass>().getMatcher();
  auto &SrcMgr = mTfmCtx->getRewriter().getSourceMgr();
  auto &Diags = SrcMgr.getDiagnostics();
  if (!PL.count(&L))
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  auto LMatchItr = LM.find<IR>(&L);
  if (LMatchItr != LM.end())
    toDiag(Diags, LMatchItr->get<AST>()->getLocStart(),
           clang::diag::remark_parallel_loop);
  auto CanonicalItr = CL.find_as(RI.getRegionFor(&L));
  if (CanonicalItr == CL.end() || !(**CanonicalItr).isCanonical()) {
    toDiag(Diags, LMatchItr->get<AST>()->getLocStart(),
           clang::diag::warn_parallel_not_canonical);
    return findParallelLoops(L.begin(), L.end(), F, Provider);
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
  auto &ASTToClient = Provider.get<ClangDIMemoryMatcherPass>().getMatcher();
  auto *ForStmt = (**CanonicalItr).getASTLoop();
  assert(ForStmt && "Source-level representation of a loop must be available!");
  ClangDependenceAnalyzer RegionAnalysis(const_cast<clang::ForStmt *>(ForStmt),
    *mGlobalOpts, Diags, DIAT, DIDepSet, *DIMemoryMatcher, ASTToClient);
  if (!RegionAnalysis.evaluateDependency())
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  if (!exploitParallelism(L, *ForStmt, Provider, RegionAnalysis, *mTfmCtx))
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  return true;
}

void ClangSMParallelization::initializeProviderOnClient(Module &M) {
  ClangSMParallelProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  ClangSMParallelProvider::initialize<AnalysisSocketImmutableWrapper>(
      [this](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*mSocketInfo);
      });
  ClangSMParallelProvider::initialize<TransformationEnginePass>(
      [this, &M](TransformationEnginePass &Wrapper) {
        Wrapper.setContext(M, mTfmCtx);
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
}

bool ClangSMParallelization::runOnModule(Module &M) {
  releaseMemory();
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  mSocketInfo = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  mMemoryMatcher = &getAnalysis<MemoryMatcherImmutableWrapper>().get();
  mGlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  mDIMEnv = &getAnalysis<DIMemoryEnvironmentWrapper>().get();
  initializeProviderOnClient(M);
  auto &RegionInfo = getAnalysis<ClangRegionCollector>().getRegionInfo();
  if (mGlobalOpts->OptRegions.empty()) {
    transform(RegionInfo, std::back_inserter(mRegions),
              [](const OptimizationRegion &R) { return &R; });
  } else {
    for (auto &Name : mGlobalOpts->OptRegions)
      if (auto *R = RegionInfo.get(Name))
        mRegions.push_back(R);
      else
        toDiag(mTfmCtx->getContext().getDiagnostics(),
               clang::diag::warn_region_not_found) << Name;
  }
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    if (I->size() > 1)
      continue;
    auto *F = I->front()->getFunction();
    if (!F || F->isIntrinsic() || F->isDeclaration() ||
        hasFnAttr(*F, AttrKind::LibFunc))
      continue;
    if (!mRegions.empty() && std::all_of(mRegions.begin(), mRegions.end(),
                                         [F](const OptimizationRegion *R) {
                                           return R->contain(*F) ==
                                                  OptimizationRegion::CS_No;
                                         }))
      continue;
    LLVM_DEBUG(dbgs() << "[SHARED PARALLEL]: process function " << F->getName()
                      << "\n");
    auto &Provider = getAnalysis<ClangSMParallelProvider>(*F);
    auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
    findParallelLoops(LI.begin(), LI.end(), *F, Provider);
  }
  return false;
}

void ClangSMParallelization::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangSMParallelProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<ClangRegionCollector>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.setPreservesAll();
}

INITIALIZE_PROVIDER(ClangSMParallelProvider,
                    "clang-shared-parallel-provider",
                    "Shared Memory Parallelization (Clang, Provider)")

ClangSMParallelization::ClangSMParallelization(char &ID) : ModulePass(ID) {
  initializeClangSMParallelProviderPass(*PassRegistry::getPassRegistry());
}

