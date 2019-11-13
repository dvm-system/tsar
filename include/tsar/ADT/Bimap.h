//===-- Bimap.h ------------- Bidirectional Map -----------------*- C++ -*-===//
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
// This file implements bidirectional map, each element of this map is a pair.
// Both values in this pair can be treated as keys.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_BIMAP_H
#define TSAR_BIMAP_H

#include <bcl/tagged.h>
#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/ilist.h>
#include <llvm/ADT/ilist_node.h>
#include <type_traits>

namespace tsar {
namespace detail {
/// \brief Provides llvm::DenseMapInfo to implement search in a Bimap.
///
/// To retrieve first and second keys from Bimap two additional containers are
/// used. This containers use BimapInfo which implements llvm::DenseMapInfo for
/// first and second keys. They store values of type ValueTy but allow usage of
/// KeyTy type to search element in the map. The current implementation of each
/// container discussed further.
///
/// Type of data stored in each container is a pointer to a pair of first and
/// second keys. To compute hash of such pointer llvm::DenseMapInfo for first or
/// second key is used and hash is equal to a hash of appropriate key.
///
/// \pre
/// * KeyInfoTy should provide at least two methods:
///   - static unsigned getHashValue(const KeyTy &);
///   - static bool isEqual(const KeyTy &, const KeyTy &);
/// * llvm::DenseMapInfo template should be specialized by ValueTy;
/// * BimapInfo should provide the method
///   static const KeyTy & getKey(const ValueTy &)
template<class BimapInfo, class ValueTy, class KeyTy, class KeyInfoTy>
struct BimapInfoBase {
  static inline ValueTy getEmptyKey() {
    return llvm::DenseMapInfo<ValueTy>::getEmptyKey();
  }

  static inline ValueTy getTombstoneKey() {
    return llvm::DenseMapInfo<ValueTy>::getTombstoneKey();
  }

  static unsigned getHashValue(const ValueTy &Val) {
    return KeyInfoTy::getHashValue(BimapInfo::getKey(Val));
  }
  static unsigned getHashValue(const KeyTy &Val) {
    return KeyInfoTy::getHashValue(Val);
  }

  static bool isEqual(const ValueTy &LHS, const ValueTy &RHS) {
    return llvm::DenseMapInfo<ValueTy>::isEqual(LHS, RHS);
  }

  static bool isEqual(const KeyTy &LHS, const ValueTy &RHS) {
    return !isEqual(RHS, getTombstoneKey()) &&
      !isEqual(RHS, getEmptyKey()) &&
      KeyInfoTy::isEqual(LHS, BimapInfo::getKey(RHS));
  }
};
}

/// \brief Bidirectional associative container, where both values in a pair are
/// treated as keys, which can be retrieved in quadratic time.
///
/// \tparam FTy Type of first object in a pair.
/// \tparam STy Type of first object in a pair.
/// \tparam FirstInfoTy Implementation of traits which is necessary to build
/// hash for the first key.
/// \tparam SecondInfoTy Implementation of traits which is necessary to build
/// hash for the second key.
///
/// Invalidation of iterators, pointers and references referring to elements may
/// occur only when element is removed from the container. But only entities
/// referring the removed element are invalidated. All other iterators, pointers
/// and reference keep their validity.
///
/// It is possible to tag a type of keys stored in this container. It means that
/// is is possible to use user-friendly names to access first and second keys of
/// each element. To use this set FTy and STy as a bcl::tagged<...> template.
///
/// Let us consider an example: the first key represents name of a person and
/// the second key represents its telephone number. To access the Bimap we want
/// to use tags Name and Telephone.
///
/// At first, we must declare this tags:
/// \code
/// struct Name {};
/// struct Telephone {};
/// \endcode
/// Now we can use these tags to declare and access Bimap:
/// \code
/// Bimap<
///   bcl::tagged<std::string, Name>,
///   bcl::tagged<unsigned, Telephone>> Folks;
///
/// Folks.emplace("Ivan", 1234);
/// auto I = Folks.find<Name>("Ivan");
/// std::cout << I->get<Name>() << " has tel. " << I->get<Telephone>() << "\n";
/// Folks.erase<Telephone>(1234)
/// \encode
/// Predefined tags Bimap<...>::First and Bimap<...>::Second are also available.
template<class FTy, class STy,
  class FirstInfoTy = llvm::DenseMapInfo<bcl::add_alias_tagged_t<FTy, FTy>>,
  class SecondInfoTy = llvm::DenseMapInfo<bcl::add_alias_tagged_t<STy, STy>>>
class Bimap {
  /// Type of this bidirectional map.
  typedef Bimap<FTy, STy, FirstInfoTy, SecondInfoTy> Self;

public:
  /// This tag can be used to access first key in a pair.
  struct First {};

