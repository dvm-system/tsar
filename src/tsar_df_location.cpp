//===--- tsar_df_location.cpp - Data Flow Framework ----- -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods declared in tsar_df_location.h
//
//===----------------------------------------------------------------------===//

#include "tsar_df_location.h"
#include "tsar_dbg_output.h"
#include "tsar_utility.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Debug.h>

using namespace llvm;

namespace tsar {
bool LocationDFValue::intersect(const LocationDFValue &With) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(With.mKind != INVALID_KIND && "Collection is corrupted!");
  if (With.mKind == KIND_FULL)
    return false;
  if (mKind == KIND_FULL) {
    *this = With;
    return true;
  }
  return mLocations.intersect(With.mLocations);
}

bool LocationDFValue::merge(const LocationDFValue &With) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(With.mKind != INVALID_KIND && "Collection is corrupted!");
  if (mKind == KIND_FULL)
    return false;
  if (With.mKind == KIND_FULL) {
    mLocations.clear();
    mKind = KIND_FULL;
    return true;
  }
  return mLocations.merge(With.mLocations);
}

void LocationDFValue::print(raw_ostream &OS) const {
  if (mKind == KIND_FULL) {
    OS << "whole program memory\n";
    return;
  }
  for (auto &Loc: mLocations) {
    printLocationSource(OS, Loc.Ptr);
    OS << " " << *Loc.Ptr << "\n";
  }
}

void LocationDFValue::dump() const { print(dbgs()); }
}
