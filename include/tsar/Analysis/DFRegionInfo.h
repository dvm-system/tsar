//===- DFRegionInfo.h ----- Data-flow Regions Analysis ----------*- C++ -*-===//
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
// This file defines function and classes to build hierarchy of regions for the
// specified region level. This hierarchy is similar to a program structure tree
// but the last one is a more general structure. In contrast, proposed hierarchy
// considers only special type regions.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DF_REGION_INFO_H
#define TSAR_DF_REGION_INFO_H

#include "tsar/Analysis/DataFlowGraph.h"
#include "tsar/Analysis/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Pass.h>

namespace tsar {
/// \brief Builds hierarchy of regions for the specified region level.
///
/// To obtain the whole constructed hierarchy it is necessary to use
/// DFRegionInfoPass.
class DFRegionInfo : private bcl::Uncopyable {
  typedef llvm::DenseMap<const llvm::BasicBlock *, tsar::DFNode *> BBToNodeMap;
public:
  /// Returns outermost region in the hierarchy.
  tsar::DFNode * getTopLevelRegion() const noexcept { return mTopLevelRegion; }

  /// Returns the smallest region that surrounds a specified basic block .
  tsar::DFNode * getRegionFor(const llvm::BasicBlock *BB) const {
    auto I = mBBToNode.find(BB);
    return I == mBBToNode.end() ? nullptr : I->second;
  }

  /// Returns the smallest region that surrounds a specified loop.
  tsar::DFNode * getRegionFor(llvm::Loop *L) const;

  /// Releases memory.
  void releaseMemory() {
    if (mTopLevelRegion) {
      delete mTopLevelRegion;
      mTopLevelRegion = nullptr;
    }
    mBBToNode.clear();
  }

  /// \brief Treats all loops in a function as regions and build the region
  /// hierarchy.
  ///
  /// This function treats a loop nest as a hierarchy of regions. Each region is
  /// an abstraction of an inner loop. Only natural loops will be treated as a
  /// region other loops will be ignored.
  /// \attention Back edges for natural loops will be omitted. If there are no
  /// explicit exists from the loop the following arcs will be added:
  /// - an arc from an entry node of this loop to an exit node of it;
  /// - an arc from region which is associated with this loop in an outer loop
  /// to an exit node of the outer loop.
  void recalculate(llvm::Function &F, llvm::LoopInfo &LpInfo);

  /// \brief Treats all inner loops as regions and build the region hierarchy.
  ///
  /// \copydetails void recalculate(llvm::Function &F, llvm::LoopInfo &LpInfo)
  void recalculate(llvm::Loop &L);

private:
  /// \brief Builds hierarchy of regions for the specified loop nest.
  ///
  /// \tparam LoopReptn Representation of the outermost loop in the nest.
  /// The tsar::LoopTraits class should be specialized by type of each loop in
  /// the nest. For example, the outermost loop can be a loop llvm::Loop * or
  /// a whole function std::pair<llvm::Function *, llvm::LoopInfo *>.
  /// \param [in] L An outermost loop in the nest, it can not be null.
  /// \param [in, out] A region which is associated with the specified loop.
  template<class LoopReptn>
  void buildLoopRegion(LoopReptn L, tsar::DFRegion *R);

  tsar::DFNode *mTopLevelRegion = nullptr;
  BBToNodeMap mBBToNode;
};
}

namespace llvm {
/// This pass buildes hierarchy of data-flow regions.
class DFRegionInfoPass :
  public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DFRegionInfoPass() : FunctionPass(ID) {
    initializeDFRegionInfoPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns hierarcy of region for the last analyzed function
  tsar::DFRegionInfo & getRegionInfo() { return mRegionInfo; }

  /// Returns hierarcy of region for the last analyzed function
  const tsar::DFRegionInfo & getRegionInfo() const { return mRegionInfo; }

  /// Treats all loops in a function as regions and build the region hierarchy.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases memory.
  void releaseMemory() override { mRegionInfo.releaseMemory(); }

private:
  tsar::DFRegionInfo mRegionInfo;
};
}
#endif//TSAR_DF_REGION_PASS_H
