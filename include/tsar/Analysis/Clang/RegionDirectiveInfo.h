//=== RegionDirectiveInfo.h - Source-level Region Directive  ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file declares passes to collect '#pragma spf region' directives
// in a source code and construct representation of regions which should be
// analyzed and optimized.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_REGION_DIRECTIVE_INFO_H
#define TSAR_CLANG_REGION_DIRECTIVE_INFO_H

#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Support/Tags.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <memory>
#include <vector>

namespace llvm {
class Function;
class Loop;
class Value;
}

namespace tsar {
/// Representation of an optimization region.
///
/// Optimization region is a set of objects which should be optimized by
/// some passes. For example, it is possible to mark which loops should be
/// parallelized (if possible). In this case other loops will be ignored
/// (even these loops could be executed in parallel.
class OptimizationRegion {
public:
  /// Region may contain part of an object or objects under conditions (for
  /// example only part of function calls).
  enum ContainStatus : uint8_t {
    CS_No = 0,
    // Set if the whole object is not in region, however region contains some
    // of its children (for example, region contains some of loops in a
    // function).
    CS_Child,
    // Set if object is in region if come of conditions is true (for example
    // region may contain only part of function calls).
    CS_Condition,
    // Set if region always contain the whole object.
    CS_Always,
    CS_Invalid
  };

  /// Create region with a specified name.
  explicit OptimizationRegion(llvm::StringRef Name) : mName(Name) {}

  /// Return name of the region.
  llvm::StringRef getName() const { return mName; }

  /// Explicitly mark loop for further optimization.
  ///
  /// Note, that this method does not mark calls from a specified object.
  /// So, mark calls explicitly if it is necessary.
  bool markForOptimization(const llvm::Loop &L);

  /// Explicitly mark an object (function or call) for further optimization.
  ///
  /// Note, that this method does not mark calls from a specified object.
  /// So, mark calls explicitly if it is necessary.
  bool markForOptimization(const llvm::Value &V);

  /// Check whether the region contains a specified function.
  ContainStatus contain(const llvm::Function &F) const {
    auto I = mFunctions.find(&F);
    return I == mFunctions.end() ? CS_No : I->second.Status;
  }

  /// Return true if region contains a specified loop.
  ///
  /// Region may contain loop implicitly if it contains the loop
  /// parent function.
  bool contain(const llvm::Loop &L) const;

  void dump();
private:
  struct ContainInfo {
    ContainInfo(ContainStatus S) : Status(S) {}

    ContainStatus Status;
    llvm::DenseSet<const llvm::Value *> Condition;
  };

  std::string mName;
  /// List of functions related to a region, if the whole function is in region
  /// then the value is true.
  llvm::DenseMap<const llvm::Function *, ContainInfo> mFunctions;
  mutable llvm::DenseSet<ObjectID> mScopes;
};

/// List of all optimization regions.
class OptimizationRegionInfo {
public:
  using RegionPool = std::vector<std::unique_ptr<OptimizationRegion>>;
  using iterator = llvm::pointee_iterator<RegionPool::iterator>;
  using const_iterator = llvm::pointee_iterator<RegionPool::const_iterator>;

  iterator begin() { return iterator(mRegions.begin()); }
  iterator end() { return iterator(mRegions.end()); }

  const_iterator begin() const { return const_iterator(mRegions.begin()); }
  const_iterator end() const { return const_iterator(mRegions.end()); }


  OptimizationRegion *get(llvm::StringRef RegionName) {
    auto Itr = mRegionMap.find(RegionName);
    return Itr != mRegionMap.end() ? Itr->second : nullptr;
  }

  const OptimizationRegion *get(llvm::StringRef RegionName) const {
    auto Itr = mRegionMap.find(RegionName);
    return Itr != mRegionMap.end() ? Itr->second : nullptr;
  }

  bool empty() const { return mRegions.empty(); }
  std::size_t size() const { return mRegions.size(); }

  void clear() {
    mRegionMap.clear();
    mRegions.clear();
  }

  /// Insert new region if it does not exist.
  std::pair<OptimizationRegion *, bool> insert(llvm::StringRef RegionName) {
    bool IsNew = false;
    auto Itr = mRegionMap.find(RegionName);
    if (Itr == mRegionMap.end()) {
      mRegions.emplace_back(std::make_unique<OptimizationRegion>(RegionName));
      Itr = mRegionMap.try_emplace(RegionName, mRegions.back().get()).first;
      IsNew = true;
    }
    return std::make_pair(Itr->second, IsNew);
  }
private:
  RegionPool mRegions;
  llvm::StringMap<OptimizationRegion *> mRegionMap;
};
}

namespace llvm {
/// Collect all '#pragma spf region' directives and construct optimization
/// regions.
class ClangRegionCollector : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangRegionCollector() : ModulePass(ID) {
    initializeClangRegionCollectorPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void releaseMemory() override {
    mRegions.clear();
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  tsar::OptimizationRegionInfo &getRegionInfo() noexcept { return mRegions; }
  const tsar::OptimizationRegionInfo &getRegionInfo() const noexcept {
    return mRegions;
  }

private:
  tsar::OptimizationRegionInfo mRegions;
};
}
#endif//TSAR_CLANG_REGION_DIRECTIVE_INFO_H