  /// This tag can be used to access second key in a pair.
  struct Second {};

private:
  typedef bcl::TypeList<
    bcl::add_alias_tagged<FTy, First>,
    bcl::add_alias_tagged<STy, Second>> Taggeds;
  typedef bcl::get_tagged_t<First, Taggeds> FirstTy;
  typedef bcl::get_tagged_t<Second, Taggeds> SecondTy;
public:
  typedef bcl::tagged_pair<
    bcl::get_tagged<First, Taggeds>,
    bcl::get_tagged<Second, Taggeds>> value_type;
  typedef value_type & reference;
  typedef const value_type & const_reference;
  typedef value_type * pointer;
  typedef const value_type * const_pointer;

  struct BimapNode : public llvm::ilist_node<BimapNode> {
    ~BimapNode() = default;
    BimapNode & operator=(const BimapNode &) = default;
    BimapNode & operator=(BimapNode &&) = default;

    template<class... ArgsTy,
      class = typename std::enable_if<
        std::is_constructible<
          typename Self::value_type, ArgsTy&&...>::value>::type>
    BimapNode(ArgsTy&&... Args) :
      mValue(std::forward<ArgsTy>(Args)...) {}

    typename Self::value_type mValue;
  };

  /// This is a main collection that contains all pairs of elements in the map.
  typedef llvm::ilist<BimapNode> Collection;
  typedef typename Collection::iterator InternalItr;
  typedef typename Collection::const_iterator InternalItrC;
  typedef typename Collection::reverse_iterator InternalItrR;
  typedef typename Collection::const_reverse_iterator InternalItrRC;

  /// Implementation of llvm::DenseMapInfo to access the first key.
  struct BimapFirstInfo : public detail::BimapInfoBase<
    BimapFirstInfo, BimapNode *, FirstTy, FirstInfoTy> {
    static inline const FirstTy & getKey(const BimapNode *Val) noexcept {
      return Val->mValue.first;
    }
  };

  /// This collection is used to consider the first element in a pair as a key.
  typedef llvm::DenseSet<BimapNode *, BimapFirstInfo> FirstToSecondMap;

  /// Implementation of llvm::DenseMapInfo to access the second key.
  struct BimapSecondInfo : public detail::BimapInfoBase<
    BimapSecondInfo, BimapNode *, SecondTy, SecondInfoTy> {
    static inline const SecondTy & getKey(const BimapNode *Val) noexcept {
      return Val->mValue.second;
    }
  };

  /// This collection is used to consider the second element in a pair as a key.
  typedef llvm::DenseSet<BimapNode *, BimapSecondInfo> SecondToFirstMap;

  /// Bidirectional iterator which is a wrapper for internal iterator Itr.
  template<class Itr>  class iterator_wrapper :
    public std::iterator<
        std::bidirectional_iterator_tag, value_type, std::ptrdiff_t,
        const_pointer, const_reference> {
  public:
    typedef typename Self::value_type value_type;
    typedef typename Self::const_pointer pointer;
    typedef typename Self::const_reference reference;

    iterator_wrapper() = default;

    reference operator*() const {return mCurItr->mValue; }
    pointer operator->() const {return &operator*();}

    bool operator==(const iterator_wrapper &RHS) const {
      return mCurItr == RHS.mCurItr;
    }
    bool operator!=(const iterator_wrapper &RHS) const {
      return mCurItr != RHS.mCurItr;
    }

    iterator_wrapper & operator--() { --mCurItr; return *this; }
    iterator_wrapper & operator++() { ++mCurItr; return *this; }
    iterator_wrapper operator--(int) { auto Tmp = *this; --*this; return Tmp; }
    iterator_wrapper operator++(int) { auto Tmp = *this; ++*this; return Tmp; }

  private:
    friend Self;
    iterator_wrapper(typename Itr::pointer NP) : mCurItr(NP) {}
    iterator_wrapper(const Itr &I) : mCurItr(I) {}

    Itr mCurItr;
  };

public:
  typedef typename Collection::size_type size_type;
  typedef iterator_wrapper<InternalItrC> iterator;
  typedef iterator const_iterator;
  typedef iterator_wrapper<InternalItrRC> reverse_iterator;
  typedef reverse_iterator const_reverse_iterator;

