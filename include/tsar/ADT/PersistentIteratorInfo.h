//=== PersistentIteratorInfo.h Type traits for PersistentIterator*- C++ -*-===//
//
//                       Traits Static Analyzer (TSAR)
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
// This file defines llvm::DenseMapInfo traits for persistent iterators.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_PERSISTENT_ITERATOR_INFO_H
#define TSAR_PERSISTENT_ITERATOR_INFO_H

#include "PersistentIterator.h"
#include <llvm/ADT/DenseMapInfo.h>

namespace llvm {
template<class PersistentT, class MapT> struct DenseMapInfo<
    tsar::PersistentIteratorC<PersistentT, MapT>> {
protected:
  using Iterator = tsar::PersistentIteratorC<PersistentT, MapT>;
  using PersistentBucket = typename Iterator::PersistentBucket;
public:
  static inline Iterator getEmptyKey() {
    return Iterator(DenseMapInfo<PersistentBucket *>::getEmptyKey());
  }
  static inline Iterator getTombstoneKey() {
    return Iterator(DenseMapInfo<PersistentBucket *>::getTombstoneKey());
  }
  static unsigned getHashValue(const Iterator &Val) {
    return DenseMapInfo<PersistentBucket *>::getHashValue(Val.mPtr);
  }
  static bool isEqual(const Iterator &LHS, const Iterator &RHS) {
    return LHS == RHS;
  }
};

template<class PersistentT, class MapT> struct DenseMapInfo<
  tsar::PersistentIterator<PersistentT, MapT>> :
    public DenseMapInfo<tsar::PersistentIteratorC<PersistentT, MapT>> {
protected:
  using Iterator = tsar::PersistentIterator<PersistentT, MapT>;
  using PersistentBucket =
    typename DenseMapInfo<tsar::PersistentIteratorC<PersistentT, MapT>>
      ::PersistentBucket;
public:
  static inline Iterator getEmptyKey() {
    return Iterator(DenseMapInfo<PersistentBucket *>::getEmptyKey());
  }
  static inline Iterator getTombstoneKey() {
    return Iterator(DenseMapInfo<PersistentBucket *>::getTombstoneKey());
  }
};
}
#endif//TSAR_PERSISTENT_ITERATOR_INFO_H
