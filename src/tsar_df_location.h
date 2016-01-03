//===--- tsar_df_location.h - Data Flow Framework ------ ----------*- C++ -*-===//
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

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace llvm {
class Value;
class LoadInst;
class GetElementPtrInst;
}

namespace tsar {
/// \brief Representation of a data-flow value formed by a set of locations.
/// 
/// A data-flow value is a set of locations for which a number of operations
/// is defined.
class LocationDFValue {
  typedef llvm::SmallPtrSet<llvm::Value *, 64> LocationSet;
  // There are two kind of values. The KIND_FULL kind means that the set of
  // variables is full and contains all variables used in the analyzed program.
  // The KIND_MASK kind means that the set contains variables located in the 
  // location collection (mLocations). This is internal information which is
  // neccessary to safely and effectively implement a number of operations
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
  /// Creats a value, which contains all locations used in the analyzed
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
  /// \param [in] LocBegin Iterator that points to the beginning of the locations
  /// set.
  /// \param [in] LocEnd Iterator that points to the ending of the locations set.
  /// \param [in] Value Data-flow value.
  /// \param [out] Result It contains the result of this operation.
  /// The following operation should be provided:
  /// - void ResultSet::insert(llvm::Value *).
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
      if (Value.exist(*I))
        Result.insert(*I);
  }

  /// Destructor.
  ~LocationDFValue() { mKind = INVALID_KIND; }

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
  bool exist(llvm::Value *Loc) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mLocations.count(Loc);
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

  /// Inserts a new location into the value, returns false if it already exists.
  bool insert(llvm::Value *Loc) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mKind == KIND_FULL || mLocations.insert(Loc);
#else
    return mKind == KIND_FULL || mLocations.insert(Loc).second;
#endif
  }

  /// Inserts all locations from the range into the value, returns false
  /// ifnothing has been added.
  template<class location_iterator >
  bool insert(const location_iterator &LocBegin, const location_iterator &LocEnd) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      return false;
    bool isChanged = false;
    for (location_iterator I = LocBegin; I != LocEnd; ++I)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
      isChanged = mLocations.insert(*I) || isChanged;
#else
      isChanged = mLocations.insert(*I).second || isChanged;
#endif
    return isChanged;
  }

  /// Realizes intersection between two values.
  bool intersect(const LocationDFValue &with);

  /// Realizes merger between two values.
  bool merge(const LocationDFValue &with);

  /// Compares two values.
  bool operator==(const LocationDFValue &RHS) const;

  /// Compares two values.
  bool operator!=(const LocationDFValue &RHS) const { return !(*this == RHS); }
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
/// - void ResultSet::insert(llvm::Value *).
template<class location_iterator, class ResultSet>
static void difference(const location_iterator &LocBegin,
  const location_iterator &LocEnd,
  const LocationDFValue &Value, ResultSet &Result) {
  LocationDFValue::difference(LocBegin, LocEnd, Value, Result);
}

/// \brief Represents memory location as an expression in a source language.
///
/// \return A string which represents the memory location or an empty string.
/// \pre At this moment arrays does not supported. Location must not be null!
std::string locationToSource(llvm::Value *Loc);

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
  llvm::Value *Loc, llvm::DITypeRef &DITy, bool &NeadBracket);

/// \brief Represents memory location as an expression in a source language.
///
/// This function is overloaded for locations which are represented as a 'load'
/// instructions.
std::string locationToSource(
  llvm::LoadInst *Loc, llvm::DITypeRef &DITy, bool &NeadBracket);

/// \brief Represents memory location as an expression in a source language.
///
/// This function is overloaded for locations which are represented as a
/// 'getelementptr' instructions.
std::string locationToSource(
  llvm::GetElementPtrInst *Loc, llvm::DITypeRef &DITy, bool &NeadBracket);
}

/// Returns the base location with respect to which an analysis is performed.
///
/// For a given location, this returns the base memory location
/// with respect to which an analysis is performed. For example,
/// a base location for 'a[i]' is a whole array 'a'.
/// 
///TODO (kaniandr@gmail.com): locationToSource method must return valid
/// representation for each base location.
llvm::Value * findLocationBase(llvm::Value *Loc);
}

#endif//TSAR_DF_LOCATION_H
