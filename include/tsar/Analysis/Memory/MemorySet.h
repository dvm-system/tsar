//===--- MemorySet.h --------- Memory Location Set --------------*- C++ -*-===//
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
// This file defines storage of objects which represent memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_SET_H
#define TSAR_MEMORY_SET_H

#include "tsar/Analysis/Memory/MemorySetInfo.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Value.h>
#include <algorithm>

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

  /// List of locations.
  using LocationList = llvm::SmallVector<LocationTy, 2>;

  /// Map from pointers to locations.
  using MapTy = llvm::DenseMap<const llvm::Value *, LocationList>;
public:
  /// \brief Calculate the difference between two sets of locations.
  ///
  /// The result set will contain locations from the first set which are not
  /// overlapped with any locations from the second set.
  /// \param [in] LocBegin Iterator that points to the beginning of
  /// the first location set.
  /// \param [in] LocEnd Iterator that points to the ending of
  /// the first location set.
  /// \param [in] LocSet The second location set.
  /// \param [out] Result It contains the result of this operation.
  /// The following operation should be provided:
  /// - void ResultSet::insert(const LocationTy &)
  /// - void ResultSet::insert(location_iterator &, location_iterator &)
  template<class location_iterator, class ResultSet>
  static void difference(
    const location_iterator &LocBegin, const location_iterator &LocEnd,
    const MemorySet &LocSet, ResultSet &Result) {
    if (LocSet.mLocations.empty()) {
      Result.insert(LocBegin, LocEnd);
    } else {
      for (location_iterator I = LocBegin; I != LocEnd; ++I)
        if (!LocSet.overlap(*I))
          Result.insert(*I);
    }
  }

  /// This implements iterator over all memory locations in a set.
  template<class map_iterator, class value_type> class LocationItr :
    public std::iterator<std::forward_iterator_tag, value_type> {
  public:
    LocationItr(const map_iterator &I, std::size_t Idx) :
      mCurItr(I), mIdx(Idx) {}

    bool operator==(const LocationItr &RHS) const {
      return mCurItr == RHS.mCurItr && mIdx == RHS.mIdx;
    }

    bool operator!=(const LocationItr &RHS) const {
      return !operator==(RHS);
    }

    value_type & operator*() const { return mCurItr->second[mIdx]; }

    value_type * operator->() const { return &operator*(); }

    /// Preincrement
    LocationItr & operator++() {
      ++mIdx;
      if (mCurItr->second.size() == mIdx) {
        ++mCurItr;
        mIdx = 0;
      }
      return *this;
    }

    /// Postincrement
    LocationItr operator++(int) {
      auto tmp = *this; ++*this; return tmp;
    }

  private:
    map_iterator mCurItr;
    std::size_t mIdx;
  };

  /// This type used to iterate over all locations in this set.
  using iterator = LocationItr<typename MapTy::iterator, LocationTy>;

  /// This type used to iterate over all locations in this set.
  using const_iterator =
    LocationItr<typename MapTy::const_iterator, const LocationTy>;

  MemorySet() = default;
  ~MemorySet() { clear(); }

  MemorySet(MemorySet &&that) : mLocations(std::move(that.mLocations)) {}
  MemorySet(const MemorySet &that) { insert(that.begin(), that.end()); }

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

  /// Return iterator that points to the beginning of locations.
  iterator begin() { return iterator(mLocations.begin(), 0); }

  /// Return iterator that points to the ending of locations.
  iterator end() { return iterator(mLocations.end(), 0); }

  /// Return iterator that points to the beginning of locations.
  const_iterator begin() const { return const_iterator(mLocations.begin(), 0); }

  /// Return iterator that points to the ending of locations.
  const_iterator end() const { return const_iterator(mLocations.end(), 0); }

  /// Return true if union of set locations contains a specified location.
  /// If `Locs != nullptr`, this locations will be stored to it. 
  template<class Ty>
  bool findContaining(const Ty &Loc,
                      llvm::SmallVectorImpl<Ty> *Locs = nullptr) const {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end())
      return false;
    llvm::SmallVector<Ty, 4> UnionLocs;
    if (MemoryInfo::getNumDims(Loc) > 0) {
      llvm::SmallVector<Ty, 2> Tails { Loc };
      for (auto &Curr : I->second) {
        if (Tails.empty()) {
          if (Locs)
            Locs->append(UnionLocs.begin(), UnionLocs.end());
          return true;
        }
        llvm::SmallVector<Ty, 2> NewTails;
        for (auto &Tail : Tails) {
          if (MemoryInfo::intersect(Curr, Tail, nullptr, &NewTails).hasValue())
            UnionLocs.push_back(Curr);
          else
            NewTails.push_back(Tail);
        }
        Tails = std::move(NewTails);
      }
      if (Tails.empty()) {
        if (Locs)
          Locs->append(UnionLocs.begin(), UnionLocs.end());
        return true;
      }
      return false;
    }
    auto Tail(Loc);
    bool KnownStartIdx = false;
    for (std::size_t Idx = 0, StartIdx = 0, EIdx = I->second.size();
         Idx < EIdx; ++Idx) {
      auto &Curr = I->second[Idx];
      if (MemoryInfo::sizecmp(
              MemoryInfo::getUpperBound(Curr),
              MemoryInfo::getLowerBound(Tail)) <= 0)
        continue;
      if (!KnownStartIdx)
        StartIdx = Idx;
      if (MemoryInfo::sizecmp(
              MemoryInfo::getLowerBound(Curr),
              MemoryInfo::getLowerBound(Tail)) > 0)
        return false;
      MemoryInfo::setLowerBound(MemoryInfo::sizeinc(
                                MemoryInfo::getUpperBound(Curr)), Tail);
      if (MemoryInfo::sizecmp(
              MemoryInfo::getUpperBound(Tail),
              MemoryInfo::getLowerBound(Tail)) < 0) {
          if (Locs) {
            for (std::size_t LocIdx = StartIdx; LocIdx < Idx; ++LocIdx)
              UnionLocs.push_back(I->second[LocIdx]);
          }
          return true;
        }
    }
    return false;
  }

  /// Return true if there are locations in this set which contain
  /// a specified location.
  template<class Ty> bool contain(const Ty &Loc) const {
    return findContaining(Loc);
  }

  /// Find list of locations which are contained in a specified location.
  template<class Ty> void findCoveredBy(const Ty &Loc,
      llvm::SmallVectorImpl<LocationTy> &Locs) const {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end())
      return;
    if (MemoryInfo::getNumDims(Loc) > 0) {
      for (auto &Curr : I->second) {
        auto IntOpt = MemoryInfo::intersect(Curr, Loc);
        if (IntOpt.hasValue())
          Locs.push_back(IntOpt.getValue());
      }
      return;
    }
    bool IsEmptyList = true;
    for (std::size_t Idx = 0, EIdx = I->second.size(); Idx < EIdx; ++Idx) {
      auto &Curr = I->second[Idx];
      if (MemoryInfo::sizecmp(
              MemoryInfo::getUpperBound(Curr),
              MemoryInfo::getLowerBound(Loc)) <= 0)
        continue;
      if (MemoryInfo::sizecmp(
              MemoryInfo::getLowerBound(Curr),
              MemoryInfo::getUpperBound(Loc)) >= 0)
        return;
      LocationTy CoveredLoc(Loc);
      MemoryInfo::setLowerBound(max(MemoryInfo::getLowerBound(Curr),
                                    MemoryInfo::getLowerBound(Loc)),
                                CoveredLoc);
      MemoryInfo::setUpperBound(min(MemoryInfo::getUpperBound(Curr),
                                    MemoryInfo::getUpperBound(Loc)),
                                CoveredLoc);

      if (IsEmptyList) {
        Locs.push_back(std::move(CoveredLoc));
        IsEmptyList = false;
      } else {
        if (MemoryInfo::sizecmp(
                MemoryInfo::getLowerBound(CoveredLoc),
                MemoryInfo::getUpperBound(Locs.back())) <= 0 &&
            MemoryInfo::sizecmp(
                MemoryInfo::getUpperBound(CoveredLoc),
                MemoryInfo::getLowerBound(Locs.back())) >= 0) {
          MemoryInfo::setLowerBound(
              min(MemoryInfo::getLowerBound(CoveredLoc),
                  MemoryInfo::getLowerBound(Locs.back())),
              Locs.back());
          MemoryInfo::setUpperBound(
              max(MemoryInfo::getUpperBound(CoveredLoc),
                  MemoryInfo::getUpperBound(Locs.back())),
              Locs.back());
        }
      }
    }
  }

  /// Return true if there are locations in this set which are contained
  /// in a specified location.
  template<class Ty> bool cover(const Ty &Loc) const {
    llvm::SmallVector<LocationTy, 2> CoveredBy;
    findCoveredBy(Loc, CoveredBy);
    return !CoveredBy.empty();
  }
  
  /// Return location which may overlap with a specified location.
  template<class Ty> iterator findOverlappedWith(const Ty &Loc) {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end())
      return iterator(mLocations.end(), 0);
    for (std::size_t Idx = 0, EIdx = I->second.size(); Idx < EIdx; ++Idx) {
      auto &Curr = I->second[Idx];
      if (MemoryInfo::hasIntersection(Curr, Loc))
        return iterator(I, Idx);
    }
    return iterator(mLocations.end(), 0);
  }

  /// Return location which may overlap with a specified location.
  template<class Ty> const_iterator findOverlappedWith(const Ty &Loc) const {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end())
      return const_iterator(mLocations.end(), 0);
    for (std::size_t Idx = 0, EIdx = I->second.size(); Idx < EIdx; ++Idx) {
      auto &Curr = I->second[Idx];
      if (MemoryInfo::hasIntersection(Curr, Loc))
        return const_iterator(I, Idx);
    }
    return const_iterator(mLocations.end(), 0);
  }

  /// Subtracts locations of this set from the specified location and puts
  /// the result to the list.
  template<class Ty> bool subtractFrom(const Ty &Loc,
      llvm::SmallVectorImpl<Ty> &Locs) const {
    typedef llvm::SmallVector<Ty, 4> LocationList;
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end())
      return false;
    bool Intersected = false;
    LocationList LocsToSub { Loc };
    for (std::size_t Idx = 0, EIdx = I->second.size(); Idx < EIdx; ++Idx) {
      auto &Curr = I->second[Idx];
      if (MemoryInfo::getNumDims(Curr) != MemoryInfo::getNumDims(Loc))
        continue;
      LocationList NewLocsToSub;
      for (auto &LocToSub : LocsToSub) {
        if (MemoryInfo::intersect(Curr, LocToSub, nullptr,
                                  &NewLocsToSub).hasValue()) {
          Intersected = true;
        } else {
          NewLocsToSub.push_back(LocToSub);
        }
      }
      LocsToSub = std::move(NewLocsToSub);
    }
    Locs = std::move(LocsToSub);
    return Intersected;
  }

  /// Return true if there is a location in this set which may overlap
  /// with a specified location.
  template<class Ty> bool overlap(const Ty &Loc) const {
    return findOverlappedWith(Loc) != end();
  }

  /// Return true if this set does not contain any location.
  bool empty() const { return mLocations.empty(); }

  /// Removes all locations from this set.
  void clear() { mLocations.clear(); }

  /// Insert a new location into this set, returns false if it already
  /// exists.
  ///
  /// If the specified value overlaps with value in this set, the appropriate
  /// value will be updated. In this case, this method also returns true.
  ///
  /// \attention This method updates AATags for an existing location. So,
  /// use `sanitizeAAInfo()` method to obtain correct value. Note, that alias
  /// analysis may not work if AATags is corrupted.
  template<class Ty> std::pair<iterator, bool> insert(const Ty &Loc) {
    auto I = mLocations.find(MemoryInfo::getPtr(Loc));
    if (I == mLocations.end()) {
      auto Pair = mLocations.try_emplace(MemoryInfo::getPtr(Loc));
      auto NewLoc = MemoryInfo::make(Loc);
      Pair.first->second.push_back(std::move(NewLoc));
      return std::make_pair(iterator(Pair.first, 0), true);
    }
    std::size_t Idx = 0;
    auto InsertItr = I->second.begin();
    for (auto EIdx = I->second.size(); Idx < EIdx; ++Idx) {
      auto &Curr = I->second[Idx];
      if (MemoryInfo::areJoinable(Curr, Loc)) {
        bool isChanged = true;
        if (MemoryInfo::getAATags(Curr) != MemoryInfo::getAATags(Loc))
          if (MemoryInfo::getAATags(Curr) ==
              llvm::DenseMapInfo<llvm::AAMDNodes>::getEmptyKey())
            MemoryInfo::setAATags(MemoryInfo::getAATags(Loc), Curr);
          else
            MemoryInfo::setAATags(
              llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey(), Curr);
        else
          isChanged = false;
        isChanged = MemoryInfo::join(Loc, Curr);
        return std::make_pair(iterator(I, Idx), isChanged);
      }
      if (MemoryInfo::getNumDims(Loc) == 0 && MemoryInfo::sizecmp(
              MemoryInfo::getLowerBound(*InsertItr),
              MemoryInfo::getLowerBound(Loc)) <= 0)
        ++InsertItr;
    }
    if (MemoryInfo::getNumDims(Loc) > 0)
      InsertItr = I->second.end();
    I->second.insert(InsertItr, Loc);
    return std::make_pair(iterator(I, Idx), true);
  }

  /// Insert all locations from the range into this set, returns false
  /// if nothing has been added and updated.
  template<class location_iterator >
  bool insert(
      const location_iterator &LocBegin, const location_iterator &LocEnd) {
    bool isChanged = false;
    for (location_iterator I = LocBegin; I != LocEnd; ++I)
      isChanged = insert(*I).second || isChanged;
    return isChanged;
  }

  /// Realize intersection between two sets.
  template<class Ty> bool intersect(const MemorySet<Ty> &With) {
    if (this == &With)
      return false;
    MapTy PrevLocations;
    mLocations.swap(PrevLocations);
    bool IsChanged = false;
    for (auto &Pair : PrevLocations) {
      for (auto &Loc : Pair.second) {
        if (With.contain(Loc)) {
          insert(Loc);
          continue;
        }
        llvm::SmallVector<LocationTy, 2> CoveredBy;
        With.findCoveredBy(Loc, CoveredBy);
        if (!CoveredBy.empty())
          IsChanged |= insert(CoveredBy.begin(), CoveredBy.end());
      }
    }
    return IsChanged;
  }

  /// Realize merger between two sets.
  template<class Ty> bool merge(const MemorySet<Ty> &With) {
    if (this == &With)
      return false;
    bool IsChanged = false;
    for (auto &Pair : With.mLocations)
      for (auto &Loc : Pair.second)
        IsChanged |= insert(Loc).second;
    return IsChanged;
  }

  /// Compare two sets.
  template<class Ty> bool operator!=(const MemorySet<Ty> &RHS) const {
    return !(*this == RHS);
  }

  /// Compare two sets.
  template<class Ty> bool operator==(const MemorySet<Ty> &RHS) const {
    if (this == &RHS)
      return true;
    if (mLocations.size() != RHS.mLocations.size())
      return false;
    auto sanitize = [](typename LocationList::const_iterator RangeItr,
        typename LocationList::const_iterator RangeItrE, LocationList &Locs) {
      Locs.push_back(*RangeItr);
      for (; RangeItr != RangeItrE; ++RangeItr) {
        if (MemoryInfo::sizecmp(
                MemoryInfo::getUpperBound(Locs.back()),
                MemoryInfo::getLowerBound(*RangeItr)) > 0 &&
            MemoryInfo::sizecmp(
                MemoryInfo::getLowerBound(Locs.back()),
                MemoryInfo::getUpperBound(*RangeItr)) < 0) {
          MemoryInfo::setLowerBound(
              min(MemoryInfo::getLowerBound(*RangeItr),
                  MemoryInfo::getLowerBound(Locs.back())),
              Locs.back());
          MemoryInfo::setUpperBound(
              max(MemoryInfo::getUpperBound(*RangeItr),
                  MemoryInfo::getUpperBound(Locs.back())),
              Locs.back());
        } else {
          Locs.push_back(*RangeItr);
        }
      }
    };
    for (auto &Pair : mLocations) {
      auto I = RHS.mLocations.find(Pair.first);
      if (I == RHS.mLocations.end())
        return false;
      LocationList LHSSet, RHSSet;
      sanitize(Pair.second.begin(), Pair.second.end(), LHSSet);
      sanitize(I->second.begin(), I->second.end(), RHSSet);
      if (LHSSet != RHSSet)
        return false;
    }
    return true;
  }

private:
  template<class SizeT>
  static const SizeT & max(const SizeT &L, const SizeT &R) {
    if (MemoryInfo::sizecmp(L, R) < 0)
      return R;
    return L;
  }
  template<class SizeT>
  static const SizeT & min(const SizeT &L, const SizeT &R) {
    if (MemoryInfo::sizecmp(L, R) > 0)
      return R;
    return L;
  }

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
