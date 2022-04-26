//===- DIDependencyAnalysis.h - Dependency Analyzer (Metadata) --*- C++ -*-===//
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
// This file defines passes which uses source-level debug information
// to summarize low-level results of privatization, reduction and induction
// recognition and flow/anti/output dependencies exploration.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_DEPENDENCY_ANALYSIS_H
#define TSAR_DI_DEPENDENCY_ANALYSIS_H

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>
#include <forward_list>

namespace llvm {
class DominatorTree;
class LoopInfo;
class PHINode;
class ScalarEvolution;
class MDNode;
}

namespace tsar {
class AliasTree;
class BitMemoryTrait;
class DIAliasMemoryNode;
class DIMemory;
class DependenceSet;
class DFRegionInfo;
template<class GraphType> class SpanningTreeRelation;
struct GlobalOptions;

/// Source-level representation of data-dependencies (including anti/flow/output
/// dependencies, privatizable variables, reductions and inductions).
using DIDependencInfo =
  llvm::DenseMap<llvm::MDNode *, DIDependenceSet,
    llvm::DenseMapInfo<llvm::MDNode *>,
    TaggedDenseMapPair<
      bcl::tagged<llvm::MDNode *, llvm::MDNode>,
      bcl::tagged<DIDependenceSet, DIDependenceSet>>>;
}

namespace llvm {
/// This uses source-level debug information to summarize low-level results of
/// privatization, reduction and induction recognition and flow/anti/output
/// dependencies exploration.
///
/// TODO (kaniandr@gmail.com):
/// (1) Support for hierarchy of promoted locations. For example, in case of
/// structure S with fields S.X and S.Y we should merge traits for each field
/// to obtain traits for the whole structure. At this moment the whole structure
/// and its fields are considered separately this leads to conservative
/// recognition of data dependence.
/// (2) Clarify analysis of corrupted memory locations. For example, in case
/// of P = A (where A is a pointer) P will be substituted in IR-level and marked
/// as corrupted. However, relation with A will be available form metadata-level
/// alias tree (binding locations for a metadata-level alias node).
/// (3) Implement register-based analysis of privitizable variables.
/// (4) Merge traits for memory across RAUW. Sometimes different locations
/// RAUWed to the same location, so traits should be merged accurately.
class DIDependencyAnalysisPass :
  public FunctionPass, private bcl::Uncopyable {

  /// List of memory location traits.
  using TraitList = std::forward_list<bcl::tagged_pair<
    bcl::tagged<const tsar::DIMemory *, tsar::DIMemory>,
    bcl::tagged<tsar::BitMemoryTrait, tsar::BitMemoryTrait>>>;
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  explicit DIDependencyAnalysisPass(bool IsInitialization = false)
      : FunctionPass(ID), mIsInitialization(IsInitialization) {
    initializeDIDependencyAnalysisPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about discovered dependencies.
  tsar::DIDependencInfo & getDependencies() noexcept {return mDeps;}

  /// Returns information about discovered dependencies.
  const tsar::DIDependencInfo &getDependencies() const noexcept {return mDeps;}

  // Summarizes low-level results of privatization, reduction and induction
  // recognition and flow/anti/output dependencies exploration.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases allocated memory.
  void releaseMemory() override {
    mDeps.clear();
    mAT = nullptr;
    mDT = nullptr;
    mSE = nullptr;
    mLiveInfo = nullptr;
    mRegionInfo = nullptr;
    mTLI = nullptr;
  }

  /// Prints out the internal state of the pass. This also used to produce
  /// analysis correctness tests.
  void print(raw_ostream &OS, const Module *M) const override;

private:
  /// Performs analysis of promoted memory locations in a specified loop and
  /// updates description of metadata-level traits in a specified pool if
  /// necessary.
  void analyzePromoted(Loop *L, Optional<unsigned> DWLang,
    const tsar::SpanningTreeRelation<tsar::AliasTree *> &AliasSTR,
    const tsar::SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const tsar::DIMemory *> LockedTraits,
    tsar::DIMemoryTraitRegionPool &Pool);

  /// Determine promoted memory locations which could be privitized in the
  /// original program.
  void analyzePrivatePromoted(Loop *L, Optional<unsigned> DWLang,
    const tsar::SpanningTreeRelation<tsar::AliasTree *> &AliasSTR,
    const tsar::SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const tsar::DIMemory *> LockedTraits,
    tsar::DIMemoryTraitRegionPool &Pool);

  /// Propagate reduction from inner loops if possible.
  ///
  /// Check whether a specified Phi node represents reduction in the loop.
  void propagateReduction(PHINode *Phi, Loop *L, Optional<unsigned> DWLang,
    const tsar::SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const tsar::DIMemory *> LockedTraits,
    tsar::DIMemoryTraitRegionPool &Pool);

  /// Perform analysis of a specified metadata-level alias node.
  ///
  /// Descendant alias node must be already analyzed. This method use results
  /// of IR-level dependence analysis (including variable privatization) and
  /// results of analysis of promoted memory locations.
  void analyzeNode(tsar::DIAliasMemoryNode &DIN, Optional<unsigned> DWLang,
    const tsar::SpanningTreeRelation<tsar::AliasTree *> &AliasSTR,
    const tsar::SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const tsar::DIMemory *> LockedTraits,
    const tsar::GlobalOptions &GlobalOpts, const llvm::Loop &L,
    tsar::DependenceSet &DepSet, tsar::DIDependenceSet &DIDepSet,
    tsar::DIMemoryTraitRegionPool &Pool);

  bool mIsInitialization;
  tsar::DIDependencInfo mDeps;
  tsar::AliasTree *mAT;
  tsar::DIMemoryTraitPool *mTraitPool;
  tsar::LiveMemoryInfo *mLiveInfo;
  tsar::DFRegionInfo *mRegionInfo;
  TargetLibraryInfo *mTLI;
  LoopInfo *mLI;
  DominatorTree *mDT;
  ScalarEvolution *mSE;
};
}
#endif//TSAR_DI_DEPENDENCY_ANALYSIS_H
