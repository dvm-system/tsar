//===--- tsar_df_alloca.cpp - Data Flow Framework ----- ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methdos declared in tsar_df_alloca.h
//
//===----------------------------------------------------------------------===//

#include"tsar_df_alloca.h"
#include "tsar_utility.h"

using namespace llvm;
using namespace tsar;

bool AllocaDFValue::intersect(const AllocaDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (with.mKind == KIND_FULL)
    return false;
  if (mKind == KIND_FULL) {
    *this = with;
    return true;
  }
  AllocaSet PrevAllocas;
  mAllocas.swap(PrevAllocas);
  for (llvm::AllocaInst *AI : PrevAllocas) {
    if (with.mAllocas.count(AI))
      mAllocas.insert(AI);
  }
  return mAllocas.size() != PrevAllocas.size();
}

bool AllocaDFValue::merge(const AllocaDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (mKind == KIND_FULL)
    return false;
  if (with.mKind == KIND_FULL) {
    mAllocas.clear();
    mKind = KIND_FULL;
    return true;
  }
  bool isChanged = false;
  for (llvm::AllocaInst *AI : with.mAllocas)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    isChanged = mAllocas.insert(AI) || isChanged;
#else
    isChanged = mAllocas.insert(AI).second || isChanged;
#endif
  return isChanged;
}

bool AllocaDFValue::operator==(const AllocaDFValue &RHS) const {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(RHS.mKind != INVALID_KIND && "Collection is corrupted!");
  if (this == &RHS || mKind == KIND_FULL && RHS.mKind == KIND_FULL)
    return true;
  if (mKind != RHS.mKind)
    return false;
  return mAllocas == RHS.mAllocas;
}

