//===--- tsar_trait.h ------ Analyzable Traits ------------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines traits which could be recognized by the analyzer.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_TRAIT_H
#define TSAR_TRAIT_H

#include <llvm/ADT/SmallPtrSet.h>
#include "tsar_df_location.h"
#include <cell.h>
#include <trait.h>
#include <utility.h>

namespace llvm {
class MemoryLocation;
}

namespace tsar {
using bcl::operator "" _b;

/// Declaration of a trait recognized by analyzer.
#define TSAR_TRAIT_DECL(name_, id_, string_) \
struct name_ : public bcl::TraitBase<id_> { \
  static llvm::StringRef toString() { \
    static std::string Str(string_); \
    return Str; \
  } \
};

namespace trait {
TSAR_TRAIT_DECL(AddressAccess,        1_b, "address access")
TSAR_TRAIT_DECL(NoAccess,             1111111_b, "no access")
TSAR_TRAIT_DECL(Shared,               1011110_b, "shared")
TSAR_TRAIT_DECL(Private,              0001111_b, "private")
TSAR_TRAIT_DECL(FirstPrivate,         0001110_b, "first private")
TSAR_TRAIT_DECL(SecondToLastPrivate,  0001011_b, "second to last private")
TSAR_TRAIT_DECL(LastPrivate,          0000111_b, "last private")
TSAR_TRAIT_DECL(DynamicPrivate,       0000011_b, "dynamic private")
TSAR_TRAIT_DECL(Reduction,            1000000_b, "reduction")
TSAR_TRAIT_DECL(Dependency,           0000000_b, "dependency")
}

/// This represents list of traits for a memory location which can be recognized
/// by analyzer.
///
/// The following information is available:
/// - a set of locations addresses of which are evaluated;
/// - a set of private locations;
/// - a set of last private locations;
/// - a set of second to last private locations;
/// - a set of dynamic private locations;
/// - a set of first private locations;
/// - a set of shared locations;
/// - a set of locations that caused dependency.
///
/// Calculation of a last private variables differs depending on internal
/// representation of a loop. There are two type of representations.
/// -# The first type has a following pattern:
/// \code
/// iter: if (...) goto exit;
///           ...
///         goto iter;
/// exit:
/// \endcode
/// For example, representation of a for-loop refers to this type.
/// The candidates for last private variables associated with the for-loop
/// will be stored as second to last privates locations, because
/// the last definition of these locations is executed on the second to the last
/// loop iteration (on the last iteration the loop condition
/// check is executed only).
/// -# The second type has a following pattern:
/// \code
/// iter:
///           ...
///       if (...) goto exit; else goto iter;
/// exit:
/// \endcode
/// For example, representation of a do-while-loop refers to this type.
/// In this case the candidates for last private variables
/// will be stored as last privates locations.
///
/// In some cases it is impossible to determine in static an iteration
/// where the last definition of an location have been executed. Such locations
/// will be stored as dynamic private locations collection.
using DependencyDescriptor = bcl::TraitDescriptor<trait::AddressAccess,
  bcl::TraitAlternative<trait::NoAccess, trait::Shared, trait::Private,
    trait::Reduction, trait::Dependency,
    bcl::TraitUnion<trait::LastPrivate, trait::FirstPrivate>,
    bcl::TraitUnion<trait::SecondToLastPrivate, trait::FirstPrivate>,
    bcl::TraitUnion<trait::DynamicPrivate, trait::FirstPrivate>>>;

/// \brief This is a set of traits for a memory location.
///
/// In general this class represents traits of base locations which has been
/// collected by a base location set, so it is not possible to modify this
///locations.
class LocationTraitSet : public bcl::TraitSet<
  DependencyDescriptor, llvm::DenseMap<bcl::TraitKey, void *>> {
  /// Base class.
  using Base = bcl::TraitSet<
    DependencyDescriptor, llvm::DenseMap<bcl::TraitKey, void *>>;

public:
  /// \brief Creates set of traits.
  LocationTraitSet(const llvm::MemoryLocation *Loc, DependencyDescriptor Dptr) :
    Base(Dptr), mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Returns memory location.
  const llvm::MemoryLocation * memory() const { return mLoc; }

private:
  const llvm::MemoryLocation *mLoc;
};

/// This is a set of different traits suitable for loops.
class DependencySet {
  typedef llvm::DenseMap<const llvm::MemoryLocation *,
    std::unique_ptr<LocationTraitSet>> LocationTraitMap;
public:
  /// This class used to iterate over traits of different memory locations.
  class iterator :
    public std::iterator<std::forward_iterator_tag, LocationTraitSet> {
  public:
    explicit iterator(const LocationTraitMap::const_iterator &I) :
      mCurrItr(I) {}
    explicit iterator(LocationTraitMap::const_iterator &&I) :
      mCurrItr(std::move(I)) {}
    LocationTraitSet & operator*() const { return *mCurrItr->second; }
    LocationTraitSet * operator->() const { return &operator*(); }
    bool operator==(const iterator &RHS) const {
      return mCurrItr == RHS.mCurrItr;
    }
    bool operator!=(const iterator &RHS) const { return !operator==(RHS); }
    iterator & operator++() { ++mCurrItr; return *this; }
    iterator operator++(int) { iterator Tmp = *this; ++*this; return Tmp; }
  private:
    LocationTraitMap::const_iterator mCurrItr;
  };

  /// This type used to iterate over traits of different memory locations.
  typedef iterator const_iterator;

  /// \brief Returns base memory location for a specified one.
  ///
  /// The base location will be stored in a base location set and memory
  /// allocation will be managed by this class.
  const llvm::MemoryLocation * base(const llvm::MemoryLocation &Loc) const {
    return *mBases.insert(Loc).first;
  }

  /// \brief Finds traits of a base memory location.
  ///
  /// If the specified location is not a base memory location the base location
  /// will be obtained at first. Then results for the base location will be
  /// returned.
  iterator find(const llvm::MemoryLocation &Loc) const {
    return iterator(mTraits.find(base(Loc)));
  }

  /// \brief Insert traits of a base memory location.
  ///
  /// If the specified location is not a base memory location the base location
  /// will be obtained at first. Note, that this class manages memory allocation
  /// to store traits.
  std::pair<iterator, bool> insert(
      const llvm::MemoryLocation &Loc, DependencyDescriptor Dptr) {
    auto BaseLoc = base(Loc);
    auto Pair = mTraits.insert(
      std::make_pair(BaseLoc,
      std::unique_ptr<LocationTraitSet>(new LocationTraitSet(BaseLoc, Dptr))));
    return std::make_pair(iterator(std::move(Pair.first)), Pair.second);
  }

  /// Erase traits of a base memory location.
  bool erase(const llvm::MemoryLocation &Loc) {
    return mTraits.erase(base(Loc));
  }

  /// Erase traits of all base memory locations.
  void clear() { mTraits.clear(); }

  /// Returns true if there are no locations with known traits.
  bool empty() const { return mTraits.empty(); }

  /// Returns number of locations with known traits.
  unsigned size() const { return mTraits.size(); }

  /// Returns iterator that points to the beginning of the traits list.
  iterator begin() const { return iterator(mTraits.begin()); }

  /// Returns iterator that points to the ending of the traits list.
  iterator end() const { return iterator(mTraits.end()); }

private:
  mutable BaseLocationSet mBases;
  LocationTraitMap mTraits;
};

/// This attribute is associated with DependencySet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DependencyAttr, DependencySet)
}
#endif//TSAR_TRAIT_H