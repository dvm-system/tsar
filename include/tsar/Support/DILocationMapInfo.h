//===-- DILocationMapInfo.h - Type Traits for llvm::DenseMap ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
//
//===----------------------------------------------------------------------===//
//
// This file provide implementation of llvm::DenseMapInfo to compare different
// representations of a presumed location with a metadata location
// llvm::DILocation.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_LOCATION_MAP_INFO_H
#define TSAR_DI_LOCATION_MAP_INFO_H

#include "tsar/Support/MetadataUtils.h"
#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Path.h>
#include <type_traits>

namespace tsar {
/// This class should be specialized for loction-like classes which are intened
/// to use as a key in llvm::DenseMap based on tsar::DILocationMapInfo traits.
///
/// The following methods must be provided:
/// - static unsigned getLine(const LocationT &Loc);
/// - static unsigned getColumn(const LocationT &Loc);
/// - static llvm::StringRef getFilename(const LocationT &Loc);
template <typename LocationT> struct PresumedLocationInfo {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  using LocationType = typename LocationT::UnknownLocationError;
};

template <> struct PresumedLocationInfo<const llvm::DILocation *> {
  static unsigned getLine(const llvm::DILocation *Loc) {
    return Loc->getLine();
  }
  static unsigned getColumn(const llvm::DILocation *Loc) {
    return Loc->getColumn();
  }
  static llvm::SmallString<128> getFilename(const llvm::DILocation *Loc) {
    llvm::SmallString<128> Path;
    tsar::getAbsolutePath(*Loc->getScope(), Path);
    return Path;
  }
};

template <>
struct PresumedLocationInfo<llvm::DILocation *>
    : public PresumedLocationInfo<const llvm::DILocation *> {};

/// Implementation of a DenseMapInfo for DILocation *.
///
/// To generate hash value pair of line and column is used. It is possible to
/// use find_as() method with a parameter of type with specified
/// tsar::PresumedLocationInfo template.
struct DILocationMapInfo {
  static inline llvm::DILocation *getEmptyKey() {
    return llvm::DenseMapInfo<llvm::DILocation *>::getEmptyKey();
  }

  static inline llvm::DILocation *getTombstoneKey() {
    return llvm::DenseMapInfo<llvm::DILocation *>::getTombstoneKey();
  }

  template <typename LocationT>
  static unsigned getHashValue(const LocationT &Loc) {
    auto Line{PresumedLocationInfo<LocationT>::getLine(Loc)};
    auto Column{PresumedLocationInfo<LocationT>::getColumn(Loc)};
    std::pair Pair{Line, Column};
    return llvm::DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }

  template <typename LHSLocationT, typename RHSLocationT>
  static bool isEqual(const LHSLocationT &LHS, const RHSLocationT &RHS) {
    if constexpr (std::is_same_v<std::decay_t<LHSLocationT>,
                                 llvm::DILocation *> &&
                  std::is_same_v<std::decay_t<RHSLocationT>,
                                 llvm::DILocation *>)
      if (LHS == RHS)
        return true;
    if constexpr (std::is_same_v<std::decay_t<LHSLocationT>,
                                 llvm::DILocation *>)
      if (LHS == getTombstoneKey() || LHS == getEmptyKey())
        return false;
    if constexpr (std::is_same_v<std::decay_t<RHSLocationT>,
                                 llvm::DILocation *>)
      if (RHS == getTombstoneKey() || RHS == getEmptyKey())
        return false;
    llvm::sys::fs::UniqueID LHSId, RHSId;
    return PresumedLocationInfo<LHSLocationT>::getLine(LHS) ==
               PresumedLocationInfo<RHSLocationT>::getLine(RHS) &&
           PresumedLocationInfo<LHSLocationT>::getColumn(LHS) ==
               PresumedLocationInfo<RHSLocationT>::getColumn(RHS) &&
           !llvm::sys::fs::getUniqueID(
               PresumedLocationInfo<LHSLocationT>::getFilename(LHS), LHSId) &&
           !llvm::sys::fs::getUniqueID(
               PresumedLocationInfo<RHSLocationT>::getFilename(RHS), RHSId) &&
           LHSId == RHSId;
  }
};
} // namespace tsar
#endif // TSAR_DI_LOCATION_MAP_INFO_H
