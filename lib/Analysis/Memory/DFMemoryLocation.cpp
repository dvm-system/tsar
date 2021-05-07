//===--- DFMemoryLocation.cpp - Data Flow Framework ----- -------*- C++ -*-===//
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
// This file implements methods declared in DFMemoryLocation.h
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DFMemoryLocation.h"
#include "tsar/Unparse/Utils.h"
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

void LocationDFValue::print(raw_ostream &OS, const DominatorTree *DT) const {
  if (mKind == KIND_FULL) {
    OS << "whole program memory\n";
    return;
  }
  for (auto &Loc: mLocations) {
    printLocationSource(OS, Loc, DT);
    OS << " " << *Loc.Ptr << "\n";
  }
}

LLVM_DUMP_METHOD void LocationDFValue::dump(const DominatorTree *DT) const {
  print(dbgs(), DT);
}
}
