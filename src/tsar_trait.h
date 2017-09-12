//===--- tsar_trait.h ------ Analyzable Traits ------------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines traits which could be recognized by the analyzer.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_MEMORY_TRAIT_H
#define TSAR_MEMORY_TRAIT_H

#include <trait.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringRef.h>

namespace tsar {
class AliasNode;
class AliasTree;
class EstimateMemory;
class ExplicitAccseeCoverage;

/// Declaration of a trait recognized by analyzer.
#define TSAR_TRAIT_DECL(name_, string_) \
struct name_ { \
  static llvm::StringRef toString() { \
    static std::string Str(string_); \
    return Str; \
  } \
  static std::string & name() { \
    static std::string Str(#name_); \
    return Str; \
  } \
};

namespace trait {
TSAR_TRAIT_DECL(AddressAccess, "address access")
TSAR_TRAIT_DECL(ExplicitAccess, "explicit access")
TSAR_TRAIT_DECL(NoAccess, "no access")
TSAR_TRAIT_DECL(Shared, "shared")
TSAR_TRAIT_DECL(Private, "private")
TSAR_TRAIT_DECL(FirstPrivate, "first private")
TSAR_TRAIT_DECL(SecondToLastPrivate, "second to last private")
TSAR_TRAIT_DECL(LastPrivate, "last private")
TSAR_TRAIT_DECL(DynamicPrivate, "dynamic private")
TSAR_TRAIT_DECL(Reduction, "reduction")
TSAR_TRAIT_DECL(Dependency, "dependency")
TSAR_TRAIT_DECL(Induction, "induction")
}

#undef TSAR_TRAIT_DECL

/// \brief This represents list of traits for a memory location which can be
/// recognized by analyzer.
///
/// The following information is available:
/// - is it a location which is not accessed in a region
/// - is it a location which is explicitly accessed in a region
/// - is it a location address of which is evaluated;
/// - is it private location;
/// - is it a last private location;
/// - is it a second to last private location;
/// - is it a dynamic private location;
/// - is it a first private location;
/// - is it a shared location;
/// - is it a location that caused dependency.
/// - is it a loop induction location;
///
/// If location is not accessed in a region it will be marked as 'no access'
/// only if it has some other traits, otherwise it can be omitted in a list
/// of region traits.
///
/// Location is accessed in a region implicitly if descendant of it in an
/// estimate memory tree will be accessed explicitly. If some other location is
/// accessed due to alias with such location it is not treated.
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
using DependencyDescriptor = bcl::TraitDescriptor<
  trait::AddressAccess, trait::ExplicitAccess,
  bcl::TraitAlternative<trait::NoAccess, trait::Shared, trait::Private,
  trait::Reduction, trait::Dependency, trait::Induction,
  bcl::TraitUnion<trait::LastPrivate, trait::FirstPrivate>,
  bcl::TraitUnion<trait::SecondToLastPrivate, trait::FirstPrivate>,
  bcl::TraitUnion<trait::DynamicPrivate, trait::FirstPrivate>>>;

/// \brief This is a set of traits for a memory location.
///
/// In general this class represents traits of estimate locations which has been
/// collected by an alias tree, so it is not possible to modify this locations.
class LocationTrait : public DependencyDescriptor {
public:
  /// Creates set of traits.
  explicit LocationTrait(const EstimateMemory *Loc) : mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Creates set of traits.
  LocationTrait(const EstimateMemory *Loc, const DependencyDescriptor &Dptr) :
    DependencyDescriptor(Dptr), mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Creates set of traits.
  LocationTrait(const EstimateMemory *Loc, DependencyDescriptor &&Dptr) :
    DependencyDescriptor(std::move(Dptr)), mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Assigns dependency descriptor to this set of traits.
  LocationTrait & operator=(const DependencyDescriptor &Dptr) noexcept {
    DependencyDescriptor::operator=(Dptr);
    return *this;
  }

  /// Assigns dependency descriptor to this set traits.
  LocationTrait & operator=(DependencyDescriptor &&Dptr) noexcept {
    DependencyDescriptor::operator=(std::move(Dptr));
    return *this;
  }

