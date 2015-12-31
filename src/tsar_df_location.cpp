//===--- tsar_df_location.cpp - Data Flow Framework ----- ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methdos declared in tsar_df_location.h
//
//===----------------------------------------------------------------------===//

#include "tsar_df_location.h"
#include "tsar_utility.h"

using namespace llvm;
using namespace tsar;

bool LocationDFValue::intersect(const LocationDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (with.mKind == KIND_FULL)
    return false;
  if (mKind == KIND_FULL) {
    *this = with;
    return true;
  }
  LocationSet PrevAllocas;
  mLocations.swap(PrevAllocas);
  for (Value *Loc : PrevAllocas) {
    if (with.mLocations.count(Loc))
      mLocations.insert(Loc);
  }
  return mLocations.size() != PrevAllocas.size();
}

bool LocationDFValue::merge(const LocationDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (mKind == KIND_FULL)
    return false;
  if (with.mKind == KIND_FULL) {
    mLocations.clear();
    mKind = KIND_FULL;
    return true;
  }
  bool isChanged = false;
  for (Value *AI : with.mLocations)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    isChanged = mLocations.insert(AI) || isChanged;
#else
    isChanged = mLocations.insert(AI).second || isChanged;
#endif
  return isChanged;
}

bool LocationDFValue::operator==(const LocationDFValue &RHS) const {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(RHS.mKind != INVALID_KIND && "Collection is corrupted!");
  if (this == &RHS || mKind == KIND_FULL && RHS.mKind == KIND_FULL)
    return true;
  if (mKind != RHS.mKind)
    return false;
  return mLocations == RHS.mLocations;
}

