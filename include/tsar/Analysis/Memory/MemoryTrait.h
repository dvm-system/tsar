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
#include <llvm/ADT/BitmaskEnum.h>
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
TSAR_TRAIT_DECL(Private, "private")
TSAR_TRAIT_DECL(FirstPrivate, "first private")
TSAR_TRAIT_DECL(SecondToLastPrivate, "second to last private")
TSAR_TRAIT_DECL(LastPrivate, "last private")
TSAR_TRAIT_DECL(DynamicPrivate, "dynamic private")
TSAR_TRAIT_DECL(Reduction, "reduction")
TSAR_TRAIT_DECL(Induction, "induction")
TSAR_TRAIT_DECL(Flow, "flow")
TSAR_TRAIT_DECL(Anti, "anti")
TSAR_TRAIT_DECL(Output, "output")

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
    /// Distance is unknown.
    UnknownDistance = 1u << 4,
    LLVM_MARK_AS_BITMASK_ENUM(UnknownDistance)
  };

  /// This returns bitwise OR of possible dependence causes.
  static Flag possibleCauses() noexcept {
    return LoadStoreCause | CallCause | UnknownCause;
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

  /// Returns true if both the lowest and highest distances are known.
  bool isKnownDistance() const noexcept { return !(mFlags & UnknownDistance); }

private:
  Flag mFlags;
};
}

/// \brief This represents list of traits for a memory location which can be
/// recognized by analyzer.
///
/// The following information is available:
/// - is it a location which is not accessed in a region
/// - is it a location which is explicitly accessed in a region
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
/// - is it a location which is accessed in a loop header.
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
using MemoryDescriptor = bcl::TraitDescriptor<
  trait::AddressAccess, trait::ExplicitAccess, trait::HeaderAccess,
  bcl::TraitAlternative<
    trait::NoAccess, trait::Readonly, trait::Reduction, trait::Induction,
    bcl::TraitUnion<trait::Flow, trait::Anti, trait::Output>,
    bcl::TraitUnion<trait::Private, trait::Shared>,
    bcl::TraitUnion<trait::LastPrivate, trait::FirstPrivate, trait::Shared>,
    bcl::TraitUnion<trait::SecondToLastPrivate, trait::FirstPrivate, trait::Shared>,
    bcl::TraitUnion<trait::DynamicPrivate, trait::FirstPrivate, trait::Shared>>>;
}
#endif//TSAR_MEMORY_TRAIT_H