  /// Returns memory location.
  const EstimateMemory * getMemory() const { return mLoc; }

private:
  const EstimateMemory *mLoc;
};
}

namespace llvm {
/// This provides DenseMapInfo for LocationTrait.
template<> struct DenseMapInfo<tsar::LocationTrait> {
  static inline tsar::LocationTrait getEmptyKey() {
    return tsar::LocationTrait(
      DenseMapInfo<const tsar::EstimateMemory *>::getEmptyKey());
  }
  static inline tsar::LocationTrait getTombstoneKey() {
    return tsar::LocationTrait(
      DenseMapInfo<const tsar::EstimateMemory *>::getTombstoneKey());
  }
  static unsigned getHashValue(const tsar::LocationTrait &Val) {
    return DenseMapInfo<const tsar::EstimateMemory *>
      ::getHashValue(Val.getMemory());
  }
  static unsigned getHashValue(const tsar::EstimateMemory *Val) {
    assert(Val && "Estimate memory must not be null!");
    return DenseMapInfo<const tsar::EstimateMemory *>
      ::getHashValue(Val);
  }
  static bool isEqual(
      const tsar::LocationTrait &LHS, const tsar::LocationTrait &RHS) {
    return LHS.getMemory() == RHS.getMemory(); }
  static bool isEqual(
      const tsar::EstimateMemory *LHS, const tsar::LocationTrait &RHS) {
    return LHS == RHS.getMemory();
  }
};
}

namespace tsar {
/// \brief This is a set of traits for an alias node.
///
/// In general this class represents traits of alias nodes which has been
/// collected by an alias tree, so it is not possible to modify this nodes.
///
/// For a node a number of accessed estimate memory locations are available.
/// Each of these locations either is explicitly accessed in the analyzed region
/// or covers some of explicitly accessed locations. There are also traits of
/// each such location. Conservative combination of these traits leads to
/// the proposed traits of a node. Nodes that explicitly accessed locations may
/// be associated in some of descendant alias nodes of the current one.
class AliasTrait : public DependencyDescriptor, private bcl::Uncopyable {
  /// List of explicitly accessed estimate memory locations and their traits.
  using AccessTraits = llvm::DenseSet<LocationTrait>;

public:
  /// This class used to iterate over traits of different memory locations.
  using iterator = AccessTraits::iterator;

  /// This class used to iterate over traits of different memory locations.
  using const_iterator = AccessTraits::const_iterator;

  /// This stores size of a list of explicitly accessed locations.
  using size_type = AccessTraits::size_type;

  /// Creates representation of traits.
  explicit AliasTrait(const AliasNode *N) : mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Creates representation of traits.
  AliasTrait(const AliasNode *N, const DependencyDescriptor &Dptr) :
    DependencyDescriptor(Dptr), mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Creates representation of traits.
  AliasTrait(const AliasNode *N, DependencyDescriptor &&Dptr) :
    DependencyDescriptor(std::move(Dptr)), mNode(N) {
    assert(N && "Alias node must not be null!");
  }

  /// Assigns dependency descriptor to this set of traits.
  AliasTrait & operator=(const DependencyDescriptor &Dptr) noexcept {
    DependencyDescriptor::operator=(Dptr);
    return *this;
  }

  /// Assigns dependency descriptor to this set of traits.
  AliasTrait & operator=(DependencyDescriptor &&Dptr) noexcept {
    DependencyDescriptor::operator=(std::move(Dptr));
    return *this;
  }

  /// Returns an alias node for which traits is specified.
  const AliasNode * getNode() const noexcept { return mNode; }

  /// \brief Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  ///
  /// \pre The specified estimate memory location is contained in the `node()`.
  std::pair<iterator, bool> insert(const LocationTrait &LT) {
    return mAccesses.insert(LT);
  }

