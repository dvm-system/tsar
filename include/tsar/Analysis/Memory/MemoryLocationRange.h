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

#include "tsar/Analysis/Memory/AssumptionInfo.h"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/Analysis/MemoryLocation.h>

namespace llvm {
class ScalarEvolution;
class SCEV;
}

namespace tsar {

using LocationSize = llvm::LocationSize;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Representation for a memory location with shifted start position.
///
/// The difference from llvm::MemoryLocation is that the current location
/// starts at `Ptr + LowerBound` address. In case of llvm::MemoryLocation
/// LowerBound is always 0.
struct MemoryLocationRange {
  enum : uint64_t { UnknownSize = llvm::MemoryLocation::UnknownSize };
  // There are several kinds of memory locations:
  // * Default - the kind of memory locations corresponding to scalar variables
  // or array memory locations can potentially be collapsed later.
  // * NonCollapsable - the kind of memory locations that are not scalar, for
  // which it has already been determined that collapse cannot be performed.
  // * Collapsed - the kind of collapsed memory locations.
  // * Hint - the kind of memory locations artificially added for correct
  // private variables analysis.
  // * Auxiliary - the kind of memory locations that intersect with some other
  // memory location and are added to MemorySet as auxiliary, in order to take
  // into account the fact that the same memory is being accessed.
  enum LocKind : uint8_t {
    Default = 0,
    NonCollapsable = 1u << 0,
    Collapsed = 1u << 1,
    Hint = 1u << 2,
    Auxiliary = 1u << 3,
    LLVM_MARK_AS_BITMASK_ENUM(Auxiliary)
  };

  struct Dimension {
    const llvm::SCEV *Start;
    const llvm::SCEV *End;
    const llvm::SCEV *Step;
    uint64_t DimSize;
    Dimension() : Start(nullptr), End(nullptr), Step(nullptr), DimSize(0) {}
    inline bool operator==(const Dimension &Other) const {
      return Start == Other.Start && End == Other.End && Step == Other.Step &&
             DimSize == Other.DimSize;
    }
    void print(llvm::raw_ostream &OS, bool IsDebug = false) const;
  };

  const llvm::Value * Ptr;
  LocationSize LowerBound;
  LocationSize UpperBound;
  llvm::SmallVector<Dimension, 0> DimList;
  llvm::AAMDNodes AATags;
  LocKind Kind;
  llvm::ScalarEvolution *SE;
  const AssumptionMap *AM;

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
        AATags(AATags), SE(nullptr), AM(nullptr), Kind(LocKind::Default) {}

  explicit MemoryLocationRange(const llvm::Value *Ptr,
                               LocationSize LowerBound,
                               LocationSize UpperBound,
                               LocKind Kind,
                               llvm::ScalarEvolution *SE,
                               AssumptionMap *AM,
                               const llvm::AAMDNodes &AATags = llvm::AAMDNodes())
      : Ptr(Ptr), LowerBound(LowerBound), UpperBound(UpperBound), Kind(Kind),
        SE(SE), AM(AM), AATags(AATags) {}

  MemoryLocationRange(const llvm::MemoryLocation &Loc)
      : Ptr(Loc.Ptr), LowerBound(0), UpperBound(Loc.Size), AATags(Loc.AATags),
        Kind(LocKind::Default), SE(nullptr), AM(nullptr) {}

  MemoryLocationRange(const MemoryLocationRange &Loc)
      : Ptr(Loc.Ptr), LowerBound(Loc.LowerBound), UpperBound(Loc.UpperBound),
        AATags(Loc.AATags), DimList(Loc.DimList), Kind(Loc.Kind), SE(Loc.SE),
        AM(Loc.AM) {}

  MemoryLocationRange &operator=(const llvm::MemoryLocation &Loc) {
    Ptr = Loc.Ptr;
    LowerBound = 0;
    UpperBound = Loc.Size;
    AATags = Loc.AATags;
    Kind = Default;
    SE = nullptr;
    AM = nullptr;
    return *this;
  }

  bool operator==(const MemoryLocationRange &Other) const {
    return Ptr == Other.Ptr && AATags == Other.AATags &&
      LowerBound == Other.LowerBound && UpperBound == Other.UpperBound &&
      DimList == Other.DimList && SE == Other.SE && AM == Other.AM &&
      Kind == Other.Kind;
  }

  std::string getKindAsString() const {
    auto append = [](const std::string &What, std::string &To) {
      if (!To.empty())
        To += " | ";
      To += What;
    };
    std::string KindStr;
    if (Kind == Hint || Kind == Default)
      append("Default", KindStr);
    if (Kind & NonCollapsable)
      append("NonCollapsable", KindStr);
    if (Kind & Collapsed)
      append("Collapsed", KindStr);
    if (Kind & Hint)
      append("Hint", KindStr);
    if (Kind & Auxiliary)
      append("Auxiliary", KindStr);
    return KindStr;
  }

  /// If the location is associated with an array, i.e. it has a non-empty list
  /// of dimensions, convert it to an ordinary location with an empty list of
  /// dimensions and return it. The new location covers the entire memory of 
  /// the array, even though the dimensions are not completely used. 
  MemoryLocationRange expand() const {
    if (DimList.empty())
      return *this;
    assert(UpperBound.hasValue() && "UpperBound must have a value!");
    auto FullSize = UpperBound.getValue();;
    for (std::size_t I = 1; I < DimList.size(); ++I)
      FullSize *= DimList[I].DimSize;
    return MemoryLocationRange(Ptr, 0, LocationSize(FullSize), AATags);
  }
};

/// \brief Finds an intersection between memory locations LHS and RHS.
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
/// If it is a location, but `Ptr` of the returned location is `nullptr`, then
/// the intersection may exist but can't be calculated (note that you will get
/// the same result if the intersection is exact but LC or RC is not `nullptr`
/// and we can't find the exact differences). Otherwise, the returned location 
/// is an exact intersection. 
llvm::Optional<MemoryLocationRange> intersect(
    MemoryLocationRange LHS,
    MemoryLocationRange RHS,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC = nullptr,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC = nullptr,
    unsigned Threshold = 10);
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
