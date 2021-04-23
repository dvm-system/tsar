//===- MemoryLocationRange.h -- Memory Location Range -----------*- C++ -*-===//
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
// This file provides utility analysis objects describing memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_LOCATION_RANGE_H
#define TSAR_MEMORY_LOCATION_RANGE_H

#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/MemoryLocation.h>

namespace tsar {

using LocationSize = llvm::LocationSize;

/// Representation for a memory location with shifted start position.
///
/// The difference from llvm::MemoryLocation is that the current location
/// starts at `Ptr + LowerBound` address. In case of llvm::MemoryLocation
/// LowerBound is always 0.
struct MemoryLocationRange {
  enum : uint64_t { UnknownSize = llvm::MemoryLocation::UnknownSize };
  enum LocKind {
    FIRST_KIND = 0,
    DEFAULT = FIRST_KIND,
    NON_COLLAPSABLE,
    COLLAPSED,
    EXPLICIT,
    LAST_KIND = EXPLICIT,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND,
  };

  struct Dimension {
    uint64_t Start;
    uint64_t Step;
    uint64_t TripCount;
    Dimension() : Start(0), Step(0), TripCount(0) {}
    inline bool operator==(const Dimension &Other) const {
      return Start == Other.Start && Step == Other.Step &&
             TripCount == Other.TripCount;
    }
  };

  const llvm::Value * Ptr;
  LocationSize LowerBound;
  LocationSize UpperBound;
  llvm::SmallVector<Dimension, 0> DimList;
  llvm::AAMDNodes AATags;
  LocKind Kind;

  /// Return a location with information about the memory reference by the given
  /// instruction.
  static MemoryLocationRange get(const llvm::LoadInst *LI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(LI));
  }
  static MemoryLocationRange get(const llvm::StoreInst *SI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(SI));
  }
  static MemoryLocationRange get(const llvm::VAArgInst *VI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(VI));
  }
  static MemoryLocationRange get(const llvm::AtomicCmpXchgInst *CXI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(CXI));
  }
  static MemoryLocationRange get(const llvm::AtomicRMWInst *RMWI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(RMWI));
  }
  static MemoryLocationRange get(const llvm::Instruction *Inst) {
    return *MemoryLocationRange::getOrNone(Inst);
  }

  static llvm::Optional<MemoryLocationRange> getOrNone(
      const llvm::Instruction *Inst) {
    auto Loc = llvm::MemoryLocation::getOrNone(Inst);
    if (Loc)
      return MemoryLocationRange(*Loc);
    else
      return llvm::None;
  }

  /// Return a location representing the source of a memory transfer.
  static MemoryLocationRange getForSource(const llvm::MemTransferInst *MTI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForSource(MTI));
  }
  static MemoryLocationRange getForSource(
      const llvm::AtomicMemTransferInst *MTI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForSource(MTI));
  }
  static MemoryLocationRange getForSource(const llvm::AnyMemTransferInst *MTI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForSource(MTI));
  }

  /// Return a location representing the destination of a memory set or
  /// transfer.
  static MemoryLocationRange getForDest(const llvm::MemIntrinsic *MI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForDest(MI));
  }
  static MemoryLocationRange getForDest(const llvm::AtomicMemIntrinsic *MI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForDest(MI));
  }
  static MemoryLocationRange getForDest(const llvm::AnyMemIntrinsic *MI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForDest(MI));
  }

  /// Return a location representing a particular argument of a call.
  static MemoryLocationRange getForArgument(const llvm::CallBase *Call,
      unsigned ArgIdx, const llvm::TargetLibraryInfo &TLI) {
    return MemoryLocationRange(
      llvm::MemoryLocation::getForArgument(Call, ArgIdx, TLI));
  }

  explicit MemoryLocationRange(const llvm::Value *Ptr = nullptr,
                               LocationSize LowerBound = 0,
                               LocationSize UpperBound = UnknownSize,
                               const llvm::AAMDNodes &AATags = llvm::AAMDNodes())
      : Ptr(Ptr), LowerBound(LowerBound), UpperBound(UpperBound),
        AATags(AATags), Kind(LocKind::DEFAULT) {}

  explicit MemoryLocationRange(const llvm::Value *Ptr,
                               LocationSize LowerBound,
                               LocationSize UpperBound,
                               LocKind Kind,
                               const llvm::AAMDNodes &AATags = llvm::AAMDNodes())
      : Ptr(Ptr), LowerBound(LowerBound), UpperBound(UpperBound),
        AATags(AATags), Kind(Kind) {}

  MemoryLocationRange(const llvm::MemoryLocation &Loc)
      : Ptr(Loc.Ptr), LowerBound(0), UpperBound(Loc.Size), AATags(Loc.AATags),
        Kind(LocKind::DEFAULT) {}

  MemoryLocationRange(const MemoryLocationRange &Loc)
      : Ptr(Loc.Ptr), LowerBound(Loc.LowerBound), UpperBound(Loc.UpperBound),
        AATags(Loc.AATags), DimList(Loc.DimList), Kind(Loc.Kind) {}

  MemoryLocationRange &operator=(const llvm::MemoryLocation &Loc) {
    Ptr = Loc.Ptr;
    LowerBound = 0;
    UpperBound = Loc.Size;
    AATags = Loc.AATags;
    return *this;
  }

  bool operator==(const MemoryLocationRange &Other) const {
    return Ptr == Other.Ptr && AATags == Other.AATags &&
      LowerBound == Other.LowerBound && UpperBound == Other.UpperBound &&
      DimList == Other.DimList && Kind == Other.Kind;
  }
};

