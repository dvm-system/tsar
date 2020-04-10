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

#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Support/PassGroupRegistry.h"
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Pass.h>

namespace clang {
class ForStmt;
}

namespace tsar {
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
class ClangDIMemoryMatcherPass;
class DFRegionInfoPass;
class LoopMatcherPass;
class LoopInfoWrapperPass;
class ParallelLoopPass;

/// This provider access to function-level analysis results on client.
using ClangSMParallelProvider =
    FunctionPassAAProvider<AnalysisSocketImmutableWrapper, LoopInfoWrapperPass,
                           ParallelLoopPass, CanonicalLoopPass, LoopMatcherPass,
                           DFRegionInfoPass, ClangDIMemoryMatcherPass>;

/// This pass try to insert directives into a source code to obtain
/// a parallel program for a shared memory.
class ClangSMParallelization: public ModulePass, private bcl::Uncopyable{
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
  virtual bool exploitParallelism(const Loop &IR, const clang::ForStmt &AST,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTDepInfo,
    tsar::TransformationContext &TfmCtx) = 0;

  /// Perform optimization of parallel loops with a common parent.
  virtual void optimizeLevel(tsar::TransformationContext &TfmCtx) { }

private:
  /// Initialize provider before on the fly passes will be run on client.
  void initializeProviderOnClient(Module &M);

  /// Check whether it is possible to parallelize a specified loop, analyze
  /// inner loops on failure.
  bool findParallelLoops(Loop &L, Function &F,
                         ClangSMParallelProvider &Provider);

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
  bool findParallelLoops(ItrT I, ItrT EI, Function &F,
                         ClangSMParallelProvider &Provider) {
    bool Parallelized = false;
    for (; I != EI; ++I)
      Parallelized |= findParallelLoops(**I, F, Provider);
    if (Parallelized)
      optimizeLevel(*mTfmCtx);
    return Parallelized;
  }

  tsar::TransformationContext *mTfmCtx = nullptr;
  const tsar::GlobalOptions *mGlobalOpts = nullptr;
  tsar::MemoryMatchInfo *mMemoryMatcher = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;
  tsar::AnalysisSocketInfo *mSocketInfo = nullptr;
  tsar::DIMemoryEnvironment *mDIMEnv = nullptr;
  SmallVector<const tsar::OptimizationRegion *, 4> mRegions;
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
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)                       \
  INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, false, false,              \
                               TransformationQueryManager::getPassRegistry())
#endif//TSAR_CLANG_SHARED_PARALLEL_H
