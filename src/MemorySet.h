//===--- MemorySet.h --------- Memory Location Set --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines storage of objects which represent memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_SET_H
#define TSAR_MEMORY_SET_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Value.h>
#include "MemorySetInfo.h"

namespace tsar {
/// \brief This implements a set of memory locations.
///
/// Methods of this class do not use alias information. Consequently,
/// two locations may overlap if they have identical address of beginning.
/// \note This class manages memory allocation to store elements of
/// a location set.
template<class LocationTy, class MemoryInfo = MemorySetInfo<LocationTy>>
class MemorySet {
  template<class Ty, class Info> friend class MemorySet;

  /// Map from pointers to locations.
  typedef llvm::DenseMap<const llvm::Value *, LocationTy *> MapTy;
public:
  /// \brief Calculates the difference between two sets of locations.
  ///
  /// The result set will contain locations from the first set which are not
  /// overlapped with any locations from the second set.
  /// \param [in] LocBegin Iterator that points to the beginning of
  /// the first locations set.
  /// \param [in] LocEnd Iterator that points to the ending of
  /// the first locations set.
  /// \param [in] LocSet The second location set.
  /// \param [out] Result It contains the result of this operation.
  /// The following operation should be provided:
  /// - void ResultSet::insert(const LocationTy &)
  /// - void ResultSet::insert(location_iterator &, location_iterator &)
  template<class location_iterator, class ResultSet>
  static void difference(
    const location_iterator &LocBegin, const location_iterator &LocEnd,
    const MemorySet &LocSet, ResultSet &Result) {
    if (LocSet.mLocations.empty())
      Result.insert(LocBegin, LocEnd);
    for (location_iterator I = LocBegin; I != LocEnd; ++I)
      if (!LocSet.overlap(*I))
        Result.insert(*I);
  }

  /// This implements iterator over all memory locations in a set.
  template<class map_iterator, class value_type> class LocationItr :
    public std::iterator<std::forward_iterator_tag, value_type> {
  public:
    explicit LocationItr(const map_iterator &I) : mCurItr(I) {}

    bool operator==(const LocationItr &RHS) const {
      return mCurItr == RHS.mCurItr;
    }

    bool operator!=(const LocationItr &RHS) const {
      return !operator==(RHS);
    }

    value_type & operator*() const { return *(mCurItr->second); }

    value_type * operator->() const { return &operator*(); }

    /// Preincrement
    LocationItr & operator++() { ++mCurItr; return *this; }

    /// Postincrement
    LocationItr operator++(int) {
      auto tmp = *this; ++*this; return tmp;
    }

  private:
    map_iterator mCurItr;
  };

  /// This type used to iterate over all locations in this set.
  using iterator = LocationItr<typename MapTy::iterator, LocationTy>;

  /// This type used to iterate over all locations in this set.
  using const_iterator =
    LocationItr<typename MapTy::const_iterator, const LocationTy>;

  /// Default constructor.
  MemorySet() {}

  /// Destructor.
  ~MemorySet() { clear(); }

  /// Move constructor.
  MemorySet(MemorySet &&that) :
    mLocations(std::move(that.mLocations)) {}

  /// Copy constructor.
  MemorySet(const MemorySet &that) {
    insert(that.begin(), that.end());
  }

  /// Move assignment operator.
  MemorySet & operator=(MemorySet &&that) {
    if (this != &that) {
      clear();
      mLocations = std::move(that.mLocations);
    }
    return *this;
  }

  /// Copy assignment operator.
  MemorySet & operator=(const MemorySet &that) {
    if (this != &that) {
      clear();
      insert(that.begin(), that.end());
    }
    return *this;
  }

  /// Returns iterator that points to the beginning of locations.
  iterator begin() { return iterator(mLocations.begin()); }

  /// Returns iterator that points to the ending of locations.
  iterator end() { return iterator(mLocations.end()); }

  /// Returns iterator that points to the beginning of locations.
  const_iterator begin() const { return const_iterator(mLocations.begin()); }

  /// Returns iterator that points to the ending of locations.
  const_iterator end() const { return const_iterator(mLocations.end()); }

