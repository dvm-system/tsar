//===-- PersistentIterator.h - Persistent Iterator --------------*- C++ -*-===//
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
// This file implements iterators that use in persistent associative containers.
// This file also provides wrapper for user-defined values to store list of
// persistent iterators.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PERSISTENT_ITERATOR_H
#define TSAR_PERSISTENT_ITERATOR_H

#include <assert.h>
#include <type_traits>

namespace llvm {
template<typename T, typename Enable> struct DenseMapInfo;
}

namespace tsar {
template<class PersistentT, class MapT> class PersistentIteratorC;
template<class PersistentT, class MapT> struct PersistentValueWrapper;
template<class PersistentT, class MapT> struct PersistentValueWrapperImpl;

/// \brief This class is used to iterate over all buckets into persistent
/// associative container.
///
/// Note, it is not persistent and it is invalidated whenever insertion into
/// the container occurs. However, it can be converted to a persistent iterator
/// which remains valid if container is changed.
///
/// \tparam IsConst True is used to implement constant iterator.
/// \tparam PersistentT Type of persistent container. It must provide
/// `value_type` type which specified a user-defined bucket.
/// \tparam MapT Type of map which is used to implement a specified persistent
/// container. It must provide `iterator` and `const_iterator` types. Each of
/// this iterators must point to a value which provides getBucket() method
/// to access user-defined bucket.
template<bool IsConst, class PersistentT, class MapT>
class NotPersistentIterator {
  friend PersistentT;
  friend class NotPersistentIterator<true, PersistentT, MapT>;
  friend class NotPersistentIterator<false, PersistentT, MapT>;
  friend class PersistentIteratorC<PersistentT, MapT>;
  using MapIterator = typename std::conditional<IsConst,
    typename MapT::const_iterator, typename MapT::iterator>::type;
  using BucketT = typename PersistentT::value_type;
  using NotPersistentIteratorC = NotPersistentIterator<true, PersistentT, MapT>;
public:
  using difference_type = typename MapIterator::difference_type;
  using value_type = typename std::conditional<IsConst,
    const BucketT, BucketT>::type;
  using pointer = value_type *;
  using reference = value_type &;
  using iterator_category = typename MapIterator::iterator_category;

  NotPersistentIterator() = default;
  NotPersistentIterator(const NotPersistentIterator &) = default;
  NotPersistentIterator(NotPersistentIterator &&) = default;
  NotPersistentIterator & operator=(const NotPersistentIterator &) = default;
  NotPersistentIterator & operator=(NotPersistentIterator &&) = default;

  explicit NotPersistentIterator(const MapIterator &Itr) : mItr(Itr) {}
  template<bool IsConstSrc,
    class = typename std::enable_if<!IsConstSrc && IsConst>::type>
    NotPersistentIterator(
      const NotPersistentIterator<IsConstSrc, PersistentT, MapT> &Itr)
    : mItr(Itr.mItr) {}

  reference operator*() const { return mItr->getBucket(); }
  pointer operator->() const { return &operator*(); }

  bool operator==(const NotPersistentIteratorC &RHS) const {
    return mItr == RHS.mItr;
  }
  bool operator!=(const NotPersistentIteratorC &RHS) const {
    return mItr != RHS.mItr;
  }

  NotPersistentIterator & operator++() { ++mItr; return *this; }
  NotPersistentIterator operator++(int) {
    auto Tmp = *this; ++*this; return Tmp;
  }

private:
  MapIterator mItr;
};

/// This is persistent iterator which remains valid when insertion occurs.
///
/// Note, it can not be used to traversed over the buckets. This iterator
/// can be implicitly created from a `Iterator`.
///
/// \tparam PersistentT Type of persistent container. It must provide
/// `value_type` type which specified a user-defined bucket.
/// \tparam MapT Type of map which is used to implement a specified persistent
/// container. It must provide `iterator` and `const_iterator` types. Each of
/// this iterators must point to a value which provides getBucket() method
/// to access user-defined bucket and getPersistentList() method to access
/// head of a list of persistent iterators. It also must provide `value_type`
/// types which defines PersistentBucket type.
template<class PersistentT, class MapT>
class PersistentIteratorC {
  friend struct PersistentValueWrapper<PersistentT, MapT>;
  friend struct llvm::DenseMapInfo<PersistentIteratorC, void>;
  using NotPersistentIteratorC = NotPersistentIterator<true, PersistentT, MapT>;
protected:
  using PersistentBucket = typename MapT::value_type;
public:
  using value_type = typename NotPersistentIteratorC::value_type;
  using pointer = typename NotPersistentIteratorC::pointer;
  using reference = typename NotPersistentIteratorC::reference;

  PersistentIteratorC(PersistentBucket *Ptr = nullptr) :
    mPtr(Ptr), mNext(nullptr), mPrev(&mNext) {
    // Note, mPrev is set to &mNext to enable remove from list in case of
    // invalid iterator.
  }

