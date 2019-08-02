//=== BitMemoryTrait.cpp  Bitwise Representation of Memory Traits *- C++ -*===//
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
// This file implements bitwise representation of memory traits.
//
//===----------------------------------------------------------------------===//

#include "BitMemoryTrait.h"

using namespace tsar;

BitMemoryTrait::BitMemoryTrait(const MemoryDescriptor &Dptr) : mId(NoAccess) {
  if (Dptr.is<trait::AddressAccess>())
    mId &= AddressAccess;
  if (Dptr.is<trait::HeaderAccess>())
    mId &= HeaderAccess;
  if (Dptr.is<trait::ExplicitAccess>())
    mId &= ExplicitAccess;
  if (Dptr.is<trait::Redundant>())
    mId &= Redundant;
  if (Dptr.is<trait::NoRedundant>())
    mId &= NoRedundant;
  if (Dptr.is<trait::Flow>() ||
      Dptr.is<trait::Anti>() ||
      Dptr.is<trait::Output>()) {
    mId &= Dependency;
  } else if (Dptr.is<trait::Reduction>()) {
    mId &= Reduction;
  } else if (Dptr.is<trait::Induction>()) {
    mId &= Induction;
  } else if (Dptr.is<trait::Readonly>()) {
    mId &= Readonly;
  } else {
    if (Dptr.is<trait::Shared>())
      mId &= Shared;
    if (Dptr.is<trait::FirstPrivate>())
      mId &= FirstPrivate;
    if (Dptr.is<trait::Private>())
      mId &= Private;
    else if (Dptr.is<trait::LastPrivate>())
      mId &= LastPrivate;
    else if (Dptr.is<trait::SecondToLastPrivate>())
      mId &= SecondToLastPrivate;
    else if (Dptr.is<trait::DynamicPrivate>())
      mId &= DynamicPrivate;
  }
}

MemoryDescriptor BitMemoryTrait::toDescriptor(unsigned TraitNumber,
    MemoryStatistic &Stat) const {
  MemoryDescriptor Dptr;
  if (!(get() & ~AddressAccess)) {
    Dptr.set<trait::AddressAccess>();
    Stat.get<trait::AddressAccess>() += TraitNumber;
  }
  if (!(get() & ~HeaderAccess)) {
    Dptr.set<trait::HeaderAccess>();
    Stat.get<trait::HeaderAccess>() += TraitNumber;
  }
  if (!(get() & ~ExplicitAccess)) {
    Dptr.set<trait::ExplicitAccess>();
    Stat.get<trait::ExplicitAccess>() += TraitNumber;
  }
  if (!(get() & ~Redundant)) {
    Dptr.set<trait::Redundant>();
    Stat.get<trait::Redundant>() += TraitNumber;
  }
  if (!(get() & ~NoRedundant)) {
    Dptr.set<trait::NoRedundant>();
    Stat.get<trait::NoRedundant>() += TraitNumber;
  }
  if (dropUnitFlag(get()) == Dependency) {
    Dptr.set<trait::Flow, trait::Anti, trait::Output>();
    Stat.get<trait::Flow>() += TraitNumber;
    Stat.get<trait::Anti>() += TraitNumber;
    Stat.get<trait::Output>() += TraitNumber;
    return Dptr;
  }
  switch (dropUnitFlag(dropSharedFlag(get()))) {
  default:
    llvm_unreachable("Unknown type of memory location dependency!");
    break;
  case NoAccess: Dptr.set<trait::NoAccess>(); break;
  case Readonly: Dptr.set<trait::Readonly>();
    Stat.get<trait::Readonly>() += TraitNumber; break;
  case Private: Dptr.set<trait::Private>();
    Stat.get<trait::Private>() += TraitNumber; break;
  case FirstPrivate: Dptr.set<trait::FirstPrivate>();
    Stat.get<trait::FirstPrivate>() += TraitNumber; break;
  case FirstPrivate & LastPrivate:
    Dptr.set<trait::FirstPrivate>();
    Stat.get<trait::FirstPrivate>() += TraitNumber;
  case LastPrivate:
    Dptr.set<trait::LastPrivate>();
    Stat.get<trait::LastPrivate>() += TraitNumber;
    break;
  case FirstPrivate & SecondToLastPrivate:
    Dptr.set<trait::FirstPrivate>();
    Stat.get<trait::FirstPrivate>() += TraitNumber;
  case SecondToLastPrivate:
    Dptr.set<trait::SecondToLastPrivate>();
    Stat.get<trait::SecondToLastPrivate>() +=TraitNumber;
    break;
  case FirstPrivate & DynamicPrivate:
    Dptr.set<trait::FirstPrivate>();
    Stat.get<trait::FirstPrivate>() += TraitNumber;
  case DynamicPrivate:
    Dptr.set<trait::DynamicPrivate>();
    Stat.get<trait::DynamicPrivate>() += TraitNumber;
    break;
  case Reduction:
    Dptr.set<trait::Reduction>();
    Stat.get<trait::Reduction>() += TraitNumber;
    break;
  case Induction:
    Dptr.set<trait::Induction>();
    Stat.get<trait::Induction>() += TraitNumber;
    break;
  }
  // If shared is one of traits it has been set as read-only in `switch`.
  // Hence, do not move this condition before `switch` because it should
  // override read-only if necessary.
  if (!(get() &  ~(~Readonly | Shared))) {
    Dptr.set<trait::Shared>();
    Stat.get<trait::Shared>() += TraitNumber;
    Stat.get<trait::Readonly>() -= TraitNumber;
  }
  return Dptr;
}