  /// \brief Adds traits of an explicitly accessed location, returns false if
  /// such location already exists. Its traits will not be updated.
  ///
  /// \pre The specified estimate memory location is contained in the `node()`.
  std::pair<iterator, bool> insert(LocationTrait &&LT) {
    return mAccesses.insert(std::move(LT));
  }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed locations.
  iterator begin() { return mAccesses.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed locations.
  iterator end() { return mAccesses.end(); }

  /// Returns iterator that points to the beginning of the list of
  /// explicitly accessed locations.
  const_iterator begin() const { return mAccesses.begin(); }

  /// Returns iterator that points to the ending of the list of
  /// explicitly accessed locations.
  const_iterator end() const { return mAccesses.end(); }

  /// Returns traits of a specified estimate memory location if it is
  /// explicitly accessed.
  iterator find(const EstimateMemory *EM) {
    assert(EM && "Estimate memory must not be null!");
    return mAccesses.find_as(EM);
  }

  /// Returns traits of a specified estimate memory location if it is
  /// explicitly accessed.
  const_iterator find(const EstimateMemory *EM) const {
    assert(EM && "Estimate memory must not be null!");
    return mAccesses.find_as(EM);
  }

  /// Returns number of explicitly accessed locations.
  size_type count() const { return mAccesses.size(); }

  /// Returns true if there are no explicitly accessed locations.
  bool empty() const { return mAccesses.empty(); }

  /// Removes an explicitly accessed location from the list.
  bool erase(const EstimateMemory *EM) {
    auto I = find(EM);
    return I != end() ? mAccesses.erase(I), true : false;
  }

  /// Removes all explicitly accessed locations from the list.
  void clear() { mAccesses.clear(); }

private:
  const AliasNode *mNode;
  AccessTraits mAccesses;
};

/// This is a set of different traits suitable for a region.
class DependencySet {
  using AliasTraits =
    llvm::DenseMap<const AliasNode *, std::unique_ptr<AliasTrait>>;
public:
  /// This class used to iterate over traits of different alias nodes.
  class const_iterator :
    public std::iterator<std::forward_iterator_tag, AliasTrait> {
  public:
    explicit const_iterator(const AliasTraits::const_iterator &I) :
      mCurrItr(I) {}
    explicit const_iterator(AliasTraits::const_iterator &&I) :
      mCurrItr(std::move(I)) {}
    const AliasTrait & operator*() const { return *mCurrItr->second; }
    const AliasTrait * operator->() const { return &operator*(); }
    bool operator==(const const_iterator &RHS) const {
      return mCurrItr == RHS.mCurrItr;
    }
    bool operator!=(const const_iterator &RHS) const {
      return !operator==(RHS);
    }
    iterator & operator++() { ++mCurrItr; return *this; }
    iterator operator++(int) { iterator Tmp = *this; ++*this; return Tmp; }
  private:
    AliasTraits::const_iterator mCurrItr;
  };

  /// This type used to iterate over traits of different alias nodes.
  class iterator : public const_iterator {
  public:
    explicit iterator(const AliasTraits::const_iterator &I) :
      const_iterator(I) {}
    explicit iterator(AliasTraits::const_iterator &&I) :
      const_iterator(std::move(I)) {}
    AliasTrait & operator*() const {
      return const_cast<AliasTrait &>(const_iterator::operator*());
    }
    AliasTrait * operator->() const { return &operator*(); }
    bool operator==(const const_iterator &RHS) const {
      return const_iterator::operator==(RHS);
    }
    bool operator!=(const const_iterator &RHS) const {
      return !operator==(RHS);
    }
    iterator & operator++() { const_iterator::operator++(); return *this; }
    iterator operator++(int) { iterator Tmp = *this; ++*this; return Tmp; }
  };

  /// Creates set of traits.
  explicit DependencySet(const AliasTree &AT) : mAliasTree(&AT) {}

  /// Returns alias tree nodes of which are analyzed.
  const AliasTree * getAliasTree() const noexcept { return mAliasTree; }

  /// Returns iterator that points to the beginning of the traits list.
  iterator begin() { return iterator(mTraits.begin()); }

  /// Returns iterator that points to the ending of the traits list.
  iterator end() { return iterator(mTraits.end()); }

  /// Returns iterator that points to the beginning of the traits list.
  const_iterator begin() const { return const_iterator(mTraits.begin()); }

  /// Returns iterator that points to the ending of the traits list.
  const_iterator end() const { return const_iterator(mTraits.end()); }


  /// Finds traits of a specified alias node.
  iterator find(const AliasNode *N) const { return iterator(mTraits.find(N)); }

  /// Inserts traits of a specified alias node.
  std::pair<iterator, bool> insert(
      const AliasNode *N, const DependencyDescriptor &Dptr) {
    auto Pair = mTraits.insert(
      std::make_pair(N, llvm::make_unique<AliasTrait>(N, Dptr)));
    return std::make_pair(iterator(std::move(Pair.first)), Pair.second);
  }

  /// Inserts traits of a specified alias node.
  std::pair<iterator, bool> insert(
      const AliasNode *N, DependencyDescriptor &&Dptr) {
    auto Pair = mTraits.insert(
      std::make_pair(N, llvm::make_unique<AliasTrait>(N, std::move(Dptr))));
    return std::make_pair(iterator(std::move(Pair.first)), Pair.second);
  }

  /// Erases traits of a specified alias node.
  bool erase(const AliasNode *N) { return mTraits.erase(N); }

  /// Erases traits of all alias nodes.
  void clear() { mTraits.clear(); }

  /// Returns true if there are no alias nodes with known traits.
  bool empty() const { return mTraits.empty(); }

  /// Returns number of alias nodes with known traits.
  unsigned size() const { return mTraits.size(); }

private:
  AliasTraits mTraits;
  const AliasTree *mAliasTree;
};
}
#endif//TSAR_MEMORY_TRAIT_H