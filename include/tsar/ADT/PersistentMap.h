//===-- PersistentMap.h ------- Persistent Map ------------------*- C++ -*-===//
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
// This file implements a persistent map which is similar to `llvm::DenseMap`
// However, this map provides persistent iterators which are not invalidated
// while the map changes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PERSISTENT_MAP_H
#define TSAR_PERSISTENT_MAP_H

#include "PersistentIterator.h"
#include <llvm/ADT/DenseMap.h>

namespace tsar {
/// This map is similar to `llvm::DenseMap` but it also provides persistent
/// iterators which are not invalidated while the map changes.
///
/// This class provides iterators of two kinds:
/// - General iterator which is similar to llvm::DenseMap::iterator. This
/// iterator is invalidated whenever the map is changed.
/// - Persistent iterator which is never invalidated (except removal appropriate
/// element from the map). However, it can not be used to iterate over
/// buckets. It can be implicitly constructed from a general iterator.
template<class KeyT, class ValueT,
  class KeyInfoT = llvm::DenseMapInfo<KeyT>,
  class BucketT = llvm::detail::DenseMapPair<KeyT, ValueT>>
class PersistentMap {
  /// \brief This is a persistent bucket which holds list of persistent
  /// references to this bucket alongside with key and value pair.
  ///
  /// The `DenseMap` class is used to implement a `PersistentMap`. So, a
  /// bucket must define getFirst() and getSecond() methods to access a key and
  /// a value correspondingly. Note, that `DenseMap` allocates a pool for set of
  /// buckets for the first time (constructors are not called). After that,
  /// it separately call constructors for getFrist() and getSecond() values.
  ///
  /// However, it is necessary to store a list of persistent references
  /// to each bucket. These references should be kept
  /// across changes of a persistent map. The only way is to put this list
  /// into the value. Otherwise, it will not be moved into a new `DenseMap` when
  /// reallocation occurs (only getFirst() and getSecond() will be moved, see
  /// implementation of `DenseMap`). On the other hand, it is convenient
  /// to enable a user specify its own bucket. So, some tricks are applied to
  /// hold a list of persistent references.
  ///
  /// The user bucket is stored as it has been specified by the user. However,
  /// the dense map is not operates with a user value type directly. A special
  /// wrapper is defined. This wrapper holds a pointer to a value from a user
  /// bucket and a list of persistent references. Constructors of this wrapper
  /// forwards data to constructors of a user value using a stored pointer.
  class PersistentBucket {
  public:
    /// This wrapper combines list of persistent references and a user
    /// value into a single object. The PersistentBucket::getSecond() method
    /// provides access to this object.
    struct ValueWrapper final :
      public PersistentValueWrapper<
        PersistentMap,
        llvm::DenseMap<KeyT, ValueWrapper, KeyInfoT, PersistentBucket>> {

      using BaseT = PersistentValueWrapper<
        PersistentMap,
        llvm::DenseMap<KeyT, ValueWrapper, KeyInfoT, PersistentBucket>>;
      using ImplT = typename BaseT::ImplT;
      using ListT = typename BaseT::ListT;

      static inline ValueT * value(const ValueWrapper *Wrapper) {
        auto &Bucket = ImplT::persistent(Wrapper)->getBucket();
        return &Bucket.getSecond();
      }
      template<class... Ts> ValueWrapper(Ts &&... Args) {
        ::new (value(this)) ValueT(std::move(Args)...);
      }
      ~ValueWrapper() { value(this)->~ValueT(); }
      ValueWrapper(const ValueWrapper &RHS) : BaseT(RHS) {
        ::new (value(this)) ValueT(*value(&RHS));
      }
      ValueWrapper(ValueWrapper &&RHS) : BaseT(std::move(RHS)) {
        ::new (value(this)) ValueT(std::move(*value(&RHS)));
      }
      ValueWrapper & operator=(const ValueWrapper &RHS) {
        BaseT::operator=(&RHS);
        *value(this) = *value(&RHS);
        return this;
      }
      ValueWrapper & operator=(ValueWrapper &&RHS) {
        *value(this) = std::move(*value(&RHS));
        BaseT::operator=(std::move(RHS));
        return *this;
      }
    };

    /// Returns user-defined key.
    KeyT & getFirst() { return Bucket.getFirst(); }

    /// Returns user-defined key.
    const KeyT &getFirst() const { return Bucket.getFirst(); }

    /// Returns wrapper which stores user-defined value and a list
    /// of persistent references.
    ValueWrapper & getSecond() {
      SelfRef.PersistentBucket = this;
      return static_cast<ValueWrapper &>(SelfRef.wrapper());
    }

