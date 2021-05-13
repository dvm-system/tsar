//===--- DFMemoryLocation.h - Data Flow Framework ------ --------*- C++ -*-===//
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
// This file defines abstractions to access information obtained from data-flow
// analysis and associated with memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DF_LOCATION_H
#define TSAR_DF_LOCATION_H

#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#include "tsar/Analysis/Memory/MemorySet.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace llvm {
class DominatorTree;
class raw_ostream;
}

namespace tsar {
/// \brief Representation of a data-flow value formed by a set of locations.
///
/// A data-flow value is a set of locations for which a number of operations
/// is defined.
class LocationDFValue {
  // There are two kind of values. The KIND_FULL kind means that the set of
  // variables is full and contains all variables used in the analyzed program.
  // The KIND_MASK kind means that the set contains variables located in the
  // location collection (mLocations). This is internal information which is
  // necessary to safely and effectively implement a number of operations
  // which is permissible for a arbitrary set of variables.
  enum Kind {
    FIRST_KIND,
    KIND_FULL = FIRST_KIND,
    KIND_MASK,
    LAST_KIND = KIND_MASK,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };
  LocationDFValue(Kind K) : mKind(K) {
    assert(FIRST_KIND <= K && K <= LAST_KIND &&
      "The specified kind is invalid!");
  }
public:
  /// Creates a value, which contains all locations used in the analyzed
  /// program.
  static LocationDFValue fullValue() {
    return LocationDFValue(LocationDFValue::KIND_FULL);
  }

  /// Creates an empty value.
  static LocationDFValue emptyValue() {
    return LocationDFValue(LocationDFValue::KIND_MASK);
  }

  /// Default constructor creates an empty value.
  LocationDFValue() : LocationDFValue(LocationDFValue::KIND_MASK) {}

  /// \brief Calculates the difference between a set of locations and a set
  /// which is represented as a data-flow value.
  ///
  /// The result set will contain locations from the first set which are not
  /// overlapped with any locations from the value.
  /// \param [in] LocBegin Iterator that points to the beginning of
  /// the locations set.
  /// \param [in] LocEnd Iterator that points to the ending of
  /// the locations set.
  /// \param [in] Value Data-flow value.
  /// \param [out] Result It contains the result of this operation.
  /// The following operation should be provided:
  /// - void ResultSet::insert(const MemoryLocationRange &)
  /// - void ResultSet::insert(location_iterator &, location_iterator &)
  template<class location_iterator, class ResultSet>
  static void difference(
      const location_iterator &LocBegin, const location_iterator &LocEnd,
      const LocationDFValue &Value, ResultSet &Result) {
    //If all locations are contained in Value or range of iterators is empty,
    //than Result should be empty.
    if (Value.mKind == KIND_FULL || LocBegin == LocEnd)
      return;
    if (Value.mLocations.empty())
      Result.insert(LocBegin, LocEnd);
    for (location_iterator I = LocBegin; I != LocEnd; ++I)
      if (!Value.mLocations.overlap(*I))
        Result.insert(*I);
  }

  /// Destructor.
  ~LocationDFValue() {
    mLocations.clear();
    mKind = INVALID_KIND;
  }

  /// Move constructor.
  LocationDFValue(LocationDFValue &&that) :
    mKind(that.mKind), mLocations(std::move(that.mLocations)) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Copy constructor.
  LocationDFValue(const LocationDFValue &that) :
    mKind(that.mKind), mLocations(that.mLocations) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Move assignment operator.
  LocationDFValue & operator=(LocationDFValue &&that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mLocations = std::move(that.mLocations);
    }
    return *this;
  }

  /// Copy assignment operator.
  LocationDFValue & operator=(const LocationDFValue &that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mLocations = that.mLocations;
    }
    return *this;
  }

  /// Returns true if there is a location in this value which contains
  /// the specified location.
  bool contain(const MemoryLocationRange &Loc) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mLocations.contain(Loc);
  }

  /// Returns true if there is a location in this value which may overlap with
  /// the specified location.
  bool overlap(const MemoryLocationRange &Loc) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mLocations.overlap(Loc);
  }

  /// Subtracts all locations containing in this value from Loc and
  /// puts the result in Compl.
  bool subtractFrom(const MemoryLocationRange &Loc,
      llvm::SmallVectorImpl<MemoryLocationRange> &Compl) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      return false;
    return mLocations.subtractFrom(Loc, Compl);
  }

  /// Returns true if there is a location in this value which is contained
  /// in the specified location.
  bool cover(const MemoryLocationRange &Loc) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind != KIND_FULL && mLocations.cover(Loc);
  }

  /// Find list of locations which are contained in a specified location Loc
  /// and puts them to Locs.
  void findCoveredBy(const MemoryLocationRange &Loc,
      llvm::SmallVectorImpl<MemoryLocationRange> &Locs) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      Locs.push_back(Loc);
    else
      mLocations.findCoveredBy(Loc, Locs);
  }

  /// Returns true if the value does not contain any location.
  bool empty() const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_MASK && mLocations.empty();
  }

  /// Removes all locations from the value.
  void clear() {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    mKind = KIND_MASK;
    mLocations.clear();
  }

  /// \brief Inserts a new location into the value, returns false if it already
  /// exists.
  ///
  /// If the specified value contains some value in this set, the appropriate
  /// value will be updated. In this case, this method also returns true.
  bool insert(const MemoryLocationRange &Loc) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      return true;
    return mLocations.insert(Loc).second;
  }

  /// Inserts all locations from the range into the value, returns false
  /// if nothing has been added.
  template<class location_iterator >
  bool insert(
      const location_iterator &LocBegin, const location_iterator &LocEnd) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      return false;
    return mLocations.insert(LocBegin, LocEnd);
  }

  /// Realizes intersection between two values.
  bool intersect(const LocationDFValue &With);

  /// Realizes merger between two values.
  bool merge(const LocationDFValue &With);

  /// Compares two values.
  bool operator==(const LocationDFValue &RHS) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(RHS.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this == &RHS || mKind == KIND_FULL && RHS.mKind == KIND_FULL)
      return true;
    if (mKind != RHS.mKind)
      return false;
    return mLocations == RHS.mLocations;
  }

  /// Compares two values.
  bool operator!=(const LocationDFValue &RHS) const { return !(*this == RHS); }

  /// Prints value.
  void print(llvm::raw_ostream &OS,
    const llvm::DominatorTree *DT = nullptr) const;

  /// Support for debugging.
  void dump(const llvm::DominatorTree *DT = nullptr) const;

private:
  Kind mKind;
  MemorySet<MemoryLocationRange> mLocations;
};

/// \brief This calculates the difference between a set of locations and a set
/// which is represented as a data-flow value.
///
/// \param [in] LocBegin Iterator that points to the beginning of the locations
/// set.
/// \param [in] LocEnd Iterator that points to the ending of the locations set.
/// \param [in] Value Data-flow value.
/// \param [out] Result It contains the result of this operation.
/// The following operation should be provided:
/// - void ResultSet::insert(const MemoryLocationRange &)
/// - void ResultSet::insert(location_iterator &, location_iterator &)
template<class location_iterator, class ResultSet>
void difference(const location_iterator &LocBegin,
  const location_iterator &LocEnd,
  const LocationDFValue &Value, ResultSet &Result) {
  LocationDFValue::difference(LocBegin, LocEnd, Value, Result);
}
}
#endif//TSAR_DF_LOCATION_H
