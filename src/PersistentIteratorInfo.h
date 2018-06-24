//=== PersistentIteratorInfo.h Type traits for PersistentIterator*- C++ -*-===//
//
//                       Traits Static Analyzer (TSAR)
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
