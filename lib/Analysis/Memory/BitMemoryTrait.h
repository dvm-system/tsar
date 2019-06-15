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
  /// Readonly & LastPrivate = 0011001 = LastPrivate & FirstPrivate. So if some
  /// part of memory locations is read-only and other part is last private a union
  /// is last private and first private (for details see resolve... methods).
  enum Id : unsigned long long {
    NoAccess =            1111111111_b,
    Readonly =            0011110111_b,
    Shared =              0011110011_b,
    Private =             0001111111_b,
    FirstPrivate =        0001110111_b,
    SecondToLastPrivate = 0001011111_b,
    LastPrivate =         0000111111_b,
    DynamicPrivate =      0000011111_b,
    Dependency =          0000000011_b,
    AddressAccess =       1111111101_b,
    HeaderAccess =        1111111110_b,
    Reduction =           0100000011_b,
    Induction =           1000000011_b,
  };

  BitMemoryTrait() = default;
  BitMemoryTrait(Id Id) noexcept : mId(static_cast<decltype(mId)>(Id)) {}
  BitMemoryTrait & operator=(Id Id) noexcept {
    return *this = BitMemoryTrait(Id);
  }
  BitMemoryTrait & operator&=(const BitMemoryTrait &With) noexcept {
    mId &= With.mId;
    return *this;
  }
  BitMemoryTrait & operator|=(const BitMemoryTrait &With) noexcept {
    mId != With.mId;
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

/// Drops bits which identifies single-bit traits.
constexpr inline BitMemoryTrait::Id dropUnitFlag(
    BitMemoryTrait::Id T) noexcept {
  return T | ~BitMemoryTrait::AddressAccess | ~BitMemoryTrait::HeaderAccess;
}

/// Drops a single bit which identifies shared trait (shared becomes read-only).
constexpr inline BitMemoryTrait::Id dropSharedFlag(
    BitMemoryTrait::Id T) noexcept {
  return T | ~(~BitMemoryTrait::Readonly | BitMemoryTrait::Shared);
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
