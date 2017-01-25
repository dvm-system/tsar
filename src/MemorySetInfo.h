//===--- MemorySetInfo.h - Type traits for MemorySet ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines MemorySetInfo traits for MemorySet.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_SET_INFO_H
#define TSAR_MEMORY_SET_INFO_H

#include <llvm/Analysis/MemoryLocation.h>

namespace tsar {
/// \brief Provides traits for objects stored in a MemorySet.
///
/// The following methods should be provided:
/// - static const llvm::Value * getPtr(const Ty &) -
///     Returns pointer to the beginning of a memory location.
/// - static uint64_t getSize(const Ty &) -
///     Returns size of a memory location.
/// - static llvm::AAMDNodes & getAATags(const Ty &) -
///     Returns the metadata nodes which describes the aliasing of the location.
/// - static void setSize(LocationTy &, uint64_t)
///     Set size for a specified location.
/// - static void setAATags(LocationTy &, const llvm::AAMDNodes &)
///     Set the metadata nodes for a specified location.
/// - static LocationTy * make(Ty &)
///     Constructs new memory location from the other one and returns a pointer
///     to the new location. It will be deleted in automatic way when memory
///     set will be destroyed.
/// In methods presented above the following denotements are used:
/// - LocationTy is a type of memory locations which are stored in memory set.
/// - Ty is a type of memory locations which is used for insertion the new one
///   and for search the existent locations. It should be also possible to use
///   LocationTy as a Ty.
template<class Ty> struct MemorySetInfo {
  typedef typename Ty::UnknownMemoryLocationError LocationTy;
};

/// Provides specialization of MemorySetInfo for llvm::MemoryLocation
template<> struct MemorySetInfo<llvm::MemoryLocation> {
  static inline const llvm::Value * getPtr(
    const llvm::MemoryLocation &Loc) noexcept {
    return Loc.Ptr;
  }
  static inline uint64_t getSize(
      const llvm::MemoryLocation &Loc) noexcept {
    return Loc.Size;
  }
  static inline void setSize(
      llvm::MemoryLocation &Loc, uint64_t Size) noexcept {
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
  static inline llvm::MemoryLocation * make(const llvm::MemoryLocation &Loc) {
    return new llvm::MemoryLocation(Loc);
  }
};
}
#endif//TSAR_MEMORY_SET_INFO_H
