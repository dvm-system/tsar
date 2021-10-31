//===-- ListBimap.h ----- Bidirectional Map of Lists ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (TSAR)
//
// Copyright 2021 DVM System Group
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
// This file implements a bidirectional map, each element of this map is
// a pair of lists. Each element in each list can be treated as a key.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LIST_BIMAP_H
#define TSAR_LIST_BIMAP_H

#include <bcl/tagged.h>
#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/ilist.h>
#include <llvm/ADT/ilist_node.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <type_traits>

namespace tsar {
/// Bidirectional associative container, where both values in a pair are
/// treated as lists of keys, which can be retrieved in quadratic time.
///
/// \tparam FTy Type of an element in the first list in a pair.
/// \tparam STy Type of an element in the second list in a pair.
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
/// Folks.emplace({"Ivan"}, {1234});
/// auto I = Folks.find<Name>("Ivan");
/// std::cout << I->get<Name>() << " has tel. " << I->get<Telephone>() << "\n";
/// Folks.erase<Telephone>(1234)
/// \encode
/// Predefined tags Bimap<...>::First and Bimap<...>::Second are also available.
template<class FTy, class STy,
  class FirstInfoTy = llvm::DenseMapInfo<bcl::add_alias_tagged_t<FTy, FTy>>,
  class SecondInfoTy = llvm::DenseMapInfo<bcl::add_alias_tagged_t<STy, STy>>>
class ListBimap {
  /// Type of this bidirectional map.
  typedef ListBimap<FTy, STy, FirstInfoTy, SecondInfoTy> Self;

public:
  /// This tag can be used to access first key in a pair.
  struct First {};

  /// This tag can be used to access second key in a pair.
  struct Second {};

private:
  using Taggeds = bcl::TypeList<bcl::add_alias_tagged<FTy, First>,
                                bcl::add_alias_tagged<STy, Second>>;
  using FirstTy = bcl::get_tagged_t<First, Taggeds>;
  using SecondTy = bcl::get_tagged_t<Second, Taggeds>;

  using FirstTyVectorTy = llvm::TinyPtrVector<FirstTy>;
  using SecondTyVectorTy = llvm::TinyPtrVector<SecondTy>;

public:
  using value_type = bcl::tagged_pair<
      bcl::add_alias_list_tagged<
          bcl::tagged<FirstTyVectorTy, bcl::get_tagged_tag<First, Taggeds>>,
          bcl::get_tagged_alias<First, Taggeds>>,
      bcl::add_alias_list_tagged<
          bcl::tagged<SecondTyVectorTy, bcl::get_tagged_tag<Second, Taggeds>>,
          bcl::get_tagged_alias<Second, Taggeds>>>;

  using reference = value_type &;
  using const_reference = const value_type &;
  using pointer = value_type *;
  using const_pointer = const value_type *;

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
  using Collection = llvm::ilist<BimapNode>;
  using InternalItr = typename Collection::iterator;
  using InternalItrC = typename Collection::const_iterator;
  using InternalItrR = typename Collection::reverse_iterator;
  using InternalItrRC = typename Collection::const_reverse_iterator;

  /// This collection is used to consider the first element in a pair as a key.
  using FirstToSecondMap = llvm::DenseMap<FirstTy, BimapNode *>;

  /// This collection is used to consider the second element in a pair as a key.
  using SecondToFirstMap = llvm::DenseMap<SecondTy, BimapNode *>;

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
  using size_type = typename Collection::size_type;
  using iterator = iterator_wrapper<InternalItrC>;
  using const_iterator = iterator;
  using reverse_iterator = iterator_wrapper<InternalItrRC>;
  using const_reverse_iterator = reverse_iterator;

  /// Default constructor.
  ListBimap() = default;

  /// Copy constructor.
  ListBimap(const ListBimap &BM) : ListBimap(BM.begin(), BM.end()) { }

  /// Move constructor.
  ListBimap(ListBimap &&BM) :
      mFirstToSecond(std::move(BM.mFirstToSecond)),
      mSecondToFirst(std::move(BM.mSecondToFirst)) {
    mColl.splice(mColl.begin(), BM.mColl);
  }

  /// Constructs the container with the contents of the range [I, EI).
  template<class Itr> ListBimap(Itr I, Itr EI) {
    insert(I, EI);
  }

  /// Constructs the container with the contents of the initializer list.
  ListBimap(std::initializer_list<value_type> List) {
    for (auto &Val : List)
      insert(Val);
  }

  /// Copy assignment operator. Replaces the contents with a copy of the
  /// contents of other
  ListBimap & operator=(const ListBimap &BM) {
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
  ListBimap & operator=(ListBimap &&BM) {
    if (this == &BM)
      return *this;
    mColl.clear();
    mColl.splice(mColl.begin(), BM.mColl);
    mFirstToSecond = std::move(BM.mFirstToSecond);
    mSecondToFirst = std::move(BM.mSecondToFirst);
  }

  /// Replaces the contents with those identified by initializer list.
  ListBimap & operator=(std::initializer_list<value_type> List) {
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
    auto I = mFirstToSecond.find(First);
    return I == mFirstToSecond.end() ? end() : iterator(I->second);
  }

  /// Finds an element with second key equivalent to Second.
  iterator find_second(const SecondTy &Second) const {
    auto I = mSecondToFirst.find(Second);
    return I == mSecondToFirst.end() ? end() : iterator(I->second);
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
    for (auto &V : Node->mValue.first)
      mFirstToSecond.erase(V);
    for (auto &V : Node->mValue.second)
      mSecondToFirst.erase(V);
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
    auto I = mFirstToSecond.find(First);
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
    auto I = mSecondToFirst.find(Second);
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
  /// and FALSE (if an element HAS BEEN found).
  std::pair<iterator, bool> lookup(const value_type &Val) const {
    for (auto &V : Val.first) {
      auto I = find_first(V);
      if (I != end())
        return std::make_pair(I, false);
    }
    for (auto &V : Val.second) {
      auto I = find_second(V);
      if (I != end())
        return std::make_pair(I, false);
    }
    return std::make_pair(end(), true);
  }

  /// This is supplementary method which make force insertion of the specified
  /// node in all collections that form the Bimap.
  std::pair<iterator, bool> insertNode(BimapNode *Node) {
    mColl.push_back(Node);
    for (auto &V: Node->mValue.first)
      mFirstToSecond.try_emplace(V, Node);
    for (auto &V: Node->mValue.second)
      mSecondToFirst.try_emplace(V, Node);
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
    tsar::ListBimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &LHS,
    tsar::ListBimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &RHS) {
  LHS.swap(RHS);
}

/// Compares the contents of two bidirectional maps.
template<class FirstTy, class SecondTy, class FirstInfoTy, class SecondInfoTy>
bool operator==(
    const tsar::ListBimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &LHS,
    const tsar::ListBimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &RHS) {
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
    const tsar::ListBimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &LHS,
    const tsar::ListBimap<FirstTy, SecondTy, FirstInfoTy, SecondInfoTy> &RHS) {
  return !operator==(LHS, RHS);
}
}
#endif//TSAR_LIST_BIMAP_H