    /// Returns wrapper which stores user-defined value and a list
    /// of persistent references.
    const ValueWrapper & getSecond() const {
      SelfRef.PersistentBucket = const_cast<PersistentBucket *>(this);
      return static_cast<const ValueWrapper &>(SelfRef.wrapper());
    }

    /// Returns a user-defined bucket.
    BucketT & getBucket() noexcept { return Bucket; }

    /// Returns a user-defined bucket.
    const BucketT & getBucket() const noexcept { return Bucket; }

    /// Returns list of persistent references which point to this bucket.
    typename ValueWrapper::ListT getPersistentList() const noexcept {
      return SelfRef.getList();
    }

  private:
    BucketT Bucket;
    typename ValueWrapper::ImplT SelfRef;
  };

  /// Internal `DenseMap` which stores key-value pairs and list of
  /// persistent references for an each pair.
  using MapT = llvm::DenseMap<
    KeyT, typename PersistentBucket::ValueWrapper, KeyInfoT, PersistentBucket>;

public:
  using size_type = unsigned;
  using key_type = KeyT;
  using mapped_type = ValueT;
  using value_type = BucketT;

  using iterator = NotPersistentIterator<false, PersistentMap, MapT>;
  using const_iterator = NotPersistentIterator<true, PersistentMap, MapT>;

  using persistent_iterator = PersistentIterator<PersistentMap, MapT>;
  using const_persistent_iterator = PersistentIteratorC<PersistentMap, MapT>;

  ~PersistentMap() = default;
  PersistentMap(const PersistentMap &) = default;
  PersistentMap & operator=(const PersistentMap &) = default;
  PersistentMap(PersistentMap &&Other) = default;
  PersistentMap & operator=(PersistentMap &&Other) = default;

  /// Creates a PersistentMap with an optional \p InitialReserve that guarantee
  /// that this number of elements can be inserted in the map without grow()
  explicit PersistentMap(unsigned InitialReserve = 0) : mMap(InitialReserve) {}

  /// Creates a PersistentMap from a range of pairs.
  template<typename InputIt>
  PersistentMap(const InputIt &I, const InputIt &E) {
    init(std::distance(I, E));
    insert(I, E);
  }

  /// Returns iterator that points at the beginning of this map.
  iterator begin() { return iterator(mMap.begin()); }

  /// Returns iterator that points at the beginning of this map.
  const_iterator begin() const { return const_iterator(mMap.begin()); }

  /// Returns iterator that points at the ending of this map.
  iterator end() { return iterator(mMap.end()); }

  /// Returns iterator that points at the ending of this map.
  const_iterator end() const { return const_iterator(mMap.end()); }

  /// Returns true if there are not buckets in the map (
  /// except empty or tombstone buckets).
  bool empty() const { return mMap.empty(); }

  /// Returns number of buckets in the map (except empty or tombstone buckets).
  unsigned size() const { return mMap.size(); }

  /// Clears the map. All persistent iterators become invalid.
  void clear() { return mMap.clear(); }

  /// Return 1 if the specified key is in the map, 0 otherwise.
  size_type count(const KeyT &Key) const { return mMap.count(Key); }

  /// Finds a key,value pair with a specified key.
  iterator find(const KeyT &Key) { return iterator(mMap.find(Key)); }

