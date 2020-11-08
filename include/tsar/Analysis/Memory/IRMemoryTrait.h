//===--- IRMemoryTrait.h --- Memory Analyzable Trait ------------*- C++ -*-===//
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
// This file defines IR-level traits of memory locations which could be
// recognized by the analyzer.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_IR_MEMORY_TRAIT_H
#define TSAR_IR_MEMORY_TRAIT_H

#include "tsar/Analysis/Memory/MemoryTrait.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>

namespace llvm {
class Instruction;
class SCEV;
}

namespace tsar {
class AliasNode;
class AliasTree;
class EstimateMemory;
class ExplicitAccseeCoverage;

namespace trait {
/// IR-level description of a loop-carried dependence.
class IRDependence : public Dependence {
public:
  /// This represents lowest and highest distances.
  using DistanceRange = std::pair<const llvm::SCEV *, const llvm::SCEV *>;

  /// This represent a distance vectors for a loop nest.
  ///
  /// The first range of distances corresponds to the outermost loop in the
  /// currently analyzed nest. Note, that this outermost loop may contain parent
  /// loops.
  using DistanceVector = llvm::SmallVector<DistanceRange, 3>;

  /// Creates dependence and set its properties to `F`.
  /// Distances will not be set.
  explicit IRDependence(Flag F, llvm::ArrayRef<llvm::Value *> Causes =
                                    llvm::ArrayRef<llvm::Value *>())
      : Dependence(F | UnknownDistance), mKnownLevel(0),
        mCauses(Causes.begin(), Causes.end()) {}

  /// Creates dependence and set its distances and properties.
  /// `UnknownDistance` flag will be updated according to specified distances.
  IRDependence(
      Flag F, llvm::ArrayRef<DistanceRange> Distances,
      llvm::ArrayRef<llvm::Value *> Causes = llvm::ArrayRef<llvm::Value *>())
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

  /// Return some instructions which imply this dependence.
  const auto &getCauses() const noexcept { return mCauses; }
private:
  unsigned mKnownLevel = 0;
  DistanceVector mDistances;
  llvm::SmallVector<llvm::Value *, 4> mCauses;
};
}

/// Correspondence between memory traits and their IR-level descriptions.
using MemoryTraitTaggeds = bcl::TypeList<
  bcl::tagged<trait::IRDependence, trait::Flow>,
  bcl::tagged<trait::IRDependence, trait::Anti>,
  bcl::tagged<trait::IRDependence, trait::Output>>;

/// Set of descriptions of IR-level memory traits.
using MemoryTraitSet = bcl::TraitSet<MemoryDescriptor,
  llvm::SmallDenseMap<bcl::TraitKey, void *, 2>, MemoryTraitTaggeds>;

/// \brief This is a set of traits for a memory location.
///
/// In general this class represents traits of locations which has been
/// collected by an external structure, so it is not possible to modify
/// this locations.
template<class MemoryTy, class BaseTy>
class MemoryTrait : public BaseTy {
public:
  /// Creates set of traits.
  explicit MemoryTrait(MemoryTy Loc) : mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Creates set of traits.
  MemoryTrait(MemoryTy Loc, const MemoryDescriptor &Dptr) :
    BaseTy(Dptr), mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Creates set of traits.
  MemoryTrait(MemoryTy Loc, MemoryDescriptor &&Dptr) :
    BaseTy(std::move(Dptr)), mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Assigns dependency descriptor to this set of traits.
  MemoryTrait & operator=(const MemoryDescriptor &Dptr) noexcept {
    BaseTy::operator=(Dptr);
    return *this;
  }

  /// Assigns dependency descriptor to this set traits.
  MemoryTrait & operator=(MemoryDescriptor &&Dptr) noexcept {
    BaseTy::operator=(std::move(Dptr));
    return *this;
  }

  /// Returns memory location.
  MemoryTy getMemory() const noexcept { return mLoc; }


  /// These methods are necessary to use this class as bucket type in a map.
  MemoryTy & getFirst() noexcept { return mLoc; }
  const MemoryTy & getFirst() const noexcept { return mLoc; }
  BaseTy & getSecond() noexcept { return *this; }
  const BaseTy & getSecond() const noexcept { return *this; }

private:
  MemoryTy mLoc;
};

/// \brief A set of traits of estimate memory locations.
///
/// Note, that an object of `EsimateMemoryTrait` can not be copied (it can be
/// moved only), because `bcl::TraitSet` supports 'move' operations only.
using EstimateMemoryTrait =
  MemoryTrait<const EstimateMemory *, MemoryTraitSet>;

/// A set of traits of unknown memory locations.
using UnknownMemoryTrait =
  MemoryTrait<const llvm::Instruction *, MemoryDescriptor>;

/// \brief This is a set of traits for an alias node.
///
/// In general this class represents traits of alias nodes which has been
/// collected by an alias tree, so it is not possible to modify this nodes.
///
/// For a node a number of accessed estimate memory locations are available.
/// Each of these locations either is explicitly accessed in the analyzed region
/// or covers some of explicitly accessed locations. There are also traits of
/// each such location. Conservative combination of these traits leads to
/// the proposed traits of a node. Nodes that explicitly accessed locations may
/// be associated with some of descendant alias nodes of the current one.
class AliasTrait : public MemoryDescriptor {
  /// List of explicitly accessed estimate memory locations and their traits.
  using AccessTraits = llvm::SmallDenseMap<
    const EstimateMemory *, MemoryTraitSet, 1,
    llvm::DenseMapInfo<const EstimateMemory *>, EstimateMemoryTrait>;

  /// List of explicitly accessed unknown memory locations and their traits.
  using UnknownTraits = llvm::DenseMap<
    const llvm::Instruction *, MemoryDescriptor,
    llvm::DenseMapInfo<const llvm::Instruction *>, UnknownMemoryTrait>;

public:
  /// This class used to iterate over traits of different estimate locations.
  using iterator = AccessTraits::iterator;

  /// This class used to iterate over traits of different estimate locations.
  using const_iterator = AccessTraits::const_iterator;

  /// This class used to iterate over traits of different unknown locations.
  using unknown_iterator = UnknownTraits::iterator;

  /// This class used to iterate over traits of different unknown locations.
  using const_unknown_iterator = UnknownTraits::const_iterator;

  /// This stores size of a list of explicitly accessed locations.
  using size_type = AccessTraits::size_type;

  AliasTrait(AliasTrait &&) = default;
  AliasTrait & operator=(AliasTrait &&) = default;

  AliasTrait(const AliasTrait &) = delete;
  AliasTrait & operator=(const AliasTrait &) = delete;

  /// Creates representation of traits.
  explicit AliasTrait(const AliasNode *N) : mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Creates representation of traits.
  AliasTrait(const AliasNode *N, const MemoryDescriptor &Dptr) :
    MemoryDescriptor(Dptr), mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Creates representation of traits.
  AliasTrait(const AliasNode *N, MemoryDescriptor &&Dptr) :
    MemoryDescriptor(std::move(Dptr)), mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Assigns dependency descriptor to this set of traits.
  AliasTrait & operator=(const MemoryDescriptor &Dptr) noexcept {
    MemoryDescriptor::operator=(Dptr);
    return *this;
  }

  /// Assigns dependency descriptor to this set of traits.
  AliasTrait & operator=(MemoryDescriptor &&Dptr) noexcept {
    MemoryDescriptor::operator=(std::move(Dptr));
    return *this;
  }

  /// Returns an alias node for which traits is specified.
  const AliasNode * getNode() const noexcept { return mNode; }

  /// Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  std::pair<iterator, bool> insert(EstimateMemoryTrait &&LT) {
    return mAccesses.try_emplace(
      std::move(LT.getFirst()), std::move(LT.getSecond()));
  }

  /// Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  std::pair<unknown_iterator, bool> insert(const UnknownMemoryTrait &LT) {
    return mUnknowns.insert(std::make_pair(LT.getFirst(), LT.getSecond()));
  }

  /// Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  std::pair<unknown_iterator, bool> insert(UnknownMemoryTrait &&LT) {
    return mUnknowns.try_emplace(
      std::move(LT.getFirst()), std::move(LT.getSecond()));
  }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed estimate locations.
  iterator begin() { return mAccesses.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed estimate locations.
  iterator end() { return mAccesses.end(); }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed estimate locations.
  const_iterator begin() const { return mAccesses.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed estimate locations.
  const_iterator end() const { return mAccesses.end(); }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed unknown locations.
  unknown_iterator unknown_begin() { return mUnknowns.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed unknown locations.
  unknown_iterator unknown_end() { return mUnknowns.end(); }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed unknown locations.
  const_unknown_iterator unknown_begin() const { return mUnknowns.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed unknown locations.
  const_unknown_iterator unknown_end() const { return mUnknowns.end(); }

  /// Returns traits of a specified estimate memory location if it is
  /// explicitly accessed.
  iterator find(const EstimateMemory *EM) {
    assert(EM && "Estimate memory must not be null!");
    return mAccesses.find(EM);
  }

  /// Returns traits of a specified estimate memory location if it is
  /// explicitly accessed.
  const_iterator find(const EstimateMemory *EM) const {
    assert(EM && "Estimate memory must not be null!");
    return mAccesses.find(EM);
  }

  /// Returns traits of a specified unknown memory location if it is
  /// explicitly accessed.
  unknown_iterator find(const llvm::Instruction *Inst) {
    assert(Inst && "Unknown memory must not be null!");
    return mUnknowns.find(Inst);
  }
  /// Returns traits of a specified unknown memory location if it is
  /// explicitly accessed.
  const_unknown_iterator find(const llvm::Instruction *Inst) const {
    assert(Inst && "Unknown memory must not be null!");
    return mUnknowns.find(Inst);
  }

  /// Returns number of explicitly accessed estimate locations.
  size_type count() const { return mAccesses.size(); }

  /// Returns number of explicitly accessed unknown locations.
  size_type unknown_count() const { return mUnknowns.size(); }

  /// Returns true if there are no explicitly accessed estimate locations.
  bool empty() const { return mAccesses.empty(); }

  /// Returns true if there are no explicitly accessed unknown locations.
  bool unknown_empty() const { return mUnknowns.empty(); }

  /// Removes an explicitly accessed location from the list.
  bool erase(const EstimateMemory *EM) { return mAccesses.erase(EM); }

  /// Removes an explicitly accessed location from the list.
  void erase(iterator I) { mAccesses.erase(I); }

  /// Removes an explicitly accessed location from the list.
  bool erase(const llvm::Instruction *Inst) { return mUnknowns.erase(Inst); }

  /// Removes an explicitly accessed location from the list.
  void erase(unknown_iterator I) { mUnknowns.erase(I); }

  /// Removes all explicitly accessed estimate locations from the list.
  void clear() { mAccesses.clear(); }

  /// Removes all explicitly accessed unknown locations from the list.
  void unknown_clear() { mUnknowns.clear(); }

private:
  const AliasNode *mNode;
  AccessTraits mAccesses;
  UnknownTraits mUnknowns;
};

/// This is a set of different traits suitable for a region.
class DependenceSet {
  using AliasTraits =
    llvm::DenseMap<const AliasNode *, AliasTrait>;
public:
  /// This class used to iterate over traits of different alias nodes.
  class const_iterator :
    public std::iterator<std::forward_iterator_tag, AliasTrait> {
  public:
    const_iterator() = default;
    const_iterator(const const_iterator &) = default;
    const_iterator(const_iterator &&) = default;
    const_iterator & operator=(const const_iterator &) = default;
    const_iterator & operator=(const_iterator &&) = default;
    explicit const_iterator(const AliasTraits::const_iterator &I) :
      mCurrItr(I) {}
    explicit const_iterator(AliasTraits::const_iterator &&I) :
      mCurrItr(std::move(I)) {}
    const AliasTrait & operator*() const { return mCurrItr->second; }
    const AliasTrait * operator->() const { return &operator*(); }
    bool operator==(const const_iterator &RHS) const {
      return mCurrItr == RHS.mCurrItr;
    }
    bool operator!=(const const_iterator &RHS) const {
      return !operator==(RHS);
    }
    iterator & operator++() { ++mCurrItr; return *this; }
    iterator operator++(int) { iterator Tmp = *this; ++*this; return Tmp; }
  private:
    AliasTraits::const_iterator mCurrItr;
  };

  /// This type used to iterate over traits of different alias nodes.
  class iterator : public const_iterator {
  public:
    iterator() = default;
    iterator(const iterator &) = default;
    iterator(iterator &&) = default;
    iterator & operator=(const iterator &) = default;
    iterator & operator=(iterator &&) = default;
    explicit iterator(const AliasTraits::const_iterator &I) :
      const_iterator(I) {}
    explicit iterator(AliasTraits::const_iterator &&I) :
      const_iterator(std::move(I)) {}
    AliasTrait & operator*() const {
      return const_cast<AliasTrait &>(const_iterator::operator*());
    }
    AliasTrait * operator->() const { return &operator*(); }
    bool operator==(const const_iterator &RHS) const {
      return const_iterator::operator==(RHS);
    }
    bool operator!=(const const_iterator &RHS) const {
      return !operator==(RHS);
    }
    iterator & operator++() { const_iterator::operator++(); return *this; }
    iterator operator++(int) { iterator Tmp = *this; ++*this; return Tmp; }
  };

  /// Creates set of traits.
  DependenceSet() = default;

  /// Returns iterator that points to the beginning of the traits list.
  iterator begin() { return iterator(mTraits.begin()); }

  /// Returns iterator that points to the ending of the traits list.
  iterator end() { return iterator(mTraits.end()); }

  /// Returns iterator that points to the beginning of the traits list.
  const_iterator begin() const { return const_iterator(mTraits.begin()); }

  /// Returns iterator that points to the ending of the traits list.
  const_iterator end() const { return const_iterator(mTraits.end()); }

  /// Finds traits of a specified alias node.
  iterator find_as(const AliasNode *N) const {
    return iterator(mTraits.find(N));
  }

  /// Inserts traits of a specified alias node.
  std::pair<iterator, bool> insert(
    const AliasNode *N, const MemoryDescriptor &Dptr) {
    auto Pair = mTraits.try_emplace(N, N, Dptr);
    return std::make_pair(iterator(std::move(Pair.first)), Pair.second);
  }

  /// Inserts traits of a specified alias node.
  std::pair<iterator, bool> insert(
    const AliasNode *N, MemoryDescriptor &&Dptr) {
    auto Pair = mTraits.try_emplace(N, N, std::move(Dptr));
    return std::make_pair(iterator(std::move(Pair.first)), Pair.second);
  }

  /// Erases traits of a specified alias node.
  bool erase(const AliasNode *N) { return mTraits.erase(N); }

  /// Erases traits of all alias nodes.
  void clear() { mTraits.clear(); }

  /// Returns true if there are no alias nodes with known traits.
  bool empty() const { return mTraits.empty(); }

  /// Returns number of alias nodes with known traits.
  unsigned size() const { return mTraits.size(); }

private:
  AliasTraits mTraits;
};
}

#endif//TSAR_IR_MEMORY_TRAIT_H