  ~PersistentIteratorC() {
    // It is necessary to call removeFromList() hear, however it should be
    // called under condition. If PersistentBucket has been already destroyed
    // it set mPrev to &mNext. This prevents access to destroyed memory.
    // Do not use mPtr == nullptr condition because mPtr is not null for
    // tombstone iterators (see llvm::DenseMapInfo<PersistentIteratorC>).
    if (isValid())
      removeFromList();
  }

  PersistentIteratorC(const PersistentIteratorC &Itr) :
    mPtr(Itr.mPtr) {
    if (Itr)
      addToList();
    else
      invalidate();
  }

  PersistentIteratorC(PersistentIteratorC &&Itr) : mPtr(Itr.mPtr) {
    if (Itr) {
      addToList();
      Itr.removeFromList();
    } else {
      invalidate();
    }
  }

  /// Creates persistent iterator which points to a specified bucket.
  /// Note, that source iterator should not be result of end().
  template<bool IsConst>
  PersistentIteratorC(
    const NotPersistentIterator<IsConst, PersistentT, MapT> &Itr) :
    mPtr(const_cast<PersistentBucket *>(&*Itr.mItr)) {
    addToList();
  }

  PersistentIteratorC & operator=(const PersistentIteratorC &Itr) {
    if (Itr) {
      if (mPtr != Itr.mPtr) {
        mPtr = Itr.mPtr;
        addToList();
      }
    } else {
      // We should distinguish empty and tombstone iterators and we also
      // should allow to compare such invalid operators after assignment.
      mPtr = Itr.mPtr;
      if (isValid())
        removeFromList();
      else
        invalidate();
    }
    return *this;
  }

  PersistentIteratorC & operator=(PersistentIteratorC &&Itr) {
    if (Itr) {
      if (mPtr != Itr.mPtr) {
        mPtr = Itr.mPtr;
        addToList();
      }
      Itr.removeFromList();
    } else {
      // We should distinguish empty and tombstone iterators and we also
      // should allow to compare such invalid operators after assignment.
      mPtr = Itr.mPtr;
      if (isValid())
        removeFromList();
      else
        invalidate();
    }
    return *this;
  }

  /// Updates persistent iterator. So, it will be point to a specified bucket.
  /// Note, that source iterator should not be result of end().
  template<bool IsConst>
  PersistentIteratorC & operator=(
    const NotPersistentIterator<IsConst, PersistentT, MapT> &Itr) {
    if (isValid())
      removeFromList();
    mPtr = const_cast<PersistentBucket *>(&*Itr.mItr);
    addToList();
    return *this;
  }

  reference operator*() const {
    assert(isValid() && "Dereference of invalid persistent iterator!");
    return mPtr->getBucket();
  }

  pointer operator->() const { return &operator*(); }

  bool operator==(const PersistentIteratorC &RHS) const {
    return mPtr == RHS.mPtr;
  }

  bool operator!=(const PersistentIteratorC &RHS) const {
    return mPtr != RHS.mPtr;
  }

  bool isValid() const noexcept { return mPrev != &mNext; }
  operator bool () const noexcept { return isValid(); }

protected:
  PersistentBucket *mPtr;

private:
  void invalidate() noexcept { mPrev = &mNext; }

  void addToList() {
    assert(mPtr && "Persistent reference to a bucket must not be null!");
    PersistentIteratorC **List = mPtr->getPersistentList();
    mNext = *List;
    *List = this;
    mPrev = List;
    if (mNext) {
      mNext->mPrev = &mNext;
      assert(mPtr == mNext->mPtr && "Iterator was added to a wrong list!");
    }
  }

  void removeFromList() {
    *mPrev = mNext;
    if (mNext) {
      assert(mNext->mPrev == &mNext && "List invariant broken!");
      mNext->mPrev = mPrev;
    }
    // Set invariant for nullptr, empty and tombstone iterators.
    invalidate();
  }

  PersistentIteratorC *mNext;
  PersistentIteratorC **mPrev;
};

/// This is persistent iterator which remains valid when insertion occurs.
///
/// Note, it can not be used to traversed over the buckets. This iterator
/// can be implicitly created from a `Iterator`.
template<class PersistentT, class MapT>
class PersistentIterator : public PersistentIteratorC<PersistentT, MapT> {
  using Base = PersistentIteratorC<PersistentT, MapT>;
  using NotPersistentIteratorT = NotPersistentIterator<false, PersistentT, MapT>;
  using PersistentBucket = typename Base::PersistentBucket;
public:
  using value_type = typename NotPersistentIteratorT::value_type;
  using pointer = typename NotPersistentIteratorT::pointer;
  using reference = typename NotPersistentIteratorT::reference;

  PersistentIterator(PersistentBucket *Ptr = nullptr) : Base(Ptr) {}
  PersistentIterator(const PersistentIterator &) = default;
  PersistentIterator(PersistentIterator &&) = default;
  ~PersistentIterator() = default;
  PersistentIterator & operator=(const PersistentIterator &) = default;
  PersistentIterator & operator=(PersistentIterator &&) = default;

  /// Creates persistent iterator which points to a specified bucket.
  /// Note, that source iterator should not be result of end().
  PersistentIterator(const NotPersistentIteratorT &Itr) : Base(Itr) {};