  /// Default constructor.
  Bimap() = default;

  /// Copy constructor.
  Bimap(const Bimap &BM) : Bimap(BM.begin(), BM.end()) { }

  /// Move constructor.
  Bimap(Bimap &&BM) :
      mFirstToSecond(std::move(BM.mFirstToSecond)),
      mSecondToFirst(std::move(BM.mSecondToFirst)) {
    mColl.splice(mColl.begin(), BM.mColl);
  }

  /// Constructs the container with the contents of the range [I, EI).
  template<class Itr> Bimap(Itr I, Itr EI) {
    insert(I, EI);
  }

  /// Constructs the container with the contents of the initializer list.
  Bimap(std::initializer_list<value_type> List) {
    for (auto &Val : List)
      insert(Val);
  }

  /// Copy assignment operator. Replaces the contents with a copy of the
  /// contents of other
  Bimap & operator=(const Bimap &BM) {
    if (this == &BM)
      return *this;
    mFirstToSecond.clear();
    mSecondToFirst.clear();
    auto LHS = mColl.begin(), LHSE = mColl.end();
    auto RHS = BM.mColl.begin(), RHSE = BM.mColl.end();
    for (; LHS != LHSE && RHS != RHSE; ++LHS, ++RHS) {
      LHS->mValue = RHS->mValue;
      mFirstToSecond.insert(LHS);
      mSecondToFirst.insert(LHS);
    }
    if (LHS != LHSE)
      mColl.erase(LHS, LHSE);
    else
      insert(iterator(*RHS), iterator(*RHSE));
  }

  /// Move assignment operator. Replaces the contents with those of other using
  /// move semantics
  Bimap & operator=(Bimap &&BM) {
    if (this == &BM)
      return *this;
    mColl.clear();
    mColl.splice(mColl.begin(), BM.mColl);
    mFirstToSecond = std::move(BM.mFirstToSecond);
    mSecondToFirst = std::move(BM.mSecondToFirst);
  }

  /// Replaces the contents with those identified by initializer list.
  Bimap & operator=(std::initializer_list<value_type> List) {
    mFirstToSecond.clear();
    mSecondToFirst.clear();
    auto LHS = mColl.begin(), LHSE = mColl.end();
    auto RHS = List.begin(), RHSE = List.end();
    for (; LHS != LHSE && RHS != RHSE; ++LHS, ++RHS) {
      LHS->mValue = *RHS;
      mFirstToSecond.insert(LHS);
      mSecondToFirst.insert(LHS);
    }
    if (LHS != LHSE)
      mColl.erase(LHS, LHSE);
    else
      insert(RHS, RHSE);
  }

  /// \brief Returns an iterator to the first element of the container.
  ///
  /// If the container is empty, the returned iterator will be equal to end().
  iterator begin() const { return mColl.begin(); }

  /// \brief Returns an iterator to the element following the last element of
  /// the container.
  ///
  /// This element acts as a placeholder; attempting to access it results in
  /// undefined behavior.
  iterator end() const { return mColl.end(); }

  /// \brief Returns an iterator to the first element of the container.
  ///
  /// If the container is empty, the returned iterator will be equal to cend().
  iterator cbegin() const { return begin(); }

  /// \brief Returns an iterator to the element following the last element of
  /// the container.
  ///
  /// This element acts as a placeholder; attempting to access it results in
  /// undefined behavior.
  iterator cend() const { return end(); }

  /// \brief Returns a reverse iterator to the first element of the reversed
  /// container.
  ///
  /// It corresponds to the last element of the non-reversed container.
  /// If the container is empty, the returned iterator will be equal to rend().
  reverse_iterator rbegin() const { return mColl.rbegin(); }

  /// \brief Returns a reverse iterator to the element following the last
  /// element of the reversed container.
  ///
  /// It corresponds to the element preceding the first element of the
  /// non-reversed container.
  /// This element acts as a placeholder; attempting to access it results in
  /// undefined behavior.
  reverse_iterator rend() const { return mColl.rend(); }

