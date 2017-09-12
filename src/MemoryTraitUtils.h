//===- MemoryTraitUtils.h - Utils For Exploring Memory Traits ----- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines useful functions to explore traits of memory locations.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_MEMORY_TRAITS_UTILS_H
#define TSAR_MEMORY_TRAITS_UTILS_H

#include "EstimateMemory.h"
#include "tsar_trait.h"

namespace tsar {
namespace detail {
template<template<class Element, class Coll> class Inserter, class Coll>
inline void explicitAccessCoverage(
    const DependencySet &DS, const AliasNode &N, Coll &C) {
  auto AT = DS.find(&N);
  if (AT == DS.end() || !AT->is<trait::ExplicitAccess>())
    for (auto &Child : make_range(N.child_begin(), N.child_end()))
      detail::explicitAccessCoverage<Inserter>(DS, Child, C);
  else
    Inserter<const AliasNode *, Coll>::insert(&N, C);
}
}

/// Returns a number of the smallest alias nodes which covers all explicit
/// memory accesses in the region.
template<
  template<class Element, class Coll> class Inserter = bcl::PushBackInserter,
  class Coll>
inline void explicitAccessCoverage(
    const DependencySet &DS, const AliasTree &AT, Coll &C) {
  detail::explicitAccessCoverage<Inserter>(DS, *AT.getTopLevelNode(), C);
}
}
#endif//TSAR_MEMORY_TRAITS_UTILS_H
