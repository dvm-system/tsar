//===- DIMemoryTrait.h - Memory Analyzable Traits (Metadata) ----*- C++ -*-===//
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
// This file defines metadata-level traits of memory locations which could be
// recognized by the analyzer.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_MEMORY_TRAIT_H
#define TSAR_DI_MEMORY_TRAIT_H

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/ADT/PersistentMap.h"
#include "tsar/ADT/PersistentIteratorInfo.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Analysis/Memory/MemoryTrait.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/Tags.h"
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

namespace tsar {
namespace trait {
/// Metadata-level description of a loop-carried dependence.
class DIDependence : public Dependence {
public:
  /// This represents distance that can be unknown.
  using Distance = llvm::Optional<llvm::APSInt>;

  /// This represents lowest and highest distances.
  using DistanceRange = std::pair<Distance, Distance>;

  /// This represent a distance vectors for a loop nest.
  ///
  /// The first range of distances corresponds to the outermost loop in the
  /// currently analyzed nest. Note, that this outermost loop may contain parent
  /// loops.
  using DistanceVector = llvm::SmallVector<DistanceRange, 1>;

  /// One of causes which imply the dependence.
  using Cause = bcl::tagged_pair<bcl::tagged<ObjectID, ObjectID>,
                                 bcl::tagged<llvm::DebugLoc, llvm::DebugLoc>>;

  /// Creates dependence and set its properties to `F`.
  /// Distances will not be set.
  explicit DIDependence(Flag F,
                        llvm::ArrayRef<Cause> Causes = llvm::ArrayRef<Cause>())
      : Dependence(F | UnknownDistance), mCauses(Causes.begin(), Causes.end()) {
  }

  /// Creates dependence and set its distances and properties.
  /// `UnknownDistance` flag will be updated according to specified distances.
  DIDependence(Flag F, llvm::ArrayRef<DistanceRange> Distances,
               llvm::ArrayRef<Cause> Causes = llvm::ArrayRef<Cause>())
      : Dependence((!Distances.empty() && Distances.front().first &&
                    Distances.front().second)
                       ? F & ~UnknownDistance
                       : F | UnknownDistance),
        mDistances(Distances.begin(), Distances.end()),
        mCauses(Causes.begin(), Causes.end()) {
    mKnownLevel = 0;
    for (auto &DR : Distances)
      if (DR.first && DR.second)
        ++mKnownLevel;
      else
        break;
  }

  /// Return distances.
  DistanceRange getDistance(unsigned Level) const {
    assert(Level < getLevels() && "Distance level is out of range!");
    return mDistances[Level];
  }

  /// Return size of a distance vector.
  unsigned getLevels() const { return mDistances.size(); }

  /// Return number of first known distances.
  unsigned getKnownLevel() const noexcept { return mKnownLevel; }

  /// Return some objects which imply the dependence.
  const auto &getCauses() const noexcept { return mCauses; }

private:
  unsigned mKnownLevel = 0;
  DistanceVector mDistances;
  llvm::SmallVector<Cause, 4> mCauses;
};

/// Metadata-level description of an induction.
class DIInduction {
public:
  /// This represents available kinds of an induction.
  using InductionKind = llvm::InductionDescriptor::InductionKind;

  /// This represents an integer constant which can be unknown;
  using Constant = llvm::Optional<llvm::APSInt>;

  /// Creates description of induction of a specified kind and specifies
  /// start, end and step of induction if possible.
  explicit DIInduction(InductionKind IK, Constant Start = Constant(),
      Constant End = Constant(), Constant Step = Constant()) :
    mIK(IK), mStart(Start), mEnd(End), mStep(Step) {}

  /// Returns kind of induction.
  InductionKind getKind() const noexcept { return mIK; }

  ///  Returns true if kind is valid.
  operator bool() const noexcept { return mIK != InductionKind::IK_NoInduction; }

  /// Returns start value of induction.
  const Constant & getStart() const noexcept { return mStart; }

  /// Returns end value of induction.
  const Constant & getEnd() const noexcept { return mEnd; }