  /// \brief Returns a location which contains the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) and getPtr(Ty) for each location type used.
  template<class Ty> iterator findContaining(const Ty &Loc) {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    return (I != mLocations.end() &&
        MemoryInfo::getSize(Loc) <= MemoryInfo::getSize(*I->second) &&
        MemoryInfo::getAATags(Loc) == MemoryInfo::getAATags(*I->second)) ?
      iterator(I) : iterator(mLocations.end());
  }

  /// \brief Returns a location which contains the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) and getPtr(Ty) for each location type used.
  template<class Ty> const_iterator findContaining(const Ty &Loc) const {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    return (I != mLocations.end() &&
        MemoryInfo::getSize(Loc) <= MemoryInfo::getSize(*I->second) &&
        MemoryInfo::getAATags(Loc) == MemoryInfo::getAATags(*I->second)) ?
      const_iterator(I) : const_iterator(mLocations.end());
  }

  /// \brief Returns true if there is a location in this set which contains
  /// the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) and getPtr(Ty) for each location type used.
  template<class Ty> bool contain(const Ty &Loc) const {
    return findContaining(Loc) != end();
  }

  /// \brief Returns a location which is contained in the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) and getPtr(Ty) for each location type used.
  template<class Ty> iterator findCoveredBy(const Ty &Loc) {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    return (I != mLocations.end() &&
        MemoryInfo::getSize(Loc) >= MemoryInfo::getSize(*I->second) &&
        MemoryInfo::getAATags(Loc) == MemoryInfo::getAATags(*I->second)) ?
      iterator(I) : iterator(mLocations.end());
  }

  /// \brief Returns a location which is contained in the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) and getPtr(Ty) for each location type used.
  template<class Ty> const_iterator findCoveredBy(const Ty &Loc) const {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    return (I != mLocations.end() &&
        MemoryInfo::getSize(Loc) >= MemoryInfo::getSize(*I->second) &&
        MemoryInfo::getAATags(Loc) == MemoryInfo::getAATags(*I->second)) ?
      const_iterator(I) : const_iterator(mLocations.end());
  }

  /// \brief Returns true if there is a location in this set which is contained
  /// in the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) and getPtr(Ty) for each location type used.
  template<class Ty> bool cover(const Ty &Loc) const {
    return findCoveredBy(Loc) != end();
  }

  /// \brief Returns a location which may overlap with the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods getPtr(Ty)
  /// for each location type used.
  template<class Ty> iterator findOverlappedWith(const Ty &Loc) {
    return iterator(mLocations.find(MemoryInfo::getPtr(Loc)));
  }

  /// \brief Returns a location which may overlap with the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods getPtr(Ty)
  /// for each location type used.
  template<class Ty> const_iterator findOverlappedWith(const Ty &Loc) const {
    return const_iterator(mLocations.find(MemoryInfo::getPtr(Loc)));
  }

  /// \brief Returns true if there is a location in this set which may overlap
  /// with the specified location.
  ///
  /// The MemorySetInfo is responsible for supplying methods getPtr(Ty)
  /// for each location type used.
  template<class Ty> bool overlap(const Ty &Loc) const {
    return findOverlappedWith(Loc) != end();
  }

  /// Returns true if this set does not contain any location.
  bool empty() const { return mLocations.empty(); }

  /// Removes all locations from this set.
  void clear() {
    for (auto Pair : mLocations)
      delete Pair.second;
    mLocations.clear();
  }

  /// \brief Inserts a new location into this set, returns false if it already
  /// exists.
  ///
  /// If the specified value contains some value in this set, the appropriate
  /// value will be updated. In this case, this method also returns true.
  ///
  /// The MemorySetInfo is responsible for supplying methods make(Ty),
  /// getPtr(Ty), getSize(Ty), setSize(Ty &, uint64_t),
  /// getAATags(Ty), setAATags(Ty &, AAMDNodes) for each location type used.
  template<class Ty> std::pair<iterator, bool> insert(const Ty &Loc) {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end()) {
      LocationTy *NewLoc = MemoryInfo::make(Loc);
      auto Pair = mLocations.insert(
        std::make_pair(MemoryInfo::getPtr(Loc), NewLoc));
      return std::make_pair(iterator(Pair.first), true);
    }
    bool isChanged = true;
    if (MemoryInfo::getAATags(*I->second) != MemoryInfo::getAATags(Loc))
      if (MemoryInfo::getAATags(*I->second) ==
          llvm::DenseMapInfo<llvm::AAMDNodes>::getEmptyKey())
        MemoryInfo::setAATags(*I->second, MemoryInfo::getAATags(Loc));
      else
        MemoryInfo::setAATags(
          *I->second, llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey());
    else
      isChanged = false;
    if (MemoryInfo::getSize(*I->second) < MemoryInfo::getSize(Loc)) {
      MemoryInfo::setSize(*I->second, MemoryInfo::getSize(Loc));
      isChanged = true;
    }
    return std::make_pair(iterator(I), isChanged);
  }

