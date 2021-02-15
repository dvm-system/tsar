//===--- MemorySetInfo.h - Type traits for MemorySet ------------*- C++ -*-===//
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
// This file defines MemorySetInfo traits for MemorySet.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_SET_INFO_H
#define TSAR_MEMORY_SET_INFO_H

#include "tsar/Analysis/Memory/MemoryLocationRange.h"

namespace tsar {
/// Provide traits for objects stored in a MemorySet.
///
/// Each object in a set is a memory location which starts and ends at specified
/// positions: [address + offset (lower bound), address + offset (upper bound)].
/// The following methods should be provided:
/// - static const llvm::Value * getPtr(const Ty &) -
///     Return pointer to the beginning of a memory location.
/// - static SizeTy getLowerBound(const Ty &) -
///     Return the lower bound (offset) of a memory location.
/// - static SizeTy getUpperBound(const Ty &) -
///     Return the upper bound (offset) of a memory location.
/// - static int8_t sizecmp(SizeT LHS, SizeT RHS) -
///     Return a negative value if LHS is less than RHS,
///            zero if sizes are equal and
///            a positive value if LHS is greater than RHS.
///  - static SizeT sizeinp(SizeT) -
///     Return the incremented size of a memory location.
/// - static llvm::AAMDNodes & getAATags(const Ty &) -
///     Return the metadata nodes which describes the aliasing of the location.
/// - static void setLowerBound(SizeTy, LocationTy &)
///     Set the lower bound for a specified location.
/// - static void setUpperBound(SizeTy, LocationTy &)
///     Set the upper bound for a specified location.
/// - static void setAATags(const llvm::AAMDNodes &, LocationTy &)
///     Set the metadata nodes for a specified location.
/// - static LocationTy make(Ty &)
///     Construct new memory location from the other one.
/// - Copy-constructor must be also available.
/// In methods presented above the following denotements are used:
/// - LocationTy is a type of memory locations which are stored in memory set.
/// - Ty is a type of memory locations which is used for insertion the new one
///   and for search the existent locations. It should be also possible to use
///   LocationTy as a Ty.
/// - SizeTy is a type which is suitable to represent size of memory location.
template<class Ty> struct MemorySetInfo {
  typedef typename Ty::UnknownMemoryLocationError LocationTy;
};

/// Provide specialization of MemorySetInfo for llvm::MemoryLocation
template<> struct MemorySetInfo<llvm::MemoryLocation> {
  static inline const llvm::Value * getPtr(
    const llvm::MemoryLocation &Loc) noexcept {
    return Loc.Ptr;
  }
  static inline llvm::LocationSize getLowerBound(
      const llvm::MemoryLocation &Loc) noexcept {
    return 0;
  }
  static inline void setLowerBound(
      llvm::LocationSize Size, llvm::MemoryLocation &Loc) noexcept {
  }
  static inline llvm::LocationSize getUpperBound(
      const llvm::MemoryLocation &Loc) noexcept {
    return Loc.Size;
  }
  static inline void setUpperBound(
      llvm::LocationSize Size, llvm::MemoryLocation &Loc)  noexcept {
    Loc.Size = Size;
  }
  static inline int8_t sizecmp(llvm::LocationSize LHS, llvm::LocationSize RHS) {
    if (LHS == RHS || !LHS.hasValue() && !RHS.hasValue())
      return 0;
    if (!LHS.hasValue())
      return 1;
    if (!RHS.hasValue())
      return -1;
    return LHS.getValue() < RHS.getValue() ? -1 :
      LHS.getValue() == RHS.getValue() ? 0 : 1;
  }
  static inline llvm::LocationSize sizeinc(llvm::LocationSize Size) {
    if (Size.isPrecise())
      return LocationSize::precise(Size.getValue() + 1);
    if (!Size.hasValue())
      return Size;
    return Size.upperBound(Size.getValue() + 1);
  }
  static inline const llvm::AAMDNodes & getAATags(
      const llvm::MemoryLocation &Loc) noexcept {
    return Loc.AATags;
  }
  static inline void setAATags(
     const llvm::AAMDNodes &AATags, llvm::MemoryLocation &Loc) noexcept {
    Loc.AATags = AATags;
  }
  static inline llvm::MemoryLocation make(const llvm::MemoryLocation &Loc) {
    return llvm::MemoryLocation(Loc);
  }
  static inline uint64_t getNumDims(
      const llvm::MemoryLocation &Loc) noexcept {
    return 0;
  }
  static inline bool areJoinable(
      const llvm::MemoryLocation &LHS, const llvm::MemoryLocation &RHS) {
    return sizecmp(getUpperBound(LHS), getLowerBound(RHS)) >= 0 &&
        sizecmp(getLowerBound(LHS), getUpperBound(RHS)) <= 0;
  }
  static inline bool join(llvm::MemoryLocation &LHS,
      const llvm::MemoryLocation &RHS) {
    bool isChanged = false;
    if (sizecmp(getUpperBound(LHS), getUpperBound(RHS)) < 0) {
      setUpperBound(getUpperBound(RHS), LHS);
      isChanged = true;
    }
    if (sizecmp(getLowerBound(LHS), getLowerBound(RHS)) > 0) {
      setLowerBound(getLowerBound(RHS), LHS);
      isChanged = true;
    }
    return isChanged;
  }
};

/// Provide specialization of MemorySetInfo for tsar::MemoryLocationRange
template<> struct MemorySetInfo<MemoryLocationRange> {
  static inline const llvm::Value * getPtr(
    const MemoryLocationRange &Loc) noexcept {
    return Loc.Ptr;
  }
  static inline LocationSize getLowerBound(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.Start;
  }
  static inline void setLowerBound(
      LocationSize Size, MemoryLocationRange &Loc) noexcept {

    Loc.Start = Size;
  }
  static inline LocationSize getUpperBound(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.getEnd();
  }
  static inline void setUpperBound(
      LocationSize Size, MemoryLocationRange &Loc) noexcept {
    return Loc.setEnd(Size);
  }
  static inline int8_t sizecmp(llvm::LocationSize LHS, llvm::LocationSize RHS) {
    return MemorySetInfo<llvm::MemoryLocation>::sizecmp(LHS, RHS);
  }
  static inline llvm::LocationSize sizeinc(llvm::LocationSize Size) {
    return MemorySetInfo<llvm::MemoryLocation>::sizeinc(Size);
  }
  static inline const llvm::AAMDNodes & getAATags(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.AATags;
  }
  static inline void setAATags(
      const llvm::AAMDNodes &AATags,  MemoryLocationRange &Loc) noexcept {
    Loc.AATags = AATags;
  }
  static inline MemoryLocationRange make(const MemoryLocationRange &Loc) {
    return MemoryLocationRange(Loc);
  }
  static inline uint64_t getNumDims(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.DimList.size();
  }
  /*static inline bool areJoinable(
      const MemoryLocationRange &LHS, const MemoryLocationRange &RHS) {
    if (LHS.DimList.size() != RHS.DimList.size())
      return false;
    if (LHS.DimList.size() == 0) {
      return sizecmp(getUpperBound(LHS), getLowerBound(RHS)) >= 0 &&
        sizecmp(getLowerBound(LHS), getUpperBound(RHS)) <= 0;
    }
    for (size_t I = 0; I < LHS.DimList.size(); ++I) {
      auto &Left = LHS.DimList[I];
      auto &Right = RHS.DimList[I];
      uint64_t L1 = Left.Start, K1 = Left.Step;
      uint64_t L2 = Right.Start, K2 = Right.Step;
      if (K1 * Left.MaxIter + L1 <= L2 || L1 >= K2 * Right.MaxIter + L2)
        return false;
    }
    return true;
  }*/
  static inline bool areJoinable(
      const MemoryLocationRange &LHS, const MemoryLocationRange &RHS) {
    if (LHS.DimList.size() != RHS.DimList.size())
      return false;
    if (LHS.DimList.size() == 0) {
      return sizecmp(getUpperBound(LHS), getLowerBound(RHS)) >= 0 &&
        sizecmp(getLowerBound(LHS), getUpperBound(RHS)) <= 0;
    }
    for (size_t I = 0; I < LHS.DimList.size(); ++I) {
      auto &Left = LHS.DimList[I];
      auto &Right = RHS.DimList[I];
      auto LeftEnd = Left.Start + Left.Step * Left.MaxIter;
      auto RightEnd = Right.Start + Right.Step * Right.MaxIter;
      if (Left.Step != Right.Step ||
          (Left.Start - Right.Start) % Left.Step != 0 ||
          !(LeftEnd < Right.Start && (Right.Start - LeftEnd) == Left.Step) ||
          !(RightEnd < Left.Start && (Left.Start - RightEnd) == Left.Step))
        return false;
    }
    return true;
  }
  static inline bool join(MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS) {
    if (LHS.DimList.size() != LHS.DimList.size())
      return false;
    if (LHS.DimList.size() == 0) {
      if (sizecmp(getUpperBound(LHS), getUpperBound(RHS)) < 0) {
        setUpperBound(getUpperBound(RHS), LHS);
        return true;
      }
      if (sizecmp(getLowerBound(LHS), getLowerBound(RHS)) > 0) {
        setLowerBound(getLowerBound(RHS), LHS);
        return true;
      }
      return false;
    }
    bool isChanged = false;
    for (size_t I = 0; I < LHS.DimList.size(); I++) {
      auto &To = LHS.DimList[I];
      auto &From = RHS.DimList[I];
      if (From.Start < To.Start) {
        To.MaxIter += (To.Start - From.Start) / From.Step;
        To.Start = From.Start;
        isChanged = true;
      }
      auto ToEnd = To.Start + To.Step * To.MaxIter;
      auto FromEnd = From.Start + From.Step * From.MaxIter;
      if (FromEnd > ToEnd) {
        To.MaxIter += (FromEnd - ToEnd) / From.Step;
        isChanged = true;
      }
    }
    return isChanged;
  }
};
}
#endif//TSAR_MEMORY_SET_INFO_H