   /// \brief Returns a reverse iterator to the first element of the reversed
  /// container.
  ///
  /// It corresponds to the last element of the non-reversed container.
  /// If the container is empty, the returned iterator will be equal to crend().
  reverse_iterator crbegin() const { return rbegin(); }

  /// \brief Returns a reverse iterator to the element following the last
  /// element of the reversed container.
  ///
  /// It corresponds to the element preceding the first element of the
  /// non-reversed container.
  /// This element acts as a placeholder; attempting to access it results in
  /// undefined behavior.
  reverse_iterator crend() const { return rend(); }

  /// Returns true if the container has no elements.
  bool empty() const { return mColl.empty(); }

  /// Returns the number of elements in the container.
  size_type size() const { return mColl.size(); }

  /// Removes all elements from the container.
  void clear() {
    mFirstToSecond.clear();
    mSecondToFirst.clear();
    mColl.clear();
  }

  /// Exchanges the contents of the container with those of other.
  void swap(Self &Other) {
    mColl.swap(Other.mColl);
    mFirstToSecond.swap(Other.mFirstToSecond());
    mSecondToFirst.swap(Other.mSecondToFirst());
  }

  /// \brief Inserts element into the container, if the container doesn't
  /// already contain an element with an equivalent key.
  ///
  /// \return Returns a pair consisting of an iterator to the inserted element
  /// (or to the element that prevented the insertion) and a bool denoting
  /// whether the insertion took place.
  template<typename Pair,
    typename = typename std::enable_if<
      std::is_constructible<value_type, Pair&&>::value>::type>
  std::pair<iterator, bool> insert(Pair&& Val) {
    auto Res = lookup(Val);
    if (!Res.second)
      return Res;
    return insertNode(new BimapNode(std::forward<Pair>(Val)));
  }

  /// Inserts copies of the elements in the initializer list to the container.
  void insert(std::initializer_list<value_type> List) {
    for (auto &Val : List)
      insert(Val);
  }

  /// Inserts elements from range [I, EI).
  template<class Itr>  void insert(Itr I, Itr EI) {
    for ( ; I != EI; ++I)
      insert(*I);
  }

  /// Inserts a new element into the container by constructing it in-place with
  /// the given Args if there is no element with the key in the container.
  template<typename... ArgTy>
  std::pair<iterator, bool> emplace(ArgTy&&... Args) {
    auto Node = new BimapNode(std::forward<ArgTy>(Args)...);
    auto Res = lookup(Node->mValue);
    if (Res.second)
      return insertNode(Node);
    delete Node;
    return Res;
  }

  /// Finds an element with first key equivalent to First.
  iterator find_first(const FirstTy &First) const {
    auto I = mFirstToSecond.find_as(First);
    return I == mFirstToSecond.end() ? end() : iterator(*I);
  }

  /// Finds an element with second key equivalent to Second.
  iterator find_second(const SecondTy &Second) const {
    auto I = mSecondToFirst.find_as(Second);
    return I == mSecondToFirst.end() ? end() : iterator(*I);
  }

  /// Finds an element with a key Tag equivalent to Key.
  template<class Tag,
    class = typename std::enable_if<
      !std::is_void<bcl::get_tagged<Tag, Taggeds>>::value>::type>
  iterator find(const bcl::get_tagged_t<Tag, Taggeds> &Key) const {
    return taggedFindImp(
      Key, std::is_same<
        bcl::get_tagged<First, Taggeds>, bcl::get_tagged<Tag, Taggeds>>());
  }

  /// \brief Removes specified element from the container.
  ///
  /// \return Iterator following the removed element.
  iterator erase(iterator I) {
    assert(I != end() && "Iterator must refer element in the container!");
    auto *Node = const_cast<BimapNode *>(&*I.mCurItr);
    mFirstToSecond.erase(Node);
    mSecondToFirst.erase(Node);
    return InternalItrC(mColl.erase(Node));
  }

  /// \brief Removes the elements in the range [I; EI), which must be
  /// a valid range in *this.
  ///
  /// \return Iterator following the last removed element.
  iterator erase(iterator I, iterator EI) {
    for (; I != EI; ++I)
      erase(I);
    return EI;
  }

