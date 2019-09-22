//===- NamedDeclMapInfo.h - Name Base Map Info For NamedDecl's --*- C++ -*-===//
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
// This file provides implementation of llvm::DenseMapInfo for clang::NamedDecl.
// Name of declaration is used to construct keys, so it is possible to use
// find_as("...") methods to find declaration in a container.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_NAMED_DECL_MAP_INFO_H
#define TSAR_NAMED_DECL_MAP_INFO_H

#include <clang/AST/Decl.h>
#include <llvm/ADT/DenseMapInfo.h>

namespace tsar {
struct NamedDeclMapInfo {
  static inline clang::NamedDecl * getEmptyKey() {
    return llvm::DenseMapInfo<clang::NamedDecl *>::getEmptyKey();
  }
  static inline clang::NamedDecl * getTombstoneKey() {
    return llvm::DenseMapInfo<clang::NamedDecl *>::getTombstoneKey();
  }
  static inline unsigned getHashValue(const clang::NamedDecl *ND) {
    return llvm::DenseMapInfo<llvm::StringRef>::getHashValue(ND->getName());
  }
  static inline unsigned getHashValue(llvm::StringRef Name) {
    return llvm::DenseMapInfo<llvm::StringRef>::getHashValue(Name);
  }
  static inline bool isEqual(const clang::NamedDecl *LHS,
      const clang::NamedDecl *RHS) noexcept {
    return LHS == RHS;
  }
  static inline bool isEqual(llvm::StringRef LHS,
      const clang::NamedDecl *RHS) {
    return RHS != getEmptyKey() && RHS != getTombstoneKey() &&
      RHS->getName() == LHS;
  }
};
}
#endif//TSAR_NAMED_DECL_MAP_INFO_H