  /// Finds a key,value pair with a specified key.
  const_iterator find(const KeyT &Key) const {
    return const_iterator(mMap.find(Key));
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

  /// Return the entry for the specified key, or a default constructed value if
  /// no such entry exists.
  ValueT lookup(const KeyT &Key) const {
    // Do not use lookup() of mMap because it try to create value which is
    // redefined by PersistentBucket. It should not be created before
    // PersistentBucket::getSecond() will be called. Otherwise dereference
    // of a not-initialized pointer will occur.
    auto I = find(Key);
    return (I == end()) ? ValueT() : I->getSecond();
  }

  /// Swaps two maps and updates persistent iterators.
  void swap(PersistentMap &RHS) {
    mMap.swap(RHS.mMap);
  }

  /// Creates a copy of a specified map, persistent iterators still point into
  /// the original map.
  void copyFrom(const PersistentMap &Other) {
    mMap->copyFrom(Other.mMap);
  }

  /// Initializes a PersistentMap with an \p InitNumEntries that guarantee
  /// that this number of elements can be inserted in the map without grow().
  void init(unsigned InitNumEntries) {
    mMap->init(InitNumEntries);
  }

  /// Increases the number of elements which can be inserted in the map without
  /// reallocation. Always reallocate the map.
  void grow(unsigned AtLeast) {
    mMap->grow(AtLeast);
  }

  /// Grow the PersistentMap so that it can contain at least \p NumEntries items
  /// before resizing again. Instead of grow() this reallocates map only
  /// if this is necessary.
  void reserve(size_type NumEntries) {
    mMap.reserve(NumEntries);
  }

  /// Removes all elements and reduces the number of buckets. All persistent
  /// iterators become invalid.
  void shrink_and_clear() {
    mMap->shrink_and_clear();
  }

  /// Inserts key,value pair into the map if the key isn't already in the map.
  /// If the key is already in the map, it returns false and doesn't update the
  /// value.
  std::pair<iterator, bool> insert(const std::pair<KeyT, ValueT> &KV) {
    return try_emplace(KV.first, KV.second);
  }

  /// Inserts key.value pair into the map if the key isn't already in the map.
  /// If the key is already in the map, it returns false and doesn't update the
  /// value.
  std::pair<iterator, bool> insert(std::pair<KeyT, ValueT> &&KV) {
    return try_emplace(std::move(KV.first), std::move(KV.second));
  }

  /// Range insertion of pairs.
  template<typename InputIt>
  void insert(InputIt I, InputIt E) {
    for (; I != E; ++I)
      insert(*I);
  }

  /// Inserts key,value pair into the map if the key isn't already in the map.
  /// The value is constructed in-place if the key is not in the map, otherwise
  /// it is not moved.
  template<class... Ts>
  std::pair<iterator, bool> try_emplace(const KeyT &Key, Ts &&... Args) {
    auto Pair = mMap.try_emplace(Key, std::forward<Ts>(Args)...);
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Inserts key,value pair into the map if the key isn't already in the map.
  /// The value is constructed in-place if the key is not in the map, otherwise
  /// it is not moved.
  template<class... Ts>
  std::pair<iterator, bool> try_emplace(KeyT &&Key, Ts &&... Args) {
    auto Pair = mMap.try_emplace(std::move(Key), std::forward<Ts>(Args)...);
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Alternate version of insert() which allows a different, and possibly
  /// less expensive, key type.
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, KeyT) for each key
  /// type used.
  template <typename LookupKeyT>
  std::pair<iterator, bool> insert_as(
      std::pair<KeyT, ValueT> &&KV, const LookupKeyT &Val) {
    auto Pair = mMap.insert_as(std::move(KV), Val);
    return std::make_pair(iterator(Pair.first), Pair.second);
  }

  /// Erases an element with a specified key if it exists in the map.
  bool erase(const KeyT &Key) { return mMap.erase(Key); }

  /// Erases an element from the map.
  void erase(iterator I) { mMap.erase(I.mItr); }

  /// Erases an element from the map.
  void erase(persistent_iterator I) { erase(I->getFirst()); }

  /// Erases an element from the map.
  void erase(const_persistent_iterator I) { erase(I->getFirst()); }

  /// Use default constructor to insert a key,value pair if it is not exist yet.
  value_type & FindAndConstruct(const KeyT &Key) {
    return mMap.FindAndConstruct(Key).getBucket();
  }

  /// Returns value with a specified key.
  ///
  /// Use default constructor to insert a key,value pair if it is not exist yet.
  ValueT & operator[](const KeyT &Key) {
    return FindAndConstruct(Key).getSecond();
  }

  /// Use default constructor to insert a key,value pair if it is not exist yet.
  value_type & FindAndConstruct(KeyT &&Key) {
    return mMap.FindAndConstruct(std::move(Key)).getBucket();
  }

  /// Returns value with a specified key.
  ///
  /// Use default constructor to insert a key,value pair if it is not exist yet.
  ValueT & operator[](KeyT &&Key) {
    return FindAndConstruct(std::move(Key)).getSecond();
  }

  /// Return the approximate size (in bytes) of the actual map.
  /// This is just the raw memory used by PersistentMap.
  /// If entries are pointers to objects, the size of the referenced objects
  /// are not included.
  std::size_t getMemorySize() const { return mMap->getMemorySize(); }

  /// Returns true if the specified pointer points somewhere into the
  /// PersistentMap's array of buckets (i.e. either to a key or value in the
  /// PersistentMap).
  bool isPointerIntoBucketsArray(const void *Ptr) const {
    return mMap->isPointerIntoBucketsArray(Ptr);
  }

  /// \brief Returns an opaque pointer into the buckets array.
  ///
  /// In conjunction with the previous method, this can be used to
  /// determine whether an insertion caused the PeristentMap to reallocate.
  const void *getPointerIntoBucketsArray() const {
    return mMap->getPointerIntoBucketsArray(); }
private:

  MapT mMap;
};

template<typename KeyT, typename ValueT, typename KeyInfoT, typename BucketT>
static inline std::size_t capacity_in_bytes(
    const PersistentMap<KeyT, ValueT, KeyInfoT, BucketT> &X) {
  return X.getMemorySize();
}
}
#endif//TSAR_PERSISTENT_MAP_H