  /// Updates persistent iterator. So, it will be point to a specified bucket.
  /// Note, that source iterator should not be result of end().
  PersistentIterator & operator=(const NotPersistentIteratorT &Itr) {
    Base::operator=(Itr);
    return *this;
  }

  reference operator*() const {
    assert(Base::isValid() && "Dereference of invalid persistent iterator!");
    return Base::mPtr->getBucket();
  }
  pointer operator->() const { return &operator*(); }
};

/// \brief This wrapper contains list of persistent references.
///
/// This class does not change pointer to the PersistentBucket stored
/// in implementation of this wrapper. So, this pointer (ImplT::persistent())
/// can be safely accessed in derived classes.
///
/// Description of PersistentT and MapT is the same as for iterator templates
/// (see above).
template<class PersistentT, class MapT>
struct PersistentValueWrapper {
  using WrapperT = PersistentValueWrapper<PersistentT, MapT>;
  using ImplT = PersistentValueWrapperImpl<PersistentT, MapT>;
  using PersistentIteratorT = PersistentIteratorC<PersistentT, MapT>;
  using ListT = PersistentIteratorT **;

  /// Default constructor.
  PersistentValueWrapper() = default;

  /// Removes persistent iterators from the list.
  ~PersistentValueWrapper() {
    while (mPersistentList)
      mPersistentList->removeFromList();
  }

  /// \brief Copy constructor.
  ///
  /// mPersistentList should not be copied, because persistent references
  /// may points into one map only.
  PersistentValueWrapper(const PersistentValueWrapper &RHS) = default;

  /// Move list of persistent iterators from a specified wrapper into this one.
  PersistentValueWrapper(PersistentValueWrapper &&RHS) :
      mPersistentList(RHS.mPersistentList) {
    RHS.mPersistentList = nullptr;
    if (!mPersistentList)
      return;
    mPersistentList->mPrev = &mPersistentList;
    mPersistentList->mPtr = ImplT::persistent(this);
    for (auto *Curr = mPersistentList->mNext; Curr; Curr= Curr->mNext)
      Curr->mPtr = ImplT::persistent(this);
  }

  /// \brief Copy assignment.
  ///
  /// mPersistentList should not be copied, because persistent references
  /// may points into one map only.
  PersistentValueWrapper & operator=(
    const PersistentValueWrapper &RHS) = default;

  /// Move list of persistent iterators from a specified wrapper into this one.
  PersistentValueWrapper & operator=(PersistentValueWrapper &&RHS) {
    mPersistentList = RHS.mPersistentList;
    RHS.mPersistentList = nullptr;
    if (!mPersistentList)
      return *this;
    mPersistentList->mPrev = &mPersistentList;
    mPersistentList->mPtr = ImplT::persistent(this);
    for (auto *Curr = mPersistentList->mNext; Curr; Curr= Curr->mNext)
      Curr->mPtr = ImplT::persistent(this);
    return *this;
  }

  /// Returns list of persistent iterators.
  ListT getList() const noexcept { return &mPersistentList; }

private:
  mutable PersistentIteratorT *mPersistentList = nullptr;
};

/// \brief This class inherits `PersistentValueWrapper` to store a pointer
/// to a persistent bucket.
///
/// It is not possible to define this pointer into a base class because
/// the pointer is initialized before the base class will be constructed.
/// So, if the base class contains this pointer it will be marked as
/// undefined after a constructor call. This leads to undefined behavior.
/// Note, that PersistentBucketT::getSecond() must return reference to a base
/// class. This means that constructors for `PersistentValueWrapperImpl` will be
/// never called.
///
/// Description of PersistentT and MapT is the same as for iterator templates
/// (see above).
template<class PersistentT, class MapT>
struct PersistentValueWrapperImpl :
    public PersistentValueWrapper<PersistentT, MapT> {
  using WrapperT = PersistentValueWrapper<PersistentT, MapT>;
  using ImplT = PersistentValueWrapperImpl<PersistentT, MapT>;
  using PersistentBucketT = typename MapT::value_type;

  PersistentValueWrapperImpl() = delete;
  ~PersistentValueWrapperImpl() = delete;
  PersistentValueWrapperImpl(const ImplT &) = delete;
  PersistentValueWrapperImpl(ImplT &&) = delete;
  PersistentValueWrapperImpl & operator=(const ImplT &) = delete;
  PersistentValueWrapperImpl & operator=(ImplT &&) = delete;

  /// Returns persistent bucket which contains a specified value wrapper.
  static inline PersistentBucketT * persistent(const WrapperT *Base) noexcept {
    auto Impl = static_cast<const ImplT *>(Base);
    return Impl->PersistentBucket;
  }

  /// Returns reference to the value wrapper which is implemented.
  WrapperT & wrapper() noexcept { return *this; }

  /// Returns reference to the wrapper.
  const WrapperT & wrapper() const noexcept { return *this; }

  mutable PersistentBucketT *PersistentBucket;
};
}

#endif//TSAR_PERSISTENT_ITERATOR_H