namespace MemoryLocationRangeEquation {
  typedef int64_t ColumnT;
  typedef int64_t ValueT;
  typedef MemoryLocationRange::Dimension Dimension;

  struct ColumnInfo {
    std::array<char, 3> Variables = {'X', 'Y', 'T'};

    template<typename T> T get(ColumnT Column) const { return 0; }
    ColumnT parameterColumn() { return 2; }
    bool isParameter(ColumnT Column) const { return Column > 1; }
    char name(ColumnT Column) const { return Variables[Column]; }
  };

  /// \brief Finds an intersection between memory locations LHS and RHS, one of 
  /// which can be scalar.
  ///
  /// \param [in] LHS The first location to intersect.
  /// \param [in] RHS The second location to intersect.
  /// \param [out] LC List of memory locations to store the difference 
  /// between locations LHS and Int. It will not be changed if the intersection
  /// is empty. If `LC == nullptr`, the difference will not be calculated and
  /// will not be stored anywhere.
  /// \param [out] RC List of memory locations to store the difference 
  /// between locations RHS and Int. It will not be changed if the intersection
  /// is empty. If `RC == nullptr`, the difference will not be calculated and
  /// will not be stored anywhere.
  /// \param [out] Threshold The maximum number of locations that can be 
  /// obtained as a result of calculating the differences. If it is exceeded, 
  /// exact differences will not be saved.
  /// \return The result of intersection. If it is None, intersection is empty.
  /// If it is a location, but DimList of the returned location is empty, then
  /// the intersection may exist but can't be calculated. Otherwise, the
  /// returned location is an exact intersection.
  llvm::Optional<MemoryLocationRange> intersect(
      const MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS,
      llvm::SmallVectorImpl<MemoryLocationRange> *LC = nullptr,
      llvm::SmallVectorImpl<MemoryLocationRange> *RC = nullptr,
      std::size_t Threshold = 10);
}
}

namespace llvm {
// Specialize DenseMapInfo for tsar::MemoryLocationRange.
template <> struct DenseMapInfo<tsar::MemoryLocationRange> {
  static inline tsar::MemoryLocationRange getEmptyKey() {
    return tsar::MemoryLocationRange(
      DenseMapInfo<const Value *>::getEmptyKey(), 0);
  }
  static inline tsar::MemoryLocationRange getTombstoneKey() {
    return tsar::MemoryLocationRange(
      DenseMapInfo<const Value *>::getTombstoneKey(), 0);
  }
  static unsigned getHashValue(const tsar::MemoryLocationRange &Val) {
    return DenseMapInfo<const Value *>::getHashValue(Val.Ptr) ^
           DenseMapInfo<LocationSize>::getHashValue(Val.LowerBound) ^
           DenseMapInfo<LocationSize>::getHashValue(Val.UpperBound) ^
           DenseMapInfo<AAMDNodes>::getHashValue(Val.AATags);
  }
  static bool isEqual(const MemoryLocation &LHS, const MemoryLocation &RHS) {
    return LHS == RHS;
  }
};
}
#endif//TSAR_MEMORY_LOCATION_RANGE_H
