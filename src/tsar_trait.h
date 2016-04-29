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
#include <utility.h>

namespace llvm {
class MemoryLocation;
class Value;
}

namespace tsar {
using Utility::operator "" _b;

/// This represents list of traits which can be racognized by analyzer.
///
/// The following information is available:
/// - a set of analyzed locations;
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
struct trait {
  /// Identifier of a trait.
  typedef unsigned long long TraitId;

  /// Set of memory locations.
  typedef tsar::LocationSet LocationSet;

  /// Set of base memory locations.
  typedef tsar::BaseLocationSet BaseLocationSet;

  /// \brief Weak set of memory locations.
  ///
  /// This set is weak because it does not manage memory allocation unlike
  /// tsar::LocationSet. It is convinient to use the trait::Analyze trait
  /// to manage memory allocation.
  typedef llvm::SmallPtrSet<const llvm::MemoryLocation *, 64> LocationWeakSet;

  /// Set of pointers to memory locations.
  typedef llvm::SmallPtrSet<const llvm::Value *, 64> PointerSet;

  /// Set of instructions.
  typedef llvm::SmallPtrSet<llvm::Instruction *, 64> InstructionSet;

/// \brief Declaration of a trait recognized by analyzer.
///
/// Use this macro with TSAR_TRAIT_DEF
#define TSAR_TRAIT_DECL(name_, id_, collection_) \
struct name_##Ty { \
  static constexpr TraitId Id = id_; \
  typedef collection_ ValueType; \
  constexpr operator const TraitId & () const { return Id; } \
}; \
static constexpr name_##Ty name_ = name_##Ty();

  TSAR_TRAIT_DECL(Analyze, 0000000_b, BaseLocationSet)
  TSAR_TRAIT_DECL(NoAccess, 1111111_b, LocationWeakSet)
  TSAR_TRAIT_DECL(AddressAccess, 1011111_b, PointerSet)
  TSAR_TRAIT_DECL(Shared, 0111110_b, LocationWeakSet)
  TSAR_TRAIT_DECL(Private, 0101111_b, LocationWeakSet)
  TSAR_TRAIT_DECL(FirstPrivate, 0101110_b, LocationWeakSet)
  TSAR_TRAIT_DECL(SecondToLastPrivate, 0101011_b, LocationWeakSet)
  TSAR_TRAIT_DECL(LastPrivate, 0100111_b, LocationWeakSet)
  TSAR_TRAIT_DECL(DynamicPrivate, 0100011_b, LocationWeakSet)
  TSAR_TRAIT_DECL(Dependency, 0100000_b, LocationWeakSet)

#undef TSAR_TRAIT_DECL
};

/// \brief This is a static list of different traits suitable for loops.
///
/// Let us give the following example to explain how to access the information:
/// \code
/// DependencySet DS;
/// for (Value *Loc : DS[Private]) {...}
/// \endcode
/// Note, (*DS)[Private] is a set of type LocationSet, so it is possible to call
/// all methods that is available for LocationSet.
/// You can also use LastPrivate, SecondToLastPrivate, DynamicPrivate instead of
/// Private to access the necessary kind of locations.
class DependencySet : public CELL_COLL_9(
  trait::AnalyzeTy,
  trait::AddressAccessTy,
  trait::PrivateTy,
  trait::LastPrivateTy,
  trait::SecondToLastPrivateTy,
  trait::DynamicPrivateTy,
  trait::FirstPrivateTy,
  trait::SharedTy,
  trait::DependencyTy) {
public:
  /// \brief Checks that a location has a specified kind of privatizability.
  ///
  /// Usage: DependencySet *DS; DS->is(trait::Private, Loc);
  template<class Kind> bool is(Kind K, const llvm::MemoryLocation *Loc) const {
    return (*this)[K].count(Loc) != 0;
  }
};

/// This attribute is associated with DependencySet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DependencyAttr, DependencySet)
}
#endif//TSAR_TRAIT_H