  /// \brief Removes the element (if one exists) with the first key equivalent
  /// to First.
  ///
  /// \return True if the element has been found and removed.
  bool erase_first(const FirstTy &First) {
    auto I = mFirstToSecond.find_as(First);
    if (I == mFirstToSecond.end())
      return false;
    auto Node = *I;
    mFirstToSecond.erase(I);
    mColl.erase(InternalItr(Node));
    return true;
  }

  /// \brief Removes the element (if one exists) with the second key equivalent
  /// to Second.
  ///
  /// \return True if the element has been found and removed.
  bool erase_second(const SecondTy &Second) {
    auto I = mSecondToFirst.find_as(Second);
    if (I == mSecondToFirst.end())
      return false;
    auto Node = *I;
    mSecondToFirst.erase(I);
    mColl.erase(InternalItr(Node));
    return true;
  }

  /// \brief Removes the element (if one exists) with the key Tag equivalent to
  /// Key.
  ///
  /// \return True if the element has been found and removed.
  template<class Tag,
    class = typename std::enable_if<
      !std::is_void<bcl::get_tagged<Tag, Taggeds>>::value>::type>
  bool erase(const bcl::get_tagged_t<Tag, Taggeds> &Key) {
    return taggedEraseImp(
      Key, std::is_same<
        bcl::get_tagged<First, Taggeds>, bcl::get_tagged<Tag, Taggeds>>());
  }

private:
  /// \brief Finds element with key equivalent to some of specified keys
  /// (first or second).
  ///
  /// \return A pair comprises iterator referring element that has been found
  /// and FALSE (it an element HAS BEEN found).
  std::pair<iterator, bool> lookup(const value_type &Val) const {
    auto I = find_first(Val.first);
    if (I != end())
      return std::make_pair(I, false);
    I = find_second(Val.second);
    if (I != end())
      return std::make_pair(I, false);
    return std::make_pair(end(), true);
  }

  /// This is supplementary method which make force insertion of the specified
  /// node in all collections that form the Bimap.
  std::pair<iterator, bool> insertNode(BimapNode *Node) {
    mColl.push_back(Node);
    mFirstToSecond.insert(Node);
    mSecondToFirst.insert(Node);
    return std::make_pair(iterator(Node), true);
  }

  /// Overloaded method. Finds an element with first key equivalent to key.
  iterator taggedFindImp(const FirstTy &Key, std::true_type) const {
    return find_first(Key);
  }

  /// Overloaded method. Finds an element with second key equivalent to key.
  iterator taggedFindImp(const SecondTy &Key, std::false_type) const {
    return find_second(Key);
  }

  /// Overloaded method. Removes an element with the specified first key.
  bool taggedEraseImp(const FirstTy &Key, std::true_type) {
    return erase_first(Key);
  }

  /// Overloaded method. Removes an element with the specified second key.
  bool taggedEraseImp(const SecondTy &Key, std::false_type) {
    return erase_second(Key);
  }

  Collection mColl;
  FirstToSecondMap mFirstToSecond;
  SecondToFirstMap mSecondToFirst;
};
}


//#include <alghorithm>

namespace std {
/// Specializes the std::swap algorithm for tsar::Bimap.
template<class FirstTy, class SecondTy, class FirstInfoTy, class SecondInfoTy>
inline void swap(
    tsar::Bimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &LHS,
    tsar::Bimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &RHS) {
  LHS.swap(RHS);
}

/// Compares the contents of two bidirectional maps.
template<class FirstTy, class SecondTy, class FirstInfoTy, class SecondInfoTy>
bool operator==(
    const tsar::Bimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &LHS,
    const tsar::Bimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &RHS) {
  if (&LHS == &RHS)
    return true;
  auto LHSItr = LHS.begin(), LHSEndItr = LHS.end();
  auto RHSItr = RHS.begin(), RHSEndItr = RHS.end();
  for (; LHSItr != LHSEndItr && RHSItr != RHSEndItr; ++LHSItr, ++RHSItr)
    if (LHSItr->first != RHSItr->first || LHSItr->second != RHSItr->second)
      return false;
  if (LHSItr != LHSEndItr || RHSItr != RHSEndItr)
    return false;
  return true;
}

/// Compares the contents of two bidirectional maps.
template<class FirstTy, class SecondTy, class FirstInfoTy, class SecondInfoTy>
bool operator!=(
    const tsar::Bimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &LHS,
    const tsar::Bimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &RHS) {
  return !operator==(LHS, RHS);
}
}
#endif//TSAR_BIMAP_H