  /// Inserts all locations from the range into this set, returns false
  /// if nothing has been added and updated.
  template<class location_iterator >
  bool insert(
      const location_iterator &LocBegin, const location_iterator &LocEnd) {
    bool isChanged = false;
    for (location_iterator I = LocBegin; I != LocEnd; ++I)
      isChanged = insert(*I).second || isChanged;
    return isChanged;
  }

  /// \brief Realizes intersection between two sets.
  ///
  /// The MemorySetInfo is responsible for supplying methods make(Ty),
  /// getPtr(Ty), getSize(Ty), setSize(Ty &, uint64_t),
  /// getAATags(Ty), setAATags(Ty &, AAMDNodes) for each location type used.
  template<class Ty> bool intersect(const MemorySet<Ty> &with) {
    if (this == &with)
      return false;
    MapTy PrevLocations;
    mLocations.swap(PrevLocations);
    bool isChanged = false;
    for (auto Pair : PrevLocations) {
      auto I = with.findContaining(*Pair.second);
      if (I != with.end()) {
        insert(*Pair.second);
        continue;
      }
      I = with.findCoveredBy(*Pair.second);
      if (I != with.end()) {
        insert(*I);
        isChanged = true;
      }
    }
    for (auto Pair : PrevLocations)
      delete Pair.second;
    return isChanged;
  }

  /// \brief Realizes merger between two sets.
  ///
  /// The MemorySetInfo is responsible for supplying methods make(Ty),
  /// getPtr(Ty), getSize(Ty), setSize(Ty &, uint64_t),
  /// getAATags(Ty), setAATags(Ty &, AAMDNodes) for each location type used.
  template<class Ty> bool merge(const MemorySet<Ty> &with) {
    if (this == &with)
      return false;
    bool isChanged = false;
    for (auto Pair : with.mLocations)
      isChanged = insert(*Pair.second).second || isChanged;
    return isChanged;
  }

  /// \brief Compares two sets.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) for each location type used.
  template<class Ty> bool operator!=(const MemorySet<Ty> &RHS) const {
    return !(*this == RHS);
  }

  /// \brief Compares two sets.
  ///
  /// The MemorySetInfo is responsible for supplying methods
  /// getSize(Ty), getAATags(Ty) for each location type used.
  template<class Ty> bool operator==(const MemorySet<Ty> &RHS) const {
    if (this == &RHS)
      return true;
    if (mLocations.size() != RHS.mLocations.size())
      return false;
    for (auto Pair : mLocations) {
      auto I = RHS.mLocations.find(Pair.first);
      if (I == RHS.mLocations.end() ||
        MemoryInfo::getSize(*I->second) != MemoryInfo::getSize(*Pair.second) ||
        MemoryInfo::getAATags(*I->second) != MemoryInfo::getAATags(*Pair.second))
        return false;
    }
    return true;
  }

private:
  MapTy mLocations;
};

/// \brief Calculates the difference between two sets of locations.
///
/// \param [in] LocBegin Iterator that points to the beginning of
/// the first locations set.
/// \param [in] LocEnd Iterator that points to the ending of
/// the first locations set.
/// \param [in] LocSet The second location set.
/// \param [out] Result It contains the result of this operation.
/// The following operation should be provided:
/// - void ResultSet::insert(const LocationTy &)
/// - void ResultSet::insert(location_iterator &, location_iterator &)
template<class location_iterator, class Ty, class ResultSet>
void difference(const location_iterator &LocBegin,
  const location_iterator &LocEnd,
  const MemorySet<Ty> &LocSet, ResultSet &Result) {
  MemorySet<Ty>::template difference(LocBegin, LocEnd, LocSet, Result);
}
}
#endif//TSAR_MEMORY_SET_H
