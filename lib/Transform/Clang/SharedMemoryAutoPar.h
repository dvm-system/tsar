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

#ifndef TSAR_CLANG_SHARED_PARALLEL_H
#define TSAR_CLANG_SHARED_PARALLEL_H

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Support/PassGroupRegistry.h"
#include <bcl/cell.h>
#include <bcl/marray.h>
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

namespace clang {
class ForStmt;
}

namespace tsar {
class DFLoop;
class AnalysisSocketInfo;
class ClangDependenceAnalyzer;
class DIMemoryEnvironment;
class ParallelItem;
class OptimizationRegion;
class TransformationContextBase;
class TransformationInfo;
struct GlobalOptions;
struct MemoryMatchInfo;
}

namespace llvm {
class GlobalsAAResult;
class Function;
class Loop;

class CanonicalLoopPass;
class ClangPerfectLoopPass;
class ClangDIMemoryMatcherPass;
class ClangExprMatcherPass;
class DIEstimateMemoryPass;
class DFRegionInfoPass;
class DominatorTreeWrapperPass;
class PostDominatorTreeWrapperPass;
class EstimateMemoryPass;
class LoopMatcherPass;
class LoopInfoWrapperPass;
class ParallelLoopPass;

/// This provide access to function-level analysis results on client.
using FunctionAnalysis =
    bcl::StaticTypeMap<AnalysisSocketImmutableWrapper *, LoopInfoWrapperPass *,
                       ParallelLoopPass *, CanonicalLoopPass *,
                       LoopMatcherPass *, DFRegionInfoPass *,
                       EstimateMemoryPass *, DominatorTreeWrapperPass *,
                       PostDominatorTreeWrapperPass *,
                       ClangDIMemoryMatcherPass *, DIEstimateMemoryPass *,
                       MemoryMatcherImmutableWrapper *, ClangPerfectLoopPass *,
                       ClangExprMatcherPass *>;

/// This pass try to insert directives into a source code to obtain
/// a parallel program for a shared memory.
class ClangSMParallelization: public ModulePass, private bcl::Uncopyable {
public:
  struct Id {};
  struct InCycle {};
  struct HasUnknownCalls {};
  struct Adjacent {};

  /// Representation of a call graph as an adjacent list. Note that calls
  /// inside the same SCC are ignored and all functions from the same SCC
  /// have the same IDs.
  using AdjacentListT =
      MapVector<Function *,
                bcl::tagged_tuple<
                    bcl::tagged<std::size_t, Id>, bcl::tagged<bool, InCycle>,
                    bcl::tagged<bool, HasUnknownCalls>,
                    bcl::tagged<llvm::SmallSet<std::size_t, 16>, Adjacent>>>;

  ClangSMParallelization(char &ID);

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override {
    mRegions.clear();
    mTfmInfo = nullptr;
    mGlobalOpts = nullptr;
    mMemoryMatcher = nullptr;
    mGlobalsAA = nullptr;
    mSocketInfo = nullptr;
  }

protected:
  /// Exploit parallelism for a specified loop.
  ///
  /// This function is will be called if some general conditions are
  /// successfully checked.
  /// \return true if a specified loop could be parallelized and inner loops
  /// should not be processed.
  virtual tsar::ParallelItem *
  exploitParallelism(const tsar::DFLoop &IR, const clang::ForStmt &AST,
                     const FunctionAnalysis &Provider,
                     tsar::ClangDependenceAnalyzer &ASTDepInfo,
                     tsar::ParallelItem *PI) = 0;

  /// Process loop after its body parallelization.
  virtual void optimizeLevel(PointerUnion<Loop *, Function *> Level,
      const FunctionAnalysis &Provider) {}

  /// Finalize parallelization.
  virtual void finalize(llvm::Module &M, bcl::marray<bool, 2> &Reachability) {}

  /// Return analysis results computed on the client for a specified function.
  FunctionAnalysis analyzeFunction(llvm::Function &F);

  const AdjacentListT &functions() const noexcept { return mAdjacentList; }

