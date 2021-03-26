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
class ClangTransformationContext;
class DIMemoryEnvironment;
class ParallelItem;
class OptimizationRegion;
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

public:
  ClangSMParallelization(char &ID);

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override {
    mRegions.clear();
    mTfmCtx = nullptr;
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

  /// Prepare level to upward optimization.
  ///
  /// Before the upward optimization levels in a function a traversed downward
  /// to reach innermost levels. So, this function is called when a level
  /// is visited for the first time.
  ///
  /// Return true if it is necessary to optimize this level and traverse
  /// inner levels.
  virtual bool optimizeGlobalIn(PointerUnion<Loop *, Function *> Level,
      const FunctionAnalysis &Provider) {
    return true;
  }

  /// Visit level in upward direction and perform optimization. Return `true`
  /// if optimization was successful.
  ///
  /// If a level has not been optimized, the outer levels will not be also
  /// optimized.
  virtual bool optimizeGlobalOut(PointerUnion<Loop *, Function *> Level,
      const FunctionAnalysis &Provider) {
    return true;
  }

  /// Return analysis results computed on the client for a specified function.
  FunctionAnalysis analyzeFunction(llvm::Function &F);

  llvm::Function *getEntryPoint() noexcept { return mEntryPoint; }

private:
  /// Initialize provider before on the fly passes will be run on client.
  void initializeProviderOnClient();

  /// Check whether it is possible to parallelize a specified loop, analyze
  /// inner loops on failure.
  bool findParallelLoops(Loop &L, const FunctionAnalysis &Provider,
      tsar::ParallelItem *PI);

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
  bool findParallelLoops(PointerUnion<Loop *, Function *> Parent,
      ItrT I, ItrT EI, const FunctionAnalysis &Provider,
      tsar::ParallelItem *PI) {
    auto Parallelized = false;
    for (; I != EI; ++I)
      Parallelized |= findParallelLoops(**I, Provider, PI);
    if (Parallelized)
      optimizeLevel(Parent, Provider);
    return Parallelized;
  }

  bool optimizeUpward(Loop &L, const FunctionAnalysis &Provider);

  template<class ItrT>
  bool optimizeUpward(PointerUnion<Loop *, Function *> Parent,
      ItrT I, ItrT EI, const FunctionAnalysis &Provider, bool Optimize) {
    if (Optimize) {
      // We treat skipped levels as optimized ones.
      if (!optimizeGlobalIn(Parent, Provider))
        return true;
    }
    for (; I != EI; ++I)
      Optimize &= optimizeUpward(**I, Provider);
    if (Optimize)
      Optimize = optimizeGlobalOut(Parent, Provider);
    return Optimize;
  }

  std::size_t buildAdjacentList();

  tsar::ClangTransformationContext *mTfmCtx = nullptr;
  tsar::TransformationInfo *mTfmInfo = nullptr;
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
  llvm::Function *mEntryPoint{nullptr};
};

/// This specifies additional passes which must be run on client.
class ClangSMParallelizationInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
}

#define INITIALIZE_SHARED_PARALLELIZATION(passName, arg, name)                 \
  INITIALIZE_PASS_IN_GROUP_BEGIN(                                              \
      passName, arg, name, false, false,                                       \
      TransformationQueryManager::getPassRegistry())                           \
  INITIALIZE_PASS_IN_GROUP_INFO(ClangSMParallelizationInfo)                    \
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)                             \
  INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)                     \
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)                              \
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)                                 \
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)                             \
  INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)                         \
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)                         \
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)                    \
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
  INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, false, false,              \
                               TransformationQueryManager::getPassRegistry())
#endif//TSAR_CLANG_SHARED_PARALLEL_H
