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
#include "tsar/Support/SCEVUtils.h"
#include <llvm/Analysis/ScalarEvolutionExpressions.h>

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
/// - static inline uint64_t getNumDims(const LocationTy &)
///     Return a number of dimensions of a specified location.
/// - static inline bool areJoinable(const LocationTy &, const LocationTy &)
///     Return `true` if one location can be joined to another, `false`
///     otherwise.
/// - static inline bool join(const LocationTy &What, LocationTy &To)
///     Join `What` location to `To` if they are joinable.
/// - static inline bool hasIntersection(const LocationTy &, const LocationTy &)
///     Return `true` if locations have an intersection, `false` otherwise.
/// - static inline llvm::Optional<LocationTy> intersect(
///       const LocationTy &A, const LocationTy &B,
///       llvm::SmallVectorImpl<LocationTy> *L,
///       llvm::SmallVectorImpl<LocationTy> *R)
///     Return the result of intersection of locations A and B.
/// - static inline void setNonCollapsable(llvm::MemoryLocation &)
///     Set `NonCollapsable` kind for a specified location.
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
    if (LHS == RHS)
      return 0;
    if (!LHS.hasValue() && !RHS.hasValue()) {
      if (LHS.mayBeBeforePointer())
        return 1;
      return -1;
    }
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
  static inline bool areJoinable(const llvm::MemoryLocation &LHS,
                                 const llvm::MemoryLocation &RHS) {
    return sizecmp(getUpperBound(LHS), getLowerBound(RHS)) >= 0 &&
        sizecmp(getLowerBound(LHS), getUpperBound(RHS)) <= 0;
  }
  static inline bool join(const llvm::MemoryLocation &What,
                          llvm::MemoryLocation &To) {
    bool IsChanged = false;
    if (sizecmp(getUpperBound(To), getUpperBound(What)) < 0) {
      setUpperBound(getUpperBound(What), To);
      IsChanged = true;
    }
    if (sizecmp(getLowerBound(To), getLowerBound(What)) > 0) {
      setLowerBound(getLowerBound(What), To);
      IsChanged = true;
    }
    return IsChanged;
  }
  static inline bool hasIntersection(const llvm::MemoryLocation &LHS,
      const llvm::MemoryLocation &RHS) {
    return sizecmp(getUpperBound(LHS), getLowerBound(RHS)) > 0 &&
           sizecmp(getLowerBound(LHS), getUpperBound(RHS)) < 0;
  }
  static inline llvm::Optional<llvm::MemoryLocation> intersect(
      const llvm::MemoryLocation &LHS,
      const llvm::MemoryLocation &RHS,
      llvm::SmallVectorImpl<llvm::MemoryLocation> *L,
      llvm::SmallVectorImpl<llvm::MemoryLocation> *R) {
    return llvm::None;
  }
  static inline void setNonCollapsable(llvm::MemoryLocation &Loc) {
    return;
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
    return Loc.LowerBound;
  }
  static inline void setLowerBound(
      LocationSize Size, MemoryLocationRange &Loc) noexcept {

    Loc.LowerBound = Size;
  }
  static inline LocationSize getUpperBound(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.UpperBound;
  }
  static inline void setUpperBound(
      LocationSize Size, MemoryLocationRange &Loc) noexcept {
    Loc.UpperBound = Size;
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
  static inline bool areJoinable(
      const MemoryLocationRange &LHS, const MemoryLocationRange &RHS) {
    assert(LHS.Ptr == RHS.Ptr && "Pointers of locations must be equal!");
    if (LHS.DimList.size() != RHS.DimList.size() ||
        LHS.Kind != RHS.Kind)
      return false;
    if (LHS.DimList.empty()) {
      return sizecmp(getUpperBound(LHS), getLowerBound(RHS)) >= 0 &&
        sizecmp(getLowerBound(LHS), getUpperBound(RHS)) <= 0;
    }
    std::size_t JoinableDimCount = 0;
    auto SE = LHS.SE;
    assert(SE && "ScalarEvolution must be specified!");
    for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
      auto &Left = LHS.DimList[I];
      auto &Right = RHS.DimList[I];
      if (Left == Right)
        continue;
      assert(llvm::isa<llvm::SCEVConstant>(Left.Step) &&
             llvm::isa<llvm::SCEVConstant>(Right.Step) &&
             "Dimension step must be constant!");
      if (!llvm::isa<llvm::SCEVConstant>(Left.Start) ||
          !llvm::isa<llvm::SCEVConstant>(Left.End) ||
          !llvm::isa<llvm::SCEVConstant>(Right.Start) ||
          !llvm::isa<llvm::SCEVConstant>(Right.End)) {
        auto CmpLSRE = compareSCEVs(Left.Start, Right.End, SE);
        auto CmpRSLE = compareSCEVs(Right.Start, Left.End, SE);
        if (Left.Step == Right.Step &&
            llvm::isa<llvm::SCEVConstant>(Left.Step) &&
            llvm::cast<llvm::SCEVConstant>(Left.Step)->
                getAPInt().getSExtValue() == 1 &&
            CmpLSRE && (*CmpLSRE == 0 || *CmpLSRE == 1) ||
            CmpRSLE && (*CmpRSLE == 0 || *CmpRSLE == 1)) {
          ++JoinableDimCount;
          continue;
        } 
        return false;
      }
      auto LeftStart = llvm::cast<llvm::SCEVConstant>(Left.Start)->
                       getValue()->getZExtValue();
      auto LeftEnd = llvm::cast<llvm::SCEVConstant>(Left.End)->
                     getValue()->getZExtValue();
      auto LeftStep = llvm::cast<llvm::SCEVConstant>(Left.Step)->
                      getValue()->getZExtValue();
      auto RightStart = llvm::cast<llvm::SCEVConstant>(Right.Start)->
                        getValue()->getZExtValue();
      auto RightEnd = llvm::cast<llvm::SCEVConstant>(Right.End)->
                        getValue()->getZExtValue();
      auto RightStep = llvm::cast<llvm::SCEVConstant>(Right.Step)->
                       getValue()->getZExtValue();
      assert(LeftStart <= LeftEnd && RightStart <= RightEnd &&
          "Start of dimension must be less or equal than End.");
      if (LeftStep != RightStep ||
          (LeftStart >= RightStart &&
              (LeftStart - RightStart) % LeftStep != 0) ||
          (RightStart >= LeftStart &&
              (RightStart - LeftStart) % LeftStep != 0) ||
          (LeftEnd < RightStart && (RightStart - LeftEnd) != LeftStep) ||
          (RightEnd < LeftStart && (LeftStart - RightEnd) != LeftStep))
        return false;
      ++JoinableDimCount;
    }
    return JoinableDimCount <= 1;
  }
  static inline bool join(const MemoryLocationRange &What,
      MemoryLocationRange &To) {
    assert(What.Ptr == To.Ptr && "Pointers of locations must be equal!");
    assert(areJoinable(What, To) && "Locations must be joinable!");
    if (To.DimList.size() != What.DimList.size())
      return false;
    bool IsChanged = false;
    if (To.DimList.empty()) {
      if (sizecmp(getUpperBound(To), getUpperBound(What)) < 0) {
        setUpperBound(getUpperBound(What), To);
        IsChanged = true;
      }
      if (sizecmp(getLowerBound(To), getLowerBound(What)) > 0) {
        setLowerBound(getLowerBound(What), To);
        IsChanged = true;
      }
    } else {
      auto SE = What.SE;
      assert(SE && "ScalarEvolution must be specified!");
      for (size_t I = 0; I < To.DimList.size(); I++) {
        auto &DimTo = To.DimList[I];
        auto &DimFrom = What.DimList[I];
        if (!llvm::isa<llvm::SCEVConstant>(DimTo.Start) ||
            !llvm::isa<llvm::SCEVConstant>(DimTo.End) ||
            !llvm::isa<llvm::SCEVConstant>(DimFrom.Start) ||
            !llvm::isa<llvm::SCEVConstant>(DimFrom.End)) {
          auto CmpTSFE = compareSCEVs(DimTo.Start, DimFrom.End, SE);
          auto CmpFSTE = compareSCEVs(DimFrom.Start, DimTo.End, SE);
          if (CmpTSFE && (*CmpTSFE == 0 || *CmpTSFE == 1)) {
            DimTo.Start = DimFrom.Start;
            IsChanged = true;
          }
          if (CmpFSTE && (*CmpFSTE == 0 || *CmpFSTE == 1)) {
            DimTo.End = DimFrom.End;
            IsChanged = true;
          }
          continue;
        }
        auto DiffStart = compareSCEVs(DimTo.Start, DimFrom.Start, SE);
        assert(DiffStart && "Difference must be constant!");
        if (*DiffStart > 0) {
          DimTo.Start = DimFrom.Start;
          IsChanged = true;
        }
        auto DiffEnd = compareSCEVs(DimFrom.End, DimTo.End, SE);
        assert(DiffEnd && "Difference must be constant!");
        if (*DiffEnd > 0) {
          DimTo.End = DimFrom.End;
          IsChanged = true;
        }
      }
    }
    return IsChanged;
  }
  static inline bool hasIntersection(const MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS) {
    return tsar::intersect(LHS, RHS).hasValue();
  }
  static inline llvm::Optional<MemoryLocationRange> intersect(
      const MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS,
      llvm::SmallVectorImpl<MemoryLocationRange> *L = nullptr,
      llvm::SmallVectorImpl<MemoryLocationRange> *R = nullptr) {
    return tsar::intersect(LHS, RHS, L, R);
  }
  static inline void setNonCollapsable(MemoryLocationRange &Loc) {
    Loc.Kind = MemoryLocationRange::LocKind::NonCollapsable |
               Loc.Kind & MemoryLocationRange::LocKind::Hint;
  }
};
}
#endif//TSAR_MEMORY_SET_INFO_H
