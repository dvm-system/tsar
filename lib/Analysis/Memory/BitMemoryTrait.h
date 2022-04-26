//=== BitMemoryTrait.h - Bitwise Representation of Memory Traits -*- C++ -*===//
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
// This file defines bitwise representation of memory traits.
// It is easy to join different traits. For example,
// Readonly & LastPrivate = 0011001 = LastPrivate & FirstPrivate.
// So if some part of memory locations is read-only and other part is
// last private a union is last private and first private.
//
// This is a helpful enumeration which must not be used outside the dependence
// analysis passes. Also, avoid usage this file in other include files.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_BIT_MEMORY_TRAIT_H
#define TSAR_BIT_MEMORY_TRAIT_H

#include "tsar/Analysis/Memory/MemoryTrait.h"
#include <type_traits>

namespace tsar {
using bcl::operator "" _b;

/// Bitwise representation of traits of memory locations.
class BitMemoryTrait {
public:
  /// \brief Identifiers of recognized traits.
  ///
  /// It is easy to join different traits. For example,
  /// `Readonly & LastPrivate = LastPrivate & FirstPrivate`. So if some part
  /// of memory locations is read-only and other part is last private a union
  /// is last private and first private (for details see resolve... methods).
  ///
  /// Some flags does not depends from other flags (unit flags) all such flags
  /// could be dropped of using 'dropUnitFlag()' method.
  ///
  /// There is a special flag `SharedJoin` it should be used with other flags
  /// to mark absence of dependence. For example Private | SharedJoin means
  /// that a privatizable variable is not caused dependency. Use `|` with
  /// `SharedJoin` instead of `&`. To drop it use `dropSharedFlag()` method
  /// (note, this method change NoAccess, Readonly and Shared to invalid flags).
  enum Id : unsigned long long {
    NoAccess =            1111111111111111111_b,
    Readonly =            1001111011111111111_b,
    SharedJoin =          0000000011111111111_b,
    Shared =              1000001011111111111_b,
    Private =             0000111101111111111_b,
    FirstPrivate =        0000111001111111111_b,
    SecondToLastPrivate = 0000101101111111111_b,
    LastPrivate =         0000011101111111111_b,
    DynamicPrivate =      0000001101111111111_b,
    Dependency =          0000000001111111111_b,
    Reduction =           0010000001111111111_b,
    Induction =           0100000001111111111_b,
    AddressAccess =       1111111110111111111_b,
    HeaderAccess =        1111111111011111111_b,
    ExplicitAccess =      1111111111101111111_b,
    Lock =                1111111111110111111_b,
    Redundant =           1111111111111011111_b,
    NoRedundant =         1111111111111101111_b,
    NoPromotedScalar =    1111111111111110111_b,
    DirectAccess =        1111111111111111011_b,
    IndirectAccess =      1111111111111111101_b,
    UseAfterLoop =        1111111111111111110_b,
    AllUnitFlags =        1111111110000000000_b,
  };

  BitMemoryTrait() = default;
  BitMemoryTrait(Id Id) noexcept : mId(static_cast<decltype(mId)>(Id)) {}

  BitMemoryTrait(const MemoryDescriptor &Dptr);

  BitMemoryTrait & operator=(Id Id) noexcept {
    return *this = BitMemoryTrait(Id);
  }
  BitMemoryTrait & operator=(const MemoryDescriptor &Dptr) {
    return *this = BitMemoryTrait(Dptr);
  }

  BitMemoryTrait & operator&=(const BitMemoryTrait &With) noexcept {
    mId &= With.mId;
    return *this;
  }

  BitMemoryTrait & operator&=(const MemoryDescriptor &With) noexcept {
    mId &= BitMemoryTrait(With).mId;
    return *this;
  }

  BitMemoryTrait & operator|=(const BitMemoryTrait &With) noexcept {
    mId |= With.mId;
    return *this;
  }

  BitMemoryTrait & operator|=(const MemoryDescriptor &With) noexcept {
    mId |= BitMemoryTrait(With).mId;
    return *this;
  }

  bool operator!() const noexcept { return !mId; }
  operator Id () const noexcept { return get(); }
  Id get() const noexcept { return static_cast<Id>(mId); }

  /// Convert internal representation of a trait to a dependency descriptor.
  ///
  /// This method also calculates statistic which proposes number of determined
  /// traits. If different locations has the same traits TraitNumber parameter
  /// can be used to take into account all traits. It also can be set to 0
  /// if a specified trait should not be counted.
  MemoryDescriptor toDescriptor(
    unsigned TraitNumber, MemoryStatistic &Stat) const;
private:
  std::underlying_type<Id>::type mId = NoAccess;
};

constexpr inline BitMemoryTrait::Id operator&(
    BitMemoryTrait::Id LHS, BitMemoryTrait::Id RHS) noexcept {
  using Id = BitMemoryTrait::Id;
  return static_cast<Id>(
    static_cast<std::underlying_type<Id>::type>(LHS) &
    static_cast<std::underlying_type<Id>::type>(RHS));
}
constexpr inline BitMemoryTrait::Id operator|(
  BitMemoryTrait::Id LHS, BitMemoryTrait::Id RHS) noexcept {
  using Id = BitMemoryTrait::Id;
  return static_cast<Id>(
    static_cast<std::underlying_type<Id>::type>(LHS) |
    static_cast<std::underlying_type<Id>::type>(RHS));
}
constexpr inline BitMemoryTrait::Id operator~(
    BitMemoryTrait::Id What) noexcept {
  using Id = BitMemoryTrait::Id;
  // We use `... & NoAccess` to avoid reversal of unused bits.
  return static_cast<Id>(
    ~static_cast<std::underlying_type<Id>::type>(What) &
      BitMemoryTrait::NoAccess);
}

/// Drop bits which identifies single-bit traits.
constexpr inline BitMemoryTrait::Id dropUnitFlag(
    BitMemoryTrait::Id T) noexcept {
  return T | ~BitMemoryTrait::AllUnitFlags;
}

/// Drop a single bit (equal to 1) which identifies shared trait.
constexpr inline BitMemoryTrait::Id dropSharedFlag(
    BitMemoryTrait::Id T) noexcept {
  return T & dropUnitFlag(~BitMemoryTrait::SharedJoin);
}

/// Check a single bit (equal to 1) which identifies shared trait.
constexpr inline bool hasSharedJoin(BitMemoryTrait::Id T) noexcept {
  return T & BitMemoryTrait::SharedJoin & BitMemoryTrait::AllUnitFlags;
}

/// Convert internal representation of a trait to a dependency descriptor.
///
/// This method also calculates statistic which proposes number of determined
/// traits. If different locations has the same traits TraitNumber parameter
/// can be used to take into account all traits. It also can be set to 0
/// if a specified trait should not be counted.
inline MemoryDescriptor toDescriptor(const BitMemoryTrait &T,
    unsigned TraitNumber, MemoryStatistic &Stat) {
  return T.toDescriptor(TraitNumber, Stat);
}
}
#endif//TSAR_BIT_MEMORY_TRAIT_H
