//===---- TraitFilter.h ------ Filters Of Traits ----------------*- C++ -*-===//
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
// This file defines some filters which check whether a trait is set.
// These filters can be used in `LockTraitPass` and as a parameters for
// `createLockTraitPass()` function to define which traits should be locked.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_TRAIT_FILTER_H
#define TSAR_MEMORY_TRAIT_FILTER_H

#include <tsar/Analysis/Memory/DIMemoryTrait.h>

namespace tsar {
/// Return true if all specified traits `Traits` is set for `T`.
template<class... Traits> bool is(const DIMemoryTrait &T) {
  if (T.is<Traits...>())
    return true;
  return false;
}

/// Return true if at least one of specified traits `Traits` is set for `T`.
template<class... Traits> bool isAny(const DIMemoryTrait &T) {
  if (T.is_any<Traits...>())
    return true;
  return false;
}
}
#endif//TSAR_MEMORY_TRAIT_FILTER_H
