//===-- PersistentSet.h ------- Persistent Set ------------------*- C++ -*-===//
//
//                       Traits Static Analyzer (TSAR)
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
// This file implements a persistent set which is similar to `llvm::DenseSet`
// However, this set provides persistent iterators which are not invalidated
// while the set changes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PERSISTENT_SET_H
#define TSAR_PERSISTENT_SET_H

#include "PersistentIterator.h"
#include <llvm/ADT/DenseMap.h>

namespace tsar {
/// This set is similar to `llvm::DenseSet` but it also provides persistent
/// iterators which are not invalidated while the set changes.
///
/// This class provides iterators of two kinds:
/// - General iterator which is similar to llvm::DenseSet::iterator. This
/// iterator is invalidated whenever the set is changed.
/// - Persistent iterator which is never invalidated (except removal appropriate
/// element from the set). However, it can not be used to iterate over
/// buckets. It can be implicitly constructed from a general iterator.
template<class ValueT, class ValueInfoT = llvm::DenseMapInfo<ValueT>>
class PersistentSet {
  /// This is a persistent bucket which holds list of persistent
  /// references to this bucket alongside with a value.
  class PersistentBucket {
  public:
    struct ValueWrapper final :
      public PersistentValueWrapper<
        PersistentSet,
        llvm::DenseMap<ValueT, ValueWrapper, ValueInfoT, PersistentBucket>> {
      using BaseT = PersistentValueWrapper<
        PersistentSet,
        llvm::DenseMap<ValueT, ValueWrapper, ValueInfoT, PersistentBucket>>;
      using ImplT = typename BaseT::ImplT;
      using ListT = typename BaseT::ListT;
    };

    ValueT & getFirst() noexcept { return Value; }
    const ValueT & getFirst() const noexcept { return Value; }

    ValueWrapper & getSecond() {
      SelfRef.PersistentBucket = this;
      return static_cast<ValueWrapper &>(SelfRef.wrapper());
    }
    const ValueWrapper & getSecond() const {
      SelfRef.PersistentBucket = const_cast<PersistentBucket *>(this);
      return static_cast<const ValueWrapper &>(SelfRef.wrapper());
    }

    ValueT & getBucket() noexcept { return Value; }
    const ValueT & getBucket() const noexcept { return Value; }

    typename ValueWrapper::ListT getPersistentList() const noexcept {
      return SelfRef.getList();
    }

  private:
    ValueT Value;
    typename ValueWrapper::ImplT SelfRef;
  };

  /// Internal `DenseMap` which stores a value and list of
  /// persistent references for an each value.
  using MapT = llvm::DenseMap<ValueT,
    typename PersistentBucket::ValueWrapper, ValueInfoT, PersistentBucket>;

public:
  using size_type = unsigned;
  using key_type = ValueT;
  using value_type = ValueT;

  using iterator = NotPersistentIterator<false, PersistentSet, MapT>;
  using const_iterator = NotPersistentIterator<true, PersistentSet, MapT>;

  using persistent_iterator = PersistentIterator<PersistentSet, MapT>;
  using const_persistent_iterator = PersistentIteratorC<PersistentSet, MapT>;

  ~PersistentSet() = default;
  PersistentSet(const PersistentSet &) = default;
  PersistentSet & operator=(const PersistentSet &) = default;
  PersistentSet(PersistentSet &&Other) = default;
  PersistentSet & operator=(PersistentSet &&Other) = default;

  /// Creates a PersistentSet with an optional \p InitialReserve that guarantee
  /// that this number of elements can be inserted in the set without grow()
  explicit PersistentSet(unsigned InitialReserve = 0) : mMap(InitialReserve) {}

  /// Creates a PersistentSet from a range of values.
  template<typename InputIt>
  PersistentSet(const InputIt &I, const InputIt &E) {
    init(std::distance(I, E));
    insert(I, E);
  }

 /// Returns iterator that points at the beginning of this set.
  iterator begin() { return iterator(mMap.begin()); }

  /// Returns iterator that points at the beginning of this set.
  const_iterator begin() const { return const_iterator(mMap.begin()); }

  /// Returns iterator that points at the ending of this set.
  iterator end() { return iterator(mMap.end()); }

  /// Returns iterator that points at the ending of this set.
  const_iterator end() const { return const_iterator(mMap.end()); }

  /// Returns true if there are not buckets in the set (
  /// except empty or tombstone buckets).
  bool empty() const { return mMap.empty(); }

  /// Returns number of buckets in the set (except empty or tombstone buckets).
  unsigned size() const { return mMap.size(); }

  /// Clears the set. All persistent iterators become invalid.
  void clear() { return mMap.clear(); }

  /// Return 1 if the specified key is in the set, 0 otherwise.
  size_type count(const ValueT &V) const { return mMap.count(V); }

  /// Finds a specified value.
  iterator find(const ValueT &V) { return iterator(mMap.find(V)); }

