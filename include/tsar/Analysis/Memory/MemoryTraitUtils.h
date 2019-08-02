//===- MemoryTraitUtils.h - Utils For Exploring Memory Traits ----- C++ -*-===//
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
// This file defines useful functions to explore traits of memory locations.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_MEMORY_TRAITS_UTILS_H
#define TSAR_MEMORY_TRAITS_UTILS_H

namespace tsar {
namespace detail {
template<template<class Element, class Coll> class InserterT,
  class DependenceSetT, class AliasNodeT, class CollT>
inline void explicitAccessCoverage(bool IgnoreRedundant,
    const DependenceSetT &DS, const AliasNodeT &N, CollT &C) {
  auto ATraitItr = DS.find_as(&N);
  if (ATraitItr == DS.end() ||
      !ATraitItr->template is<trait::ExplicitAccess>() ||
      (IgnoreRedundant && !ATraitItr->template is<trait::NoRedundant>()))
    for (auto &Child : make_range(N.child_begin(), N.child_end()))
      detail::explicitAccessCoverage<InserterT>(IgnoreRedundant,DS, Child, C);
  else
    InserterT<const AliasNodeT *, CollT>::insert(&N, C);
}

template<
  template<class ElementT, class CollT> class InserterT,
  class DependenceSetT, class AliasNodeT, class CollT>
inline void accessCoverage(bool IgnoreRedundant,
    const DependenceSetT &DS, const AliasNodeT &N, CollT &C) {
  auto ATraitItr = DS.find_as(&N);
  if (ATraitItr == DS.end())
    return;
  if (ATraitItr->template is<trait::NoRedundant>() ||
      (!IgnoreRedundant && ATraitItr->template is<trait::Redundant>())) {
    InserterT<const AliasNodeT *, CollT>::insert(&N, C);
    return;
  }
  for (auto &Child : make_range(N.child_begin(), N.child_end()))
    detail::accessCoverage<InserterT>(IgnoreRedundant, DS, Child, C);
}
}

/// Returns a number of the smallest alias nodes which covers all explicit
/// memory accesses in the region.
template<
  template<class ElementT, class CollT> class InserterT = bcl::PushBackInserter,
  class DependenceSetT, class AliasTreeT, class CollT>
inline void explicitAccessCoverage(
    const DependenceSetT &DS, const AliasTreeT &AT, CollT &C,
    bool IgnoreRedundant = false) {
  detail::explicitAccessCoverage<InserterT>(
    IgnoreRedundant, DS, *AT.getTopLevelNode(), C);
}

/// Returns a number of the smallest alias nodes which covers all
/// memory accesses in the region.
template<
  template<class ElementT, class CollT> class InserterT = bcl::PushBackInserter,
  class DependenceSetT, class AliasTreeT, class CollT>
inline void accessCoverage(
    const DependenceSetT &DS, const AliasTreeT &AT, CollT &C,
    bool IgnoreRedundant = false) {
  detail::accessCoverage<InserterT>(
    IgnoreRedundant, DS, *AT.getTopLevelNode(), C);
}
}
#endif//TSAR_MEMORY_TRAITS_UTILS_H