  /// Returns step of induction.
  const Constant & getStep() const noexcept { return mStep; }

private:
  InductionKind mIK;
  Constant mStart;
  Constant mEnd;
  Constant mStep;
};

/// Metadata-level description of a reduction.
using DIReduction = tsar::trait::Reduction;

/// List of locations which covers some other location.
class DICoverage {
public:
  using CoverageT = llvm::SmallVector<WeakDIMemoryHandle, 1 >;
  using iterator = CoverageT::iterator;
  using const_iterator = CoverageT::const_iterator;

  template<class ItrT> DICoverage(ItrT BeginItr, ItrT EndItr) :
    mCoverage(BeginItr, EndItr) {}

  /// Return list of locations which are used to access location.
  llvm::ArrayRef<WeakDIMemoryHandle> getCoverage() const { return mCoverage; }

  iterator begin() { return mCoverage.begin(); }
  iterator end() { return mCoverage.end(); }

  const_iterator begin() const { return mCoverage.begin(); }
  const_iterator end() const { return mCoverage.end(); }

private:
   CoverageT mCoverage;
};
}

/// Correspondence between memory traits and their metadata-level descriptions.
using DIMemoryTraitTaggeds = bcl::TypeList<
  bcl::tagged<trait::DIDependence, trait::Flow>,
  bcl::tagged<trait::DIDependence, trait::Anti>,
  bcl::tagged<trait::DIDependence, trait::Output>,
  bcl::tagged<trait::DIInduction, trait::Induction>,
  bcl::tagged<trait::DIReduction, trait::Reduction>,
  bcl::tagged<trait::DICoverage, trait::Redundant>,
  bcl::tagged<trait::DICoverage, trait::IndirectAccess>>;

/// Set of descriptions of metadata-level memory traits.
using DIMemoryTraitSet = bcl::TraitSet<MemoryDescriptor,
  llvm::SmallDenseMap<bcl::TraitKey, void *, 2>, DIMemoryTraitTaggeds>;

class DIMemoryTraitHandle;
class DIMemoryTrait;

/// This is a set of metadata-level memory traits in a region of a code.
using DIMemoryTraitRegionPool = PersistentMap<
  DIMemoryTraitHandle, DIMemoryTraitSet, DIMemoryMapInfo, DIMemoryTrait>;

/// This removes traits from a set on memory location destruction and changes
/// memory location which is attached to some traits on RAUW.
class DIMemoryTraitHandle final : public CallbackDIMemoryHandle {
public:
  DIMemoryTraitHandle(DIMemory *M, DIMemoryTraitRegionPool *Pool = nullptr) :
    CallbackDIMemoryHandle(M),  mPool(Pool) {}

  DIMemoryTraitHandle & operator=(DIMemory *M) {
    return *this = DIMemoryTraitHandle(M, mPool);
  }

  operator DIMemory * () const {
    return CallbackDIMemoryHandle::operator tsar::DIMemory *();
  }
private:
  friend struct llvm::DenseMapInfo<DIMemoryTrait>;

  void deleted() override;
  void allUsesReplacedWith(DIMemory *M) override;

  DIMemoryTraitRegionPool *mPool;
};

/// This is a set of metadata-level traits for a memory location.
class DIMemoryTrait : public DIMemoryTraitSet {
public:
  DIMemoryTrait(DIMemoryTrait &&) = default;
  DIMemoryTrait & operator=(DIMemoryTrait &&) = default;

  // Delete 'copy' operations because DIMemoryTraitSet can not be copied.
  DIMemoryTrait(const DIMemoryTrait &) = delete;
  DIMemoryTrait & operator=(const DIMemoryTrait &) = delete;

  explicit DIMemoryTrait(const DIMemoryTraitHandle &M) : mMemory(M) {}
  explicit DIMemoryTrait(DIMemoryTraitHandle &&M) : mMemory(std::move(M)) {}

  /// Creates set of traits.
  DIMemoryTrait(const DIMemoryTraitHandle &M, const MemoryDescriptor &Dptr) :
    DIMemoryTraitSet(Dptr), mMemory(M) {}

  /// Creates set of traits.
  DIMemoryTrait(const DIMemoryTraitHandle &M, MemoryDescriptor &&Dptr) :
    DIMemoryTraitSet(std::move(Dptr)), mMemory(M) {}

