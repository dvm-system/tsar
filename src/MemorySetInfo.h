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
/// - static uint64_t getLowerBound(const Ty &) -
///     Return the lower bound (offset) of a memory location.
/// - static uint64_t getUpperBound(const Ty &) -
///     Return the upper bound (offset) of a memory location.
/// - static llvm::AAMDNodes & getAATags(const Ty &) -
///     Return the metadata nodes which describes the aliasing of the location.
/// - static void setLowerBound(LocationTy &, uint64_t)
///     Set the lower bound for a specified location.
/// - static void setUpperBound(LocationTy &, uint64_t)
///     Set the upper bound for a specified location.
/// - static void setAATags(LocationTy &, const llvm::AAMDNodes &)
///     Set the metadata nodes for a specified location.
/// - static LocationTy make(Ty &)
///     Construct new memory location from the other one.
/// In methods presented above the following denotements are used:
/// - LocationTy is a type of memory locations which are stored in memory set.
/// - Ty is a type of memory locations which is used for insertion the new one
///   and for search the existent locations. It should be also possible to use
///   LocationTy as a Ty.
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
      llvm::MemoryLocation &Loc, llvm::LocationSize Size) noexcept {
  }
  static inline llvm::LocationSize getUpperBound(
      const llvm::MemoryLocation &Loc) noexcept {
    return Loc.Size;
  }
  static inline void setUpperBound(
      llvm::MemoryLocation &Loc, llvm::LocationSize Size) noexcept {
    Loc.Size = Size;
  }
    static inline const llvm::AAMDNodes & getAATags(
      const llvm::MemoryLocation &Loc) noexcept {
    return Loc.AATags;
  }
  static inline void setAATags(
    llvm::MemoryLocation &Loc, const llvm::AAMDNodes &AATags) noexcept {
    Loc.AATags = AATags;
  }
  static inline llvm::MemoryLocation make(const llvm::MemoryLocation &Loc) {
    return llvm::MemoryLocation(Loc);
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
      MemoryLocationRange &Loc, LocationSize Size) noexcept {
    Loc.LowerBound = Size;
  }
  static inline LocationSize getUpperBound(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.UpperBound;
  }
  static inline void setUpperBound(
    MemoryLocationRange &Loc, LocationSize Size) noexcept {
    Loc.UpperBound = Size;
  }
    static inline const llvm::AAMDNodes & getAATags(
      const MemoryLocationRange &Loc) noexcept {
    return Loc.AATags;
  }
  static inline void setAATags(
    MemoryLocationRange &Loc, const llvm::AAMDNodes &AATags) noexcept {
    Loc.AATags = AATags;
  }
  static inline MemoryLocationRange make(const MemoryLocationRange &Loc) {
    return MemoryLocationRange(Loc);
  }
};
}
#endif//TSAR_MEMORY_SET_INFO_H
