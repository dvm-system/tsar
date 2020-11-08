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
// This file defines some filters which marks memory location if some
// traits are set. These filters can be used in `ProcessDIMemoryTraitPass` and
// as a parameters for `createProcessDIMemoryTraitPass()` function to define
// which traits should be marked.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_TRAIT_FILTER_H
#define TSAR_MEMORY_TRAIT_FILTER_H

#include <tsar/Analysis/Memory/DIMemoryTrait.h>

namespace llvm {
class DataLayout;
}

namespace tsar {
/// Set `WhatT` trait.
template<class WhatT> void mark(DIMemoryTrait &T) {
  T.set<WhatT>();
}

/// Unset `WhatT` trait.
template<class WhatT> void unmark(DIMemoryTrait &T) {
  T.unset<WhatT>();
}

/// Set `WhatT` trait if all specified traits `Traits` is set for `T`.
template<class WhatT, class... Traits> void markIf(DIMemoryTrait &T) {
  if (T.is<Traits...>())
    T.set<WhatT>();
}

/// Unset `WhatT` trait if all specified traits `Traits` is set for `T`.
template<class WhatT, class... Traits> void unmarkIf(DIMemoryTrait &T) {
  if (T.is<Traits...>())
    T.unset<WhatT>();
}

/// Set `WhatT` trait if at least one of specified traits `Traits` is set for
/// `T`.
template<class WhatT, class... Traits> void markIfAny(DIMemoryTrait &T) {
  if (T.is_any<Traits...>())
    T.set<WhatT>();
}

/// Unset `WhatT` trait if at least one of specified traits `Traits` is set for
/// `T`.
template<class WhatT, class... Traits> void unmarkIfAny(DIMemoryTrait &T) {
  if (T.is_any<Traits...>())
    T.unset<WhatT>();
}

/// This filter marks locations which have not been promoted yet.
///
/// Actually this filter looks for `dbg.declare` metadata and if it
/// exist the filter marks appropriate location as a not promoted.
void markIfNotPromoted(const llvm::DataLayout &DL, DIMemoryTrait &T);
}
#endif//TSAR_MEMORY_TRAIT_FILTER_H
