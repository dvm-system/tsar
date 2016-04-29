//===--- tsar_trait.cpp ------ Analyzable Traits ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements traits which could be recognized by the analyzer.
//
//===----------------------------------------------------------------------===//

#include "tsar_trait.h"

/// \brief Definition of a trait recognized by analyzer.
///
/// Use this macro with TSAR_TRAIT_DECL
#define TSAR_TRAIT_DEF(name_) \
constexpr tsar::trait::name_##Ty tsar::trait::name_; \
constexpr unsigned long long tsar::trait::name_##Ty::Id;

TSAR_TRAIT_DEF(Analyze)
TSAR_TRAIT_DEF(NoAccess)
TSAR_TRAIT_DEF(AddressAccess)
TSAR_TRAIT_DEF(Shared)
TSAR_TRAIT_DEF(Private)
TSAR_TRAIT_DEF(FirstPrivate)
TSAR_TRAIT_DEF(SecondToLastPrivate)
TSAR_TRAIT_DEF(LastPrivate)
TSAR_TRAIT_DEF(DynamicPrivate)
TSAR_TRAIT_DEF(Dependency)

#undef TSAR_TRAIT_DEF