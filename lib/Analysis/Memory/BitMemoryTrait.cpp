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
#include <llvm/Support/ErrorHandling.h>

using namespace tsar;

BitMemoryTrait::BitMemoryTrait(const MemoryDescriptor &Dptr) : mId(NoAccess) {
  if (Dptr.is<trait::AddressAccess>())
    mId &= AddressAccess;
  if (Dptr.is<trait::UseAfterLoop>())
    mId &= UseAfterLoop;
  if (Dptr.is<trait::HeaderAccess>())
    mId &= HeaderAccess;
  if (Dptr.is<trait::NoPromotedScalar>())
    mId &= NoPromotedScalar;
  if (Dptr.is<trait::ExplicitAccess>())
    mId &= ExplicitAccess;
  if (Dptr.is<trait::Redundant>())
    mId &= Redundant;
  if (Dptr.is<trait::NoRedundant>())
    mId &= NoRedundant;
  if (Dptr.is<trait::DirectAccess>())
    mId &= DirectAccess;
  if (Dptr.is<trait::IndirectAccess>())
    mId &= IndirectAccess;
  if (Dptr.is<trait::Lock>())
    mId &= Lock;
  if (Dptr.is<trait::NoAccess>())
    return;
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
    auto SharedFlag = Dptr.is<trait::Shared>() ? SharedJoin : ~NoAccess;
    auto SharedTrait = Dptr.is<trait::Shared>() ? Shared : Dependency;
    if (Dptr.is<trait::Private>()) {
      mId &= Private | SharedFlag;
    }  else {
      if (Dptr.is<trait::FirstPrivate>())
        mId &= FirstPrivate | SharedFlag;
      if (Dptr.is<trait::LastPrivate>())
        mId &= LastPrivate | SharedFlag;
      else if (Dptr.is<trait::SecondToLastPrivate>())
        mId &= SecondToLastPrivate | SharedFlag;
      else if (Dptr.is<trait::DynamicPrivate>())
        mId &= DynamicPrivate | SharedFlag;
      else if (!Dptr.is<trait::FirstPrivate>()) {
        mId &= SharedTrait;
      }
    }
  }
}

MemoryDescriptor BitMemoryTrait::toDescriptor(unsigned TraitNumber,
  MemoryStatistic &Stat) const {
  MemoryDescriptor Dptr;
  if (!(get() & ~UseAfterLoop)) {
    Dptr.set<trait::UseAfterLoop>();
    Stat.get<trait::UseAfterLoop>() += TraitNumber;
  }
  if (!(get() & ~AddressAccess)) {
    Dptr.set<trait::AddressAccess>();
    Stat.get<trait::AddressAccess>() += TraitNumber;
  }
  if (!(get() & ~HeaderAccess)) {
    Dptr.set<trait::HeaderAccess>();
    Stat.get<trait::HeaderAccess>() += TraitNumber;
  }
  if (!(get() & ~Lock)) {
    Dptr.set<trait::Lock>();
    Stat.get<trait::Lock>() += TraitNumber;
  }
  if (!(get() & ~NoPromotedScalar)) {
    Dptr.set<trait::NoPromotedScalar>();
    Stat.get<trait::NoPromotedScalar>() += TraitNumber;
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
  if (!(get() & ~DirectAccess)) {
    Dptr.set<trait::DirectAccess>();
    Stat.get<trait::DirectAccess>() += TraitNumber;
  }
  if (!(get() & ~IndirectAccess)) {
    Dptr.set<trait::IndirectAccess>();
    Stat.get<trait::IndirectAccess>() += TraitNumber;
  }
  switch (dropUnitFlag(get())) {
  case Readonly:
    Dptr.set<trait::Readonly>();
    Stat.get<trait::Readonly>() += TraitNumber;
    return Dptr;
  case NoAccess:
    Dptr.set<trait::NoAccess>();
    return Dptr;
  case Shared:
    Dptr.set<trait::Shared>();
    Stat.get<trait::Shared>() += TraitNumber;
    return Dptr;
  }
  if (hasSharedJoin(get())) {
    Dptr.set<trait::Shared>();
    Stat.get<trait::Shared>() += TraitNumber;
  }
  switch (dropUnitFlag(dropSharedFlag(get()))) {
  default:
    llvm_unreachable("Unknown type of memory location dependency!");
    break;
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
  case Dependency:
    Dptr.set<trait::Flow, trait::Anti, trait::Output>();
    Stat.get<trait::Flow>() += TraitNumber;
    Stat.get<trait::Anti>() += TraitNumber;
    Stat.get<trait::Output>() += TraitNumber;
    return Dptr;
  case Reduction:
    Dptr.set<trait::Reduction>();
    Stat.get<trait::Reduction>() += TraitNumber;
    return Dptr;
  case Induction:
    Dptr.set<trait::Induction>();
    Stat.get<trait::Induction>() += TraitNumber;
    return Dptr;
  }
  return Dptr;
}