  /// Finds a specified value.
  const_iterator find(const ValueT &V) const {
    return const_iterator(mMap.find(V));
  }

  /// \brief Alternate version of find() which allows a different, and possibly
  /// less expensive, key type.
  ///
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, KeyT) for each key
  /// type used.
  template<class LookupKeyT>
  iterator find_as(const LookupKeyT &Key) {
    return iterator(mMap.find_as(Key));
  }

  /// \brief Alternate version of find() which allows a different, and possibly
  /// less expensive, key type.
  ///
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, KeyT) for each key
  /// type used.
  template<class LookupKeyT>
  const_iterator find_as(const LookupKeyT &Key) const {
    return const_iterator(mMap.find_as(Key));
  }

  /// Swaps two sets and updates persistent iterators.
  void swap(PersistentSet &RHS) {
    mMap.swap(RHS.mMap);
  }

  /// Creates a copy of a specified set, persistent iterators still point into
  /// the original set.
  void copyFrom(const PersistentSet &Other) {
    mMap->copyFrom(Other.mMap);
  }

  /// Initializes a PersistentSet with an \p InitNumEntries that guarantee
  /// that this number of elements can be inserted in the set without grow().
  void init(unsigned InitNumEntries) {
    mMap->init(InitNumEntries);
  }

  /// Increases the number of elements which can be inserted in the set without
  /// reallocation. Always reallocate the set.
  void grow(unsigned AtLeast) {
    mMap->grow(AtLeast);
  }

  /// Grow the PersistentSet so that it can contain at least \p NumEntries items
  /// before resizing again. Instead of grow() this reallocates set only
  /// if this is necessary.
  void reserve(size_type NumEntries) {
    mMap.reserve(NumEntries);
  }

  /// Removes all elements and reduces the number of buckets. All persistent
  /// iterators become invalid.
  void shrink_and_clear() {
    mMap->shrink_and_clear();
  }

  /// Inserts value into the set if the value isn't already in the set.
  /// If the value is already in the set, it returns false and doesn't update
  /// the value.
  std::pair<iterator, bool> insert(const ValueT &V) {
    auto Pair = mMap.try_emplace(V);
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Inserts value into the set if the value isn't already in the set.
  /// If the value is already in the set, it returns false and doesn't update
  /// the value.
  std::pair<iterator, bool> insert(ValueT &&V) {
    auto Pair = mMap.try_emplace(std::move(V));
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Alternate version of insert() which allows a different, and possibly
  /// less expensive, key type.
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, ValueT) for each key
  /// type used.
  template <typename LookupKeyT>
  std::pair<iterator, bool> insert_as(const ValueT &V, const LookupKeyT &Key) {
    auto Pair = mMap.insert_as(
      {V, typename PersistentBucket::ValueWrapper() }, Key);
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Alternate version of insert() which allows a different, and possibly
  /// less expensive, key type.
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, ValueT) for each key
  /// type used.
  template <typename LookupKeyT>
  std::pair<iterator, bool> insert_as(ValueT &&V, const LookupKeyT &Key) {
    auto Pair = mMap.insert_as(
      {std::move(V), typename PersistentBucket::ValueWrapper() }, Key);
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Range insertion of values.
  template<typename InputIt>
  void insert(InputIt I, InputIt E) {
    for (; I != E; ++I)
      insert(*I);
  }

  /// Erases a specified value if it exists in the set.
  bool erase(const ValueT &V) { return mMap.erase(V); }

  /// Erases an element from the set.
  void erase(iterator I) { mMap.erase(I.mItr); }

  /// Erases an element from the set.
  void erase(persistent_iterator I) { erase(I->getFirst()); }

  /// Erases an element from the set.
  void erase(const_persistent_iterator I) { erase(I->getFirst()); }

  /// Return the approximate size (in bytes) of the actual set.
  /// This is just the raw memory used by PersistentSet.
  /// If entries are pointers to objects, the size of the referenced objects
  /// are not included.
  std::size_t getMemorySize() const { return mMap->getMemorySize(); }

  /// Returns true if the specified pointer points somewhere into the
  /// PersistentSet's array of buckets.
  bool isPointerIntoBucketsArray(const void *Ptr) const {
    return mMap->isPointerIntoBucketsArray(Ptr);
  }

  /// \brief Returns an opaque pointer into the buckets array.
  ///
  /// In conjunction with the previous method, this can be used to
  /// determine whether an insertion caused the PeristentSet to reallocate.
  const void *getPointerIntoBucketsArray() const {
    return mMap->getPointerIntoBucketsArray(); }
private:
  MapT mMap;
};

template<typename ValueT, typename ValueInfoT>
static inline std::size_t capacity_in_bytes(
    const PersistentSet<ValueT, ValueInfoT> &X) {
  return X.getMemorySize();
}
}
#endif//TSAR_PERSISTENT_SET_H
