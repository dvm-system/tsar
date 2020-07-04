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
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Support/PassGroupRegistry.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>

namespace clang {
class ForStmt;
}

namespace tsar {
class DFLoop;
class AnalysisSocketInfo;
class ClangDependenceAnalyzer;
class DIMemoryEnvironment;
class OptimizationRegion;
class TransformationContext;
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
class DFRegionInfoPass;
class LoopMatcherPass;
class LoopInfoWrapperPass;
class ParallelLoopPass;

/// This provider access to function-level analysis results on client.
using ClangSMParallelProvider =
    FunctionPassAAProvider<AnalysisSocketImmutableWrapper, LoopInfoWrapperPass,
                           ParallelLoopPass, CanonicalLoopPass, LoopMatcherPass,
                           DFRegionInfoPass, ClangDIMemoryMatcherPass,
                           ClangPerfectLoopPass, DominatorTreeWrapperPass,
                           PostDominatorTreeWrapperPass, TargetLibraryInfoWrapperPass,
                           EstimateMemoryPass, DIEstimateMemoryPass>;

/// This pass try to insert directives into a source code to obtain
/// a parallel program for a shared memory.
class ClangSMParallelization: public ModulePass, private bcl::Uncopyable{
  struct Preorder {};
  struct ReversePreorder {};
  struct Postorder {};
  struct ReversePostorder {};

  /// Storage for numbers of call graph nodes.
  using CGNodeNumbering = llvm::DenseMap<
    Function *,
    std::tuple<
      std::size_t, std::size_t,
      std::size_t, std::size_t>,
    DenseMapInfo<Function *>,
    tsar::TaggedDenseMapTuple<
      bcl::tagged<Function *, Function>,
      bcl::tagged<std::size_t, Preorder>,
      bcl::tagged<std::size_t, ReversePreorder>,
      bcl::tagged<std::size_t, Postorder>,
      bcl::tagged<std::size_t, ReversePostorder>>>;

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
  virtual bool exploitParallelism(tsar::DFLoop &IR,
    const clang::ForStmt &AST, Function* F,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTDepInfo,
    tsar::TransformationContext &TfmCtx) = 0;

  virtual void optimizeRegions(const Function* F, tsar::TransformationContext& TfmCtx) { };

  /// Perform optimization of parallel loops with a common parent.
  virtual void optimizeLevelLoop(tsar::TransformationContext& TfmCtx, Function& F,
    Loop* L, ClangSMParallelProvider& Provider) { }

  virtual void optimizeLevelFunction(tsar::TransformationContext& TfmCtx, Function& F,
    std::vector<std::pair<const Function*, Instruction*>>& Callees,
    ClangSMParallelProvider& Provider) { }

  virtual void finalize(tsar::TransformationContext& TfmCtx) { };

private:
  /// Initialize provider before on the fly passes will be run on client.
  void initializeProviderOnClient(Module &M);

  /// Check whether it is possible to parallelize a specified loop, analyze
  /// inner loops on failure.
  bool findParallelLoops(Loop &L, Function &F,
                         ClangSMParallelProvider &Provider);

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
  bool findParallelLoops(Loop* L, ItrT I, ItrT EI, Function &F,
                         ClangSMParallelProvider &Provider) {
    bool Parallelized = false;
    for (; I != EI; ++I)
      Parallelized |= findParallelLoops(**I, F, Provider);
    if (Parallelized)
      mParallelLoops.push_back(L);
    return Parallelized;
  }

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
  void optimizeLoops(Loop* L, ItrT I, ItrT EI, Function& F,
    ClangSMParallelProvider& Provider) {
    for (; I != EI; ++I)
      optimizeLoops(*I, (**I).begin(), (**I).end(), F, Provider);
    // if (std::find(mParallelLoops.begin(), mParallelLoops.end(), L) != mParallelLoops.end()) {
      optimizeLevelLoop(*mTfmCtx, F, L, Provider);
    //}
  }

  tsar::TransformationContext *mTfmCtx = nullptr;
  const tsar::GlobalOptions *mGlobalOpts = nullptr;
  tsar::MemoryMatchInfo *mMemoryMatcher = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;
  tsar::AnalysisSocketInfo *mSocketInfo = nullptr;
  tsar::DIMemoryEnvironment *mDIMEnv = nullptr;
  SmallVector<const tsar::OptimizationRegion *, 4> mRegions;
  CGNodeNumbering mCGNodes;
  CGNodeNumbering mParallelCallees;
  std::vector<const Loop*> mParallelLoops;
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
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)                              \
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)                                 \
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)                             \
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)                               \
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
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)                     \
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)                         \
  INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)                     \
  INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)                                \
  INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)                             \
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)                       \
  INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, false, false,              \
                               TransformationQueryManager::getPassRegistry())
#endif//TSAR_CLANG_SHARED_PARALLEL_H
