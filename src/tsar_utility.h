//===----- tsar_utility.h - Utility Methods and Classes ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_UTILITY_H
#define TSAR_UTILITY_H

#include <llvm/ADT/SmallPtrSet.h>

namespace tsar {
/// Compares two set.
template<class PtrType, unsigned SmallSize>
bool operator==(const llvm::SmallPtrSet<PtrType, SmallSize> &LHS,
  const llvm::SmallPtrSet<PtrType, SmallSize> &RHS) {
  if (LHS.size() != RHS.size())
    return false;
  for (PtrType V : LHS)
    if (RHS.count(V) == 0)
      return false;
  return true;
}

/// Compares two set.
template<class PtrType, unsigned SmallSize>
bool operator!=(const llvm::SmallPtrSet<PtrType, SmallSize> &LHS,
  const llvm::SmallPtrSet<PtrType, SmallSize> &RHS) {
  return !(LHS == RHS);
}

}
#endif//TSAR_UTILITY_H
