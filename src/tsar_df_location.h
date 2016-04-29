//===--- tsar_df_location.h - Data Flow Framework ------ --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines abstractions to access information obtained from data-flow
// analysis and associated with memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DF_LOCATION_H
#define TSAR_DF_LOCATION_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Analysis/MemoryLocation.h>

namespace llvm {
class LoadInst;
class GetElementPtrInst;
class raw_ostream;
}

namespace tsar {
/// \brief This implements a set of memory locations.
///
/// Methods of this class do not use alias information. Consequently,
/// two locations may overlap if they have identical address of beginning.
/// \note This class manages memory allocation to store elements of
/// a location set.
class LocationSet {
  /// Map from pointers to locations.
  typedef llvm::DenseMap<const llvm::Value *, llvm::MemoryLocation *> MapTy;
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
  /// - void ResultSet::insert(const llvm::MemoryLocation &)
  /// - void ResultSet::insert(location_iterator &, location_iterator &)
  template<class location_iterator, class ResultSet>
  static void difference(
    const location_iterator &LocBegin, const location_iterator &LocEnd,
    const LocationSet &LocSet, ResultSet &Result) {
    if (LocSet.mLocations.empty())
      Result.insert(LocBegin, LocEnd);
    for (location_iterator I = LocBegin; I != LocEnd; ++I)
      if (!LocSet.overlap(*I))
        Result.insert(*I);
  }

  /// This implements iterator over all memory locations in a set.
  template<class map_iterator> class MemoryLocationItr :
    public std::iterator<std::forward_iterator_tag, llvm::MemoryLocation> {
  public:
    explicit MemoryLocationItr(const map_iterator &I) : mCurItr(I) {}

    bool operator==(const MemoryLocationItr &RHS) const {
      return mCurItr == RHS.mCurItr;
    }

    bool operator!=(const MemoryLocationItr &RHS) const {
      return !operator==(RHS);
    }

    value_type & operator*() const { return *(mCurItr->second); }

    value_type * operator->() const { return &operator*(); }

    /// Preincrement
    MemoryLocationItr & operator++() { ++mCurItr; return *this; }

    /// Postincrement
    MemoryLocationItr operator++(int) {
      auto tmp = *this; ++*this; return tmp;
    }

  private:
    map_iterator mCurItr;
  };

  /// This type used to iterate over all locations in this set.
  typedef MemoryLocationItr<MapTy::iterator> iterator;

  /// This type used to iterate over all locations in this set.
  typedef MemoryLocationItr<MapTy::const_iterator> const_iterator;

  /// Default constructor.
  LocationSet() {}

  /// Destructor.
  ~LocationSet() { clear(); }

  /// Move constructor.
  LocationSet(LocationSet &&that) :
    mLocations(std::move(that.mLocations)) {}

  /// Copy constructor.
  LocationSet(const LocationSet &that) {
    insert(that.begin(), that.end());
  }

  /// Move assignment operator.
  LocationSet & operator=(LocationSet &&that) {
    if (this != &that) {
      clear();
      mLocations = std::move(that.mLocations);
    }
    return *this;
  }