  DIMemoryTrait(const DIMemoryTraitHandle &M, DIMemoryTrait &&T) :
    DIMemoryTrait(std::move(T)) { mMemory = M; }


  /// Assigns dependency descriptor to this set of traits.
  DIMemoryTrait & operator=(const MemoryDescriptor &Dptr) noexcept {
    DIMemoryTraitSet::operator=(Dptr);
    return *this;
  }

  /// Assigns dependency descriptor to this set traits.
  DIMemoryTrait & operator=(MemoryDescriptor &&Dptr) noexcept {
    DIMemoryTraitSet::operator=(std::move(Dptr));
    return *this;
  }

  /// Returns memory location.
  const DIMemory * getMemory() const { return mMemory; }

  DIMemoryTraitHandle & getFirst() noexcept { return mMemory; }
  const DIMemoryTraitHandle & getFirst() const noexcept { return mMemory; }
  DIMemoryTraitSet & getSecond() noexcept { return *this; }
  const DIMemoryTraitSet & getSecond() const noexcept { return *this; }

private:
  DIMemoryTraitHandle mMemory;
};

/// This is a persistent map from regions of code to a set of memory traits.
using DIMemoryTraitPool = llvm::DenseMap<
  llvm::Metadata *, std::unique_ptr<DIMemoryTraitRegionPool>,
  llvm::DenseMapInfo<llvm::Metadata *>,
  TaggedDenseMapPair<
    bcl::tagged<llvm::Metadata *, Region>,
    bcl::tagged<std::unique_ptr<DIMemoryTraitRegionPool>, Pool>>>;

/// Persistent reference to a metadata-level trait in a pool.
using DIMemoryTraitRef = DIMemoryTraitRegionPool::persistent_iterator;

/// This is representation of llvm::DenseMapInfo for a persistent iterator that
/// points to a memory trait in a persistent map. It uses tsar::DIMemory *
/// to access an element in the set.
struct DIMTraitPersistentSetInfo {
  using Iterator = DIMemoryTraitRef;
  static inline Iterator getEmptyKey() {
    return llvm::DenseMapInfo<Iterator>::getEmptyKey();
  }
  static inline Iterator getTombstoneKey() {
    return llvm::DenseMapInfo<Iterator>::getTombstoneKey();
  }
  static unsigned getHashValue(const Iterator &Val) {
    return llvm::DenseMapInfo<const DIMemory *>::getHashValue(Val->getMemory());
  }
  static unsigned getHashValue(const DIMemoryTrait &Val) {
    return llvm::DenseMapInfo<const DIMemory *>::getHashValue(Val.getMemory());
  }
  static unsigned getHashValue(const DIMemory *Val) {
    return llvm::DenseMapInfo<const DIMemory *>::getHashValue(Val);
  }
  static bool isEqual(const Iterator &LHS, const Iterator &RHS) {
    return LHS == RHS || LHS && RHS && LHS->getMemory() == RHS->getMemory();
  }
  static bool isEqual(const tsar::DIMemoryTrait &LHS, const Iterator &RHS) {
    return RHS && RHS->getMemory() == LHS.getMemory();
  }
  static bool isEqual(const tsar::DIMemory *LHS, const Iterator &RHS) {
    return RHS && RHS->getMemory() == LHS;
  }
};

class DIAliasNode;

/// This is a set of metadata-level traits for an metadata-level alias node.
class DIAliasTrait : public MemoryDescriptor {
  using AccessTraits =
    llvm::SmallDenseSet<DIMemoryTraitRef, 1, DIMTraitPersistentSetInfo>;

public:
  /// This class used to iterate over traits of different memory locations.
  using iterator = AccessTraits::iterator;

  /// This class used to iterate over traits of different memory locations.
  using const_iterator = AccessTraits::const_iterator;

  /// This stores size of a list of explicitly accessed locations.
  using size_type = AccessTraits::size_type;

  /// Creates representation of traits.
  explicit DIAliasTrait(const DIAliasNode *N) : mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Creates representation of traits.
  DIAliasTrait(const DIAliasNode *N, const MemoryDescriptor &Dptr) :
    MemoryDescriptor(Dptr), mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Creates representation of traits.
  DIAliasTrait(const DIAliasNode *N, MemoryDescriptor &&Dptr) :
    MemoryDescriptor(std::move(Dptr)), mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Assigns dependency descriptor to this set of traits.
  DIAliasTrait & operator=(const MemoryDescriptor &Dptr) noexcept {
    MemoryDescriptor::operator=(Dptr);
    return *this;
  }

  /// Assigns dependency descriptor to this set of traits.
  DIAliasTrait & operator=(MemoryDescriptor &&Dptr) noexcept {
    MemoryDescriptor::operator=(std::move(Dptr));
    return *this;
  }

  /// Returns an alias node for which traits is specified.
  const DIAliasNode * getNode() const noexcept { return mNode; }

  /// Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  std::pair<iterator, bool> insert(const DIMemoryTraitRef &LT) {
    return mAccesses.insert(LT);
  }

  /// Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  std::pair<iterator, bool> insert(DIMemoryTraitRef &&LT) {
    return mAccesses.insert(std::move(LT));
  }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed memory locations.
  iterator begin() { return mAccesses.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed memory locations.
  iterator end() { return mAccesses.end(); }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed memory locations.
  const_iterator begin() const { return mAccesses.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed memory locations.
  const_iterator end() const { return mAccesses.end(); }

  /// Returns traits of a specified memory location if it is
  /// explicitly accessed.
  iterator find(const DIMemory *M) {
    assert(M && "Memory must not be null!");
    return mAccesses.find_as(M);
  }

  /// Returns traits of a specified memory location if it is
  /// explicitly accessed.
  const_iterator find(const DIMemory *M) const {
    assert(M && "Estimate memory must not be null!");
    return mAccesses.find_as(M);
  }

  /// Returns number of explicitly accessed memory locations.
  size_type size() const { return mAccesses.size(); }

  /// Returns true if there are no explicitly accessed memory locations.
  bool empty() const { return mAccesses.empty(); }

  /// Removes an explicitly accessed location from the list.
  bool erase(const DIMemory *M) {
    auto I = mAccesses.find_as(M);
    return I != mAccesses.end() ? mAccesses.erase(I), true : false;
  }

  /// Removes all explicitly accessed memory locations from the list.
  void clear() { mAccesses.clear(); }

private:
  const DIAliasNode *mNode;
  AccessTraits mAccesses;
};
}

namespace llvm {
template<> struct DenseMapInfo<tsar::DIAliasTrait> {
  static inline tsar::DIAliasTrait getEmptyKey() {
    return tsar::DIAliasTrait(
      DenseMapInfo<const tsar::DIAliasNode *>::getEmptyKey());
  }
  static inline tsar::DIAliasTrait getTombstoneKey() {
    return tsar::DIAliasTrait(
      DenseMapInfo<const tsar::DIAliasNode *>::getTombstoneKey());
  }
  static unsigned getHashValue(const tsar::DIAliasTrait &Val) {
    return DenseMapInfo<const tsar::DIAliasNode *>::getHashValue(Val.getNode());
  }
  static unsigned getHashValue(const tsar::DIAliasNode *Val) {
    return DenseMapInfo<const tsar::DIAliasNode *>::getHashValue(Val);
  }
  static bool isEqual(const tsar::DIAliasTrait &LHS,
      const tsar::DIAliasTrait &RHS) {
    return LHS.getNode() == RHS.getNode();
  }
  static bool isEqual(const tsar::DIAliasNode *LHS,
      const tsar::DIAliasTrait &RHS) {
    return LHS == RHS.getNode();
  }
};

/// Wrapper to access a pool of metadata-level traits of memory locations.
using DIMemoryTraitPoolWrapper =
  AnalysisWrapperPass<tsar::DIMemoryTraitPool>;
}

namespace tsar {
class DIAliasTree;

/// This is a set of different traits suitable for a region.
using DIDependenceSet = llvm::DenseSet<DIAliasTrait>;
}
#endif//TSAR_DI_MEMORY_TRAIT_H