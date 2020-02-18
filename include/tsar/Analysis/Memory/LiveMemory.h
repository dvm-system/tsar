//===--- LiveMemory.h ------ Lived Memory Analysis --------------*- C++ -*-===//
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
// This file defines passes to determine live memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LIVE_MEMORY_H
#define TSAR_LIVE_MEMORY_H

#include "tsar/ADT/DataFlow.h"
#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DFMemoryLocation.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class DominatorTree;
}

namespace tsar {
/// Data-flow framework which is used to find live locations
/// for each data-flow regions: basic blocks, loops, functions, etc.
class LiveDFFwk : private bcl::Uncopyable {
public:
  typedef DFValue<LiveDFFwk, MemorySet<MemoryLocationRange>> LiveSet;
  typedef llvm::DenseMap<DFNode *, std::unique_ptr<LiveSet>,
    llvm::DenseMapInfo<DFNode*>,
    tsar::TaggedDenseMapPair<
      bcl::tagged<DFNode *, DFNode>,
      bcl::tagged<std::unique_ptr<LiveSet>, LiveSet>>> LiveMemoryInfo;

  /// This represents results of interprocedural analysis.
  typedef llvm::DenseMap<llvm::Function *, std::unique_ptr<LiveSet>,
    llvm::DenseMapInfo<llvm::Function *>,
    tsar::TaggedDenseMapPair<
    bcl::tagged<llvm::Function *, llvm::Function>,
    bcl::tagged<std::unique_ptr<LiveSet>, LiveSet>>> InterprocLiveMemoryInfo;

  LiveDFFwk(LiveMemoryInfo &LiveInfo, DefinedMemoryInfo &DefInfo,
      const llvm::DominatorTree *DT) :
    mLiveInfo(&LiveInfo), mDefInfo(&DefInfo), mDT(DT) {}
  LiveMemoryInfo & getLiveInfo() noexcept { return *mLiveInfo; }
  const LiveMemoryInfo & getLiveInfo() const noexcept { return *mLiveInfo; }
  DefinedMemoryInfo & getDefInfo() noexcept { return *mDefInfo; }
  const DefinedMemoryInfo & getDefInfo() const noexcept { return *mDefInfo; }
  const llvm::DominatorTree * getDomTree() const noexcept { return mDT; }
private:
  LiveMemoryInfo *mLiveInfo;
  DefinedMemoryInfo *mDefInfo;
  const llvm::DominatorTree *mDT;
};

/// This covers IN and OUT value for a live locations analysis.
typedef LiveDFFwk::LiveSet LiveSet;

/// Representation of live memory analysis results.
typedef LiveDFFwk::LiveMemoryInfo LiveMemoryInfo;

/// Results of inter-procedural live memory analysis for an each function in a
/// call graph.
typedef LiveDFFwk::InterprocLiveMemoryInfo InterprocLiveMemoryInfo;

/// Traits for a data-flow framework which is used to find live locations.
template<> struct DataFlowTraits<LiveDFFwk *> {
  typedef Backward<DFRegion * > GraphType;
  typedef MemorySet<MemoryLocationRange> ValueType;
  static ValueType topElement(LiveDFFwk *, GraphType) { return ValueType(); }
  static ValueType boundaryCondition(LiveDFFwk *DFF, GraphType G) {
    assert(DFF && "Data-flow framework must not be null!");
    auto I = DFF->getLiveInfo().find(G.Graph);
    assert(I != DFF->getLiveInfo().end() && I->get<LiveSet>() &&
      "Data-flow value must be specified!");
    auto &LS = I->get<LiveSet>();
    ValueType V(topElement(DFF, G));
    // If a location is alive before a loop it is alive before each iteration.
    // This occurs due to conservatism of analysis.
    // If a location is alive before iteration with number I then it is alive
    // after iteration with number I-1. So it should be used as a boundary
    // value.
    meetOperator(LS->getIn(), V, DFF, G);
    // If a location is alive after a loop it also should be used as a boundary
    // value.
    meetOperator(LS->getOut(), V, DFF, G);
    return V;
  }
  static void setValue(ValueType V, DFNode *N, LiveDFFwk *DFF) {
    assert(N && "Node must not be null!");
    assert(DFF && "Data-flow framework must not be null!");
    auto I = DFF->getLiveInfo().find(N);
    assert(I != DFF->getLiveInfo().end() && I->get<LiveSet>() &&
      "Data-flow value must be specified!");
    auto & LS = I->get<LiveSet>();
    LS->setIn(std::move(V));
  }
  static const ValueType & getValue(DFNode *N, LiveDFFwk *DFF) {
    assert(N && "Node must not be null!");
    assert(DFF && "Data-flow framework must not be null!");
    auto I = DFF->getLiveInfo().find(N);
    assert(I != DFF->getLiveInfo().end() && I->get<LiveSet>() &&
      "Data-flow value must be specified!");
    auto &LS = I->get<LiveSet>();
    return LS->getIn();
  }
  static void initialize(DFNode *, LiveDFFwk *, GraphType);
  static void meetOperator(
    const ValueType &LHS, ValueType &RHS, LiveDFFwk *, GraphType) {
    RHS.insert(LHS.begin(), LHS.end());
  }
  static bool transferFunction(ValueType, DFNode *, LiveDFFwk *, GraphType);
};

/// Traits for a data-flow framework which is used to find live locations.
template<> struct RegionDFTraits<LiveDFFwk *> :
  DataFlowTraits<LiveDFFwk *> {
  static void expand(LiveDFFwk *, GraphType G) {
    DFNode *LN = G.Graph->getLatchNode();
    if (!LN)
      return;
    DFNode *EN = G.Graph->getExitNode();
    LN->addSuccessor(EN);
    EN->addPredecessor(LN);
  }
  static void collapse(LiveDFFwk *, GraphType G) {
    DFNode *LN = G.Graph->getLatchNode();
    if (!LN)
      return;
    DFNode *EN = G.Graph->getExitNode();
    LN->removeSuccessor(EN);
    EN->removePredecessor(LN);
  }
  typedef DFRegion::region_iterator region_iterator;
  static region_iterator region_begin(GraphType G) {
    return G.Graph->region_begin();
  }
  static region_iterator region_end(GraphType G) {
    return G.Graph->region_end();
  }
};
}

namespace llvm {
class LiveMemoryPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  LiveMemoryPass() : FunctionPass(ID) {
    initializeLiveMemoryPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns results of live memory analysis.
  tsar::LiveMemoryInfo & getLiveInfo() noexcept {
    return mLiveInfo;
  }

  /// Returns results of live memory analysis.
  const tsar::LiveMemoryInfo & getLiveInfo() const noexcept {
    return mLiveInfo;
  }

  /// Executes live memory analysis for a specified function.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases memory.
  void releaseMemory() override { mLiveInfo.clear(); }
private:
  tsar::LiveMemoryInfo mLiveInfo;
};

/// Wrapper to access results of interprocedural live memory analysis.
using GlobalLiveMemoryWrapper =
  AnalysisWrapperPass<tsar::InterprocLiveMemoryInfo>;
}

#endif//TSAR_LIVE_MEMORY_H