  // Return true if functions from a specified SCC might be called from an
  // unknown external node.
  bool hasExternalCalls(std::size_t Id) const {
    return mExternalCalls.count(Id);
  }

  /// Return list of optimization regions.
  llvm::ArrayRef<const tsar::OptimizationRegion *>
  getOptimizationRegions() const {
    return mRegions;
  }

  /// Check if a specified function should be optimized.
  /// The first value is true if it is necessary to optimize the whole function.
  /// The second value is true if at least part of functions (nested loops)
  /// should be optimized.
  std::pair<bool, bool> needToOptimize(const Function &F) const;

  /// Return true if a specified loop should be optimized.
  bool needToOptimize(const Loop &L) const;

  /// Return true if a specified function is called from a parallel loop (may be
  /// implicitly).
  bool isParallelCallee(const Function &F, std::size_t Id,
                        bcl::marray<bool, 2> &Reachability) const {
    return mParallelCallees.count(&F) ||
           llvm::any_of(mParallelCallees,
                        [Id, &Reachability](const auto &Parallel) {
                          return Reachability[Parallel.second][Id];
                        });
  }

private :
    /// Initialize provider before on the fly passes will be run on client.
    void
    initializeProviderOnClient();

  /// Check whether it is possible to parallelize a specified loop, analyze
  /// inner loops on failure.
    bool findParallelLoops(Loop &L, const FunctionAnalysis &Provider,
                           tsar::TransformationContextBase &TfmCtx,
                           tsar::ParallelItem *PI);

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
    bool findParallelLoops(PointerUnion<Loop *, Function *> Parent, ItrT I,
                           ItrT EI, const FunctionAnalysis &Provider,
                           tsar::TransformationContextBase &TfmCtx,
                           tsar::ParallelItem *PI) {
    auto Parallelized = false;
    for (; I != EI; ++I)
      Parallelized |= findParallelLoops(**I, Provider, TfmCtx, PI);
    if (Parallelized)
      optimizeLevel(Parent, Provider);
    return Parallelized;
  }


  std::size_t buildAdjacentList();

  tsar::TransformationInfo *mTfmInfo= nullptr;
  const tsar::GlobalOptions *mGlobalOpts = nullptr;
  tsar::MemoryMatchInfo *mMemoryMatcher = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;
  tsar::AnalysisSocketInfo *mSocketInfo = nullptr;
  tsar::DIMemoryEnvironment *mDIMEnv = nullptr;
  SmallVector<const tsar::OptimizationRegion *, 4> mRegions;
  AdjacentListT mAdjacentList;
  DenseSet<std::size_t> mExternalCalls;
  // Set of functions and their IDs which are called from parallel loops.
  DenseMap<Function *, std::size_t> mParallelCallees;
};

/// This specifies additional passes which must be run on client.
class ClangSMParallelizationInfo: public tsar::PassGroupInfo {
public:
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
}

#define INITIALIZE_SHARED_PARALLELIZATION(passName, arg, name, info)           \
  INITIALIZE_PASS_IN_GROUP_BEGIN(                                              \
      passName, arg, name, false, false,                                       \
      TransformationQueryManager::getPassRegistry())                           \
  INITIALIZE_PASS_IN_GROUP_INFO(info)                                          \
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)                             \
  INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)                     \
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)                              \
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)                                 \
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)                             \
  INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)                         \
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)                         \
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)                    \
  INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)                             \
  INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)                       \
  INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)                          \
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)                    \
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)                       \
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)                         \
  INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)                     \
  INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)                                 \
  INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)                                \
  INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)                             \
  INITIALIZE_PASS_DEPENDENCY(ClangExprMatcherPass)                             \
  INITIALIZE_PASS_DEPENDENCY(DIArrayAccessWrapper)                             \
  INITIALIZE_PASS_DEPENDENCY(RegionWeightsEstimator)                           \
  INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, false, false,              \
                               TransformationQueryManager::getPassRegistry())
#endif//TSAR_CLANG_SHARED_PARALLEL_H