  /// Copy assignment operator.
  LocationSet & operator=(const LocationSet &that) {
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

  /// Returns a location which contains the specified location.
  iterator findContaining(const llvm::MemoryLocation &Loc) {
    auto I = mLocations.find(Loc.Ptr);
    return (I != mLocations.end() &&
      Loc.Size <= I->second->Size &&
      Loc.AATags == I->second->AATags) ?
      iterator(I) : iterator(mLocations.end());
  }

  /// Returns a location which contains the specified location.
  const_iterator findContaining(const llvm::MemoryLocation &Loc) const {
    auto I = mLocations.find(Loc.Ptr);
    return (I != mLocations.end() &&
      Loc.Size <= I->second->Size &&
      Loc.AATags == I->second->AATags) ?
      const_iterator(I) : const_iterator(mLocations.end());
  }

  /// Returns true if there is a location in this set which contains
  /// the specified location.
  bool contain(const llvm::MemoryLocation &Loc) const {
    return findContaining(Loc) != end();
  }

  /// Returns a location which is contained in the specified location.
  iterator findCoveredBy(const llvm::MemoryLocation &Loc) {
    auto I = mLocations.find(Loc.Ptr);
    return (I != mLocations.end() &&
      Loc.Size >= I->second->Size &&
      Loc.AATags == I->second->AATags) ?
      iterator(I) : iterator(mLocations.end());
  }

  /// Returns a location which is contained in the specified location.
  const_iterator findCoveredBy(const llvm::MemoryLocation &Loc) const {
    auto I = mLocations.find(Loc.Ptr);
    return (I != mLocations.end() &&
      Loc.Size >= I->second->Size &&
      Loc.AATags == I->second->AATags) ?
      const_iterator(I) : const_iterator(mLocations.end());
  }

  /// Returns true if there is a location in this set which is contained
  /// in the specified location.
  bool cover(const llvm::MemoryLocation &Loc) const {
    return findCoveredBy(Loc) != end();
  }

  /// Returns a location which may overlap with the specified location.
  iterator findOverlappedWith(const llvm::MemoryLocation &Loc) {
    return iterator(mLocations.find(Loc.Ptr));
  }

  /// Returns a location which may overlap with the specified location.
  const_iterator findOverlappedWith(const llvm::MemoryLocation &Loc) const {
    return const_iterator(mLocations.find(Loc.Ptr));
  }

  /// Returns true if there is a location in this set which may overlap with
  /// the specified location.
  bool overlap(const llvm::MemoryLocation &Loc) const {
    return findOverlappedWith(Loc) != end();
  }

  /// Returns true if this set does not contain any location.
  bool empty() const {return mLocations.empty();}

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
  std::pair<iterator, bool> insert(const llvm::MemoryLocation &Loc);

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

  /// Realizes intersection between two sets.
  bool intersect(const LocationSet &with);

  /// Realizes merger between two sets.
  bool merge(const LocationSet &with) {
    if (this == &with)
      return false;
    bool isChanged = false;
    for (auto Pair : with.mLocations)
      isChanged = insert(*Pair.second).second || isChanged;
    return isChanged;
  }

  /// Compares two sets.
  bool operator!=(const LocationSet &RHS) const { return !(*this == RHS); }

  /// Compares two sets.
  bool operator==(const LocationSet &RHS) const;

  /// Prints set of memory locations.
  void print(llvm::raw_ostream &OS) const;

  /// Support for debugging.
  void dump() const;

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
/// - void ResultSet::insert(const llvm::MemoryLocation &)
/// - void ResultSet::insert(location_iterator &, location_iterator &)
template<class location_iterator, class ResultSet>
void difference(const location_iterator &LocBegin,
  const location_iterator &LocEnd,
  const LocationSet &LocSet, ResultSet &Result) {
  LocationSet::difference(LocBegin, LocEnd, LocSet, Result);
}

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
  /// - void ResultSet::insert(const llvm::MemoryLocation &)
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

  /// Returns true if the value contains the specified location.
  bool contain(const llvm::MemoryLocation &Loc) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mLocations.contain(Loc);
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
  bool insert(const llvm::MemoryLocation &Loc) {
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
  bool intersect(const LocationDFValue &with);

  /// Realizes merger between two values.
  bool merge(const LocationDFValue &with);

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
  void print(llvm::raw_ostream &OS) const;

  /// Support for debugging.
  void dump() const;

private:
  Kind mKind;
  LocationSet mLocations;
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
/// - void ResultSet::insert(const llvm::MemoryLocation *)
/// - void ResultSet::insert(location_iterator &, location_iterator &)
template<class location_iterator, class ResultSet>
void difference(const location_iterator &LocBegin,
  const location_iterator &LocEnd,
  const LocationDFValue &Value, ResultSet &Result) {
  LocationDFValue::difference(LocBegin, LocEnd, Value, Result);
}

/// \brief This implements a set of base memory locations.
///
/// The base memory location is used to represent results of program analysis.
/// Let see an example of a base location for 'a[i]', it is a whole array 'a'.
/// \attention Methods of this class do not use alias information.
/// Consequently, two locations may overlap.
/// \note This class manages memory allocation to store elements of
/// a location set.
class BaseLocationSet {
  /// \brief Map from stripped pointers which address base locations to a sets
  /// which contain all base locations addressed by the key pointer.
  typedef llvm::DenseMap<const llvm::Value *, LocationSet *> StrippedMap;

  /// This is used to implement efficient iteration over all locations
  /// in the set.
  typedef llvm::SmallPtrSet<const llvm::MemoryLocation *, 64> BaseSet;

public:
  /// This type used to represent properties associated with a size of the set.
  typedef unsigned size_type;

  /// This type used to iterate over all locations in this set.
  typedef BaseSet::const_iterator iterator;

  /// This type used to iterate over all locations in this set.
  typedef BaseSet::const_iterator const_iterator;

  /// \brief Inserts a new base location into this set, returns false if
  ///  it already exists.
  ///
  /// If a base location for the specified location contains some base location
  /// in this set, the appropriate location will be updated.
  /// In this case, this method also returns true. For example,
  /// the following expressions *(short*)P and *P where P has type int
  /// have base locations started at *P with different sizes.
  /// When *(short*)P will be evaluated the result will be *P with size
  /// size_of(short). When *P will be evaluated the result will be *P with
  /// size size_of(int). These two results will be  merged, so the general base
  /// must be *P with size size_of(int).
  ///
  /// For some locations base location is unknown, for convenience unknown base
  /// location will be also stored in the set with null pointer to the beginning
  /// and unknown size.
  std::pair<iterator, bool> insert(const llvm::MemoryLocation &Loc);

  /// Returns true if there are no base locations is the set.
  bool empty() const { return mBases.empty(); }

  /// Returns number of base locations in the set.
  size_type size() const { return mBases.size(); }

  /// Return 1 if the specified location is in the set, 0 otherwise.
  size_type count(const llvm::MemoryLocation &Loc) const;

  /// Returns iterator that points to the beginning of locations.
  iterator begin() { return mBaseList.begin(); }

  /// Returns iterator that points to the ending of locations.
  iterator end() { return mBaseList.end(); }

  /// Returns iterator that points to the beginning of locations.
  const_iterator begin() const { return mBaseList.begin(); }

  /// Returns iterator that points to the ending of locations.
  const_iterator end() const { return mBaseList.end(); }

private:
  /// \brief Strips a pointer to an 'alloca' or a 'global variable'.
  static const llvm::Value * stripPointer(const llvm::Value *Ptr);

  /// \brief Strips a location to its base.
  ///
  /// Base location will be stored in the parameter Loc: pointer or size can be
  /// changed. Final type cast will be eliminate but size will be remembered.
  /// A base location for element of an array is a whole array, so 'getelementpr'
  /// will be stripped (pointer will be changed) and size will be changed to
  /// llvm::MemoryLocation::UnknownSize. But if this element is a structure
  /// 'getelementptr' will not be stripped because it is convenient to analyze
  /// different members of structure separately. If base is unknown Loc.Ptr will
  /// be set to nullptr.
  static void stripToBase(llvm::MemoryLocation &Loc);

  /// Compares to bases.
  static bool isSameBase(const llvm::Value *BasePtr1, const llvm::Value *BasePtr2);

  StrippedMap mBases;
  BaseSet mBaseList;
};

/// \brief Represents memory location as an expression in a source language.
///
/// \return A string which represents the memory location or an empty string.
/// \pre At this moment arrays does not supported. Location must not be null!
std::string locationToSource(const llvm::Value *Loc);

namespace detail {
/// \brief Represents memory location as an expression in a source language.
///
/// \param [in] Loc A memory location.
/// \param [out] DITy Meta information describing a type of this location.
/// \param [out] NeadBracket This specifies that brackets are necessary
/// to combined the result with other expressions. For example, brackets are
/// necessary if the result is 'p+1', where 'p' is a pointer. To dereference
/// this location *(p+1) expression should be used.
/// \return A string which represents the memory location or an empty string.
/// \pre At this moment arrays does not supported. Location must not be null!
std::string locationToSource(
  const llvm::Value *Loc, llvm::DITypeRef &DITy, bool &NeadBracket);

/// \brief Represents memory location as an expression in a source language.
///
/// This function is overloaded for locations which are represented as a 'load'
/// instructions.
std::string locationToSource(
  const llvm::LoadInst *Loc, llvm::DITypeRef &DITy, bool &NeadBracket);

/// \brief Represents memory location as an expression in a source language.
///
/// This function is overloaded for locations which are represented as a
/// 'getelementptr' instructions.
std::string locationToSource(
  const llvm::GetElementPtrInst *Loc, llvm::DITypeRef &DITy, bool &NeadBracket);
}
}

#endif//TSAR_DF_LOCATION_H
