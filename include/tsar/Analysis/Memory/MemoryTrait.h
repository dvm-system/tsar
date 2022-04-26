//===--- MemoryTrait.h ------ Memory Analyzable Traits  ---------*- C++ -*-===//
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
// This file defines traits of memory locations which could be recognized by
// the analyzer.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_TRAIT_H
#define TSAR_MEMORY_TRAIT_H

#include <bcl/trait.h>
#include <bcl/tagged.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/StringRef.h>

namespace tsar {
namespace trait {
/// Definition of methods that identify a trait.
#define TSAR_TRAIT_DECL_STRING(name_, string_) \
static llvm::StringRef toString() { \
  static std::string Str(string_); \
  return Str; \
} \
static std::string & name() { \
  static std::string Str(#name_); \
  return Str; \
}

/// Declaration of a trait recognized by analyzer.
#define TSAR_TRAIT_DECL(name_, string_) \
struct name_ { \
  TSAR_TRAIT_DECL_STRING(name_, string_) \
};

TSAR_TRAIT_DECL(AddressAccess, "address access")
TSAR_TRAIT_DECL(ExplicitAccess, "explicit access")
TSAR_TRAIT_DECL(HeaderAccess, "header access")
TSAR_TRAIT_DECL(NoAccess, "no access")
TSAR_TRAIT_DECL(Readonly, "read only")
TSAR_TRAIT_DECL(Shared, "shared")
TSAR_TRAIT_DECL(Local, "local")
TSAR_TRAIT_DECL(Private, "private")
TSAR_TRAIT_DECL(FirstPrivate, "first private")
TSAR_TRAIT_DECL(SecondToLastPrivate, "second to last private")
TSAR_TRAIT_DECL(LastPrivate, "last private")
TSAR_TRAIT_DECL(DynamicPrivate, "dynamic private")
TSAR_TRAIT_DECL(Induction, "induction")
TSAR_TRAIT_DECL(Flow, "flow")
TSAR_TRAIT_DECL(Anti, "anti")
TSAR_TRAIT_DECL(Output, "output")
TSAR_TRAIT_DECL(Lock, "lock")
TSAR_TRAIT_DECL(Redundant, "redundant")
TSAR_TRAIT_DECL(NoRedundant, "no redundant")
TSAR_TRAIT_DECL(NoPromotedScalar, "no promoted scalar")
TSAR_TRAIT_DECL(DirectAccess, "direct access")
TSAR_TRAIT_DECL(IndirectAccess, "indirect access")
TSAR_TRAIT_DECL(UseAfterLoop, "use after loop")
TSAR_TRAIT_DECL(WriteOccurred, "write")
TSAR_TRAIT_DECL(ReadOccurred, "read")

#undef TSAR_TRAIT_DECL

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Description of a loop-carried dependence.
class Dependence {
public:
  /// List of available bitwise properties.
  enum Flag : uint8_t {
    No = 0,
    /// There is no assurance in existence of a dependence.
    May = 1 << 0,
    /// At least one of dependence causes is load/store to a memory.
    LoadStoreCause = 1u << 1,
    /// At least one of dependence causes is call of a function.
    CallCause = 1u << 2,
    /// At least one of dependence causes is unknown instruction which accesses
    /// a memory.
    UnknownCause = 1u << 3,
    /// At least one of dependence causes is impossibility to analyze some of
    /// surrounding loop nests (for example, unable to check that some
    /// expressions is loop invariant).
    ConfusedCause = 1u << 4,
    /// Distance is unknown.
    UnknownDistance = 1u << 5,
    LLVM_MARK_AS_BITMASK_ENUM(UnknownDistance)
  };

  /// This returns bitwise OR of possible dependence causes.
  static Flag possibleCauses() noexcept {
    return LoadStoreCause | CallCause | UnknownCause | ConfusedCause;
  }

  /// Creates dependence and set its properties to `F`.
  explicit Dependence(Flag F) : mFlags(F) {}

  /// Returns bitwise properties.
  Flag getFlags() const noexcept { return mFlags; }

  /// Returns true if there is no assurance in existence of a dependence.
  bool isMay() const noexcept { return mFlags & May; }

  /// Returns true if at least one of dependence causes is load/store
  /// instruction.
  bool isLoadStore() const noexcept { return mFlags & LoadStoreCause; }

  /// Returns true if all dependence causes are load/store instructions.
  bool isLoadStoreOnly() const noexcept {
    return (mFlags & possibleCauses()) == LoadStoreCause;
  }

  /// Returns true if at least one of dependence causes is call instruction.
  bool isCall() const noexcept { return mFlags & CallCause; }

  /// Returns true if all dependence causes are call instructions.
  bool isCallOnly() const noexcept {
    return (mFlags & possibleCauses()) == CallCause;
  }

  /// Returns true if at least one of dependence causes is unknown access to
  /// a memory.
  bool isUnknown() const noexcept { return mFlags & UnknownCause; }

  /// Returns true if all dependence causes are unknown accesses to a memory.
  bool isUnknownOnly() const noexcept {
    return (mFlags & possibleCauses()) == UnknownCause;
  }

  /// Returns true if at least one of dependence causes is impossibility to
  //// analyze some of surrounding loop nests.
  bool isConfused() const noexcept { return mFlags & ConfusedCause; }

  /// Returns true if all dependence causes is impossibility to
  //// analyze some of surrounding loop nests.
  bool isConfusedOnly() const noexcept {
    return (mFlags & possibleCauses()) == ConfusedCause;
  }

  /// Returns true if both the lowest and highest distances are known.
  bool isKnownDistance() const noexcept { return !(mFlags & UnknownDistance); }

private:
  Flag mFlags;
};

/// Description of a reduction memory location.
class Reduction {
public:
  /// This represents available kinds of a reduction.
  enum Kind : uint8_t {
    RK_First,
    RK_Add = RK_First,
    RK_Mult,
    RK_Or,
    RK_And,
    RK_Xor,
    RK_Max,
    RK_Min,
    RK_NoReduction,
    RK_NumberOf = RK_NoReduction,
  };

  TSAR_TRAIT_DECL_STRING(Reduction, "reduction")

  /// Creates reduction with a specified kind.
  explicit Reduction(Kind RK) : mRK(RK) {}

  /// Returns reduction kind.
  Kind getKind() const noexcept { return mRK; }

  /// Returns true if kind is valid.
  operator bool() const noexcept { return getKind() != RK_NoReduction; }

private:
  Kind mRK;
};
}

/// \brief This represents list of traits for a memory location which can be
/// recognized by analyzer.
///
/// The following information is available:
/// - is it a location which is not accessed in a region;
/// - is it a location which is explicitly accessed in a region;
/// - is it a location address of which is evaluated;
/// - is it a private location;
/// - is it a last private location;
/// - is it a second to last private location;
/// - is it a dynamic private location;
/// - is it a first private location;
/// - is it a read-only location;
/// - is it a shared location;
/// - is it a location that caused dependence;
/// - is it a loop induction location;
/// - is it a location which is accessed in a loop header;
/// - is it a location with locked traits which should not be further analyzed;
/// - is it a redundant location which can be removed after some transformations;
/// - is it a location which is not redundant;
/// - is it a scalar which has not been promoted to registers;
/// - is it a directly accessed location;
/// - is it an indirectly accessed location with known list of locations which
///   are used to access it.
///
/// If location is not accessed in a region it will be marked as 'no access'
/// only if it has some other traits, otherwise it can be omitted in a list
/// of region traits.
///
/// Location is accessed in a region implicitly if descendant of it in an
/// alias tree will be accessed explicitly. If some other location is
/// accessed due to alias with such location it is not treated.
///
/// There are difference of meaning of some traits for a separate memory
/// location and for a node in alias tree. This relates to explicitly accessed,
/// redundant, not redundant and direct locations. An alias node has
/// one of these traits only if it contains at least one location which has an
/// appropriate trait (locations from descendant nodes are not analyzed
/// in this case).
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
using MemoryDescriptor = bcl::TraitDescriptor<
  trait::AddressAccess, trait::ExplicitAccess, trait::HeaderAccess, trait::Lock,
  trait::Redundant, trait::NoRedundant, trait::NoPromotedScalar,
  trait::DirectAccess, trait::IndirectAccess, trait::UseAfterLoop,
  bcl::TraitAlternative<
    trait::NoAccess, trait::Readonly, trait::Reduction, trait::Induction,
    bcl::TraitUnion<trait::Flow, trait::Anti, trait::Output>,
    bcl::TraitUnion<trait::Private, trait::Shared>,
    bcl::TraitUnion<trait::LastPrivate, trait::FirstPrivate, trait::Shared>,
    bcl::TraitUnion<trait::SecondToLastPrivate, trait::FirstPrivate, trait::Shared>,
    bcl::TraitUnion<trait::DynamicPrivate, trait::FirstPrivate, trait::Shared>>>;

/// Return true if there is no any dependencies.
inline bool hasNoDep(const MemoryDescriptor &Dptr) {
  return Dptr.is_any<trait::NoAccess, trait::Readonly, trait::Shared>();
}

/// Return true if there is spurious dependence which could be removed in some
/// cases.
inline bool hasSpuriousDep(const MemoryDescriptor &Dptr) {
  return !hasNoDep(Dptr) &&
           !Dptr.is_any<trait::Flow, trait::Anti, trait::Output>() ||
         bcl::ForwardTypeList<
           bcl::trait::IsAny,
           bcl::RemoveFromTypeList<
             trait::Shared,
             bcl::RemoveDuplicate<bcl::ForwardTypeList<
               bcl::TraitList,
               bcl::trait::find_union_t<
                 trait::Shared, MemoryDescriptor>>::Type::Type>::Type>::
                   Type>::Type::is_any(Dptr);
}

/// Statistic of collected memory traits.
using MemoryStatistic = bcl::tagged_tuple<
  bcl::tagged<llvm::Statistic &, trait::AddressAccess>,
  bcl::tagged<llvm::Statistic &, trait::ExplicitAccess>,
  bcl::tagged<llvm::Statistic &, trait::HeaderAccess>,
  bcl::tagged<llvm::Statistic &, trait::Readonly>,
  bcl::tagged<llvm::Statistic &, trait::Shared>,
  bcl::tagged<llvm::Statistic &, trait::Private>,
  bcl::tagged<llvm::Statistic &, trait::FirstPrivate>,
  bcl::tagged<llvm::Statistic &, trait::SecondToLastPrivate>,
  bcl::tagged<llvm::Statistic &, trait::LastPrivate>,
  bcl::tagged<llvm::Statistic &, trait::DynamicPrivate>,
  bcl::tagged<llvm::Statistic &, trait::Reduction>,
  bcl::tagged<llvm::Statistic &, trait::Induction>,
  bcl::tagged<llvm::Statistic &, trait::Flow>,
  bcl::tagged<llvm::Statistic &, trait::Anti>,
  bcl::tagged<llvm::Statistic &, trait::Output>,
  bcl::tagged<llvm::Statistic &, trait::Lock>,
  bcl::tagged<llvm::Statistic &, trait::Redundant>,
  bcl::tagged<llvm::Statistic &, trait::NoRedundant>,
  bcl::tagged<llvm::Statistic &, trait::NoPromotedScalar>,
  bcl::tagged<llvm::Statistic &, trait::DirectAccess>,
  bcl::tagged<llvm::Statistic &, trait::IndirectAccess>,
  bcl::tagged<llvm::Statistic &, trait::UseAfterLoop>>;

/// A macro to make definition of statistics really simple.
///
/// This automatically passes the DEBUG_TYPE of the file into the statistic.
/// Note that this macro defines static variable VARNAME and a list of variables
/// with VARNAME##<trait kind> names.
#define MEMORY_TRAIT_STATISTIC(VARNAME) \
  STATISTIC(VARNAME##AddressAccess, "Number of locations address of which is evaluated"); \
  STATISTIC(VARNAME##ExplicitAccess, "Number of explicitly accessed locations"); \
  STATISTIC(VARNAME##HeaderAccess, "Number of traits caused by memory accesses in a loop header"); \
  STATISTIC(VARNAME##Readonly, "Number of read-only locations found"); \
  STATISTIC(VARNAME##Shared, "Number of shared locations found"); \
  STATISTIC(VARNAME##Private, "Number of private locations found"); \
  STATISTIC(VARNAME##FirstPrivate, "Number of first private locations found"); \
  STATISTIC(VARNAME##SecondToLastPrivate, "Number of second-to-last-private locations found"); \
  STATISTIC(VARNAME##LastPrivate, "Number of last private locations found"); \
  STATISTIC(VARNAME##DynamicPrivate, "Number of dynamic private locations found"); \
  STATISTIC(VARNAME##Reduction, "Number of reductions found"); \
  STATISTIC(VARNAME##Induction, "Number of inductions found"); \
  STATISTIC(VARNAME##Flow, "Number of flow dependencies found"); \
  STATISTIC(VARNAME##Anti, "Number of anti dependencies found"); \
  STATISTIC(VARNAME##Output, "Number of output dependencies found"); \
  STATISTIC(VARNAME##Lock, "Number of locked traits"); \
  STATISTIC(VARNAME##Redundant, "Number of redundant locations"); \
  STATISTIC(VARNAME##NoRedundant, "Number of not redundant locations"); \
  STATISTIC(VARNAME##NoPromotedScalar, "Number of not promoted scalars"); \
  STATISTIC(VARNAME##DirectAccess, "Number of not directly accessed locations"); \
  STATISTIC(VARNAME##IndirectAccess, "Number of not indirectly accessed locations"); \
  STATISTIC(VARNAME##UseAfterLoop, "Number of locations used after exit from a loop"); \
  static ::tsar::MemoryStatistic VARNAME = {                                   \
      VARNAME##AddressAccess, VARNAME##HeaderAccess, VARNAME##ExplicitAccess,  \
      VARNAME##Readonly, VARNAME##Shared, VARNAME##Private,                    \
      VARNAME##FirstPrivate, VARNAME##SecondToLastPrivate,                     \
      VARNAME##LastPrivate, VARNAME##DynamicPrivate, VARNAME##Reduction,       \
      VARNAME##Induction, VARNAME##Flow, VARNAME##Anti, VARNAME##Output,       \
      VARNAME##Lock, VARNAME##Redundant, VARNAME##NoRedundant,                 \
      VARNAME##NoPromotedScalar, VARNAME##DirectAccess,                        \
      VARNAME##IndirectAccess, VARNAME##UseAfterLoop                           \
  };
}
#endif//TSAR_MEMORY_TRAIT_H

