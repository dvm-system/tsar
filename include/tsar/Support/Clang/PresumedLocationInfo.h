//===-- PresumedLocationInfo.h - Type Traits for llvm::DenseMap -*- C++ -*-===//
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

#ifndef TSAR_CLANG_PRESUMED_LOCATION_INFO_H
#define TSAR_CLANG_PRESUMED_LOCATION_INFO_H

#include "tsar/Support/DILocationMapInfo.h"
#include <clang/Basic/SourceLocation.h>

namespace tsar {
template <> struct PresumedLocationInfo<clang::PresumedLoc> {
  static unsigned getLine(const clang::PresumedLoc &Loc) {
    return Loc.getLine();
  }
  static unsigned getColumn(const clang::PresumedLoc &Loc) {
    return Loc.getColumn();
  }
  static llvm::StringRef getFilename(const clang::PresumedLoc &Loc) {
    return Loc.getFilename();
  }
};
} // namespace tsar

#endif//TSAR_CLANG_PRESUMED_LOCATION_INFO_H
