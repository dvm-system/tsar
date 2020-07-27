//===--- MemoryCoverage.h -- Estimate Memory Coverage -----------*- C++ -*-===//
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
// This file proposes functions to explore relation between groups of
// estimate memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_COVERAGE_H
#define TSAR_MEMORY_COVERAGE_H

#include "tsar/ADT/GraphNumbering.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include <bcl/tagged.h>
#include <llvm/ADT/iterator.h>
#include <map>
#include <tuple>
#include <type_traits>

namespace tsar {
/// \brief Returns true if a specified location `EM` is covered by
/// a union of locations from a specified range [BeginItr, EndItr).
///
/// All locations should be from the same estimate memory tree. If a location
/// in the range is not associated with a tree containing `EM` it will be
/// ignored.
template<class ItrTy>
bool cover(
    const AliasTree &AT, const GraphNumbering<const AliasNode *> &Numbers,
    const EstimateMemory &EM, const ItrTy &BeginItr, const ItrTy &EndItr) {
  if (BeginItr == EndItr)
    return false;
  using PointeeItr = typename std::conditional<
    std::is_same<
      typename std::decay<decltype(*std::declval<ItrTy>())>::type,
      EstimateMemory>::value,
    ItrTy,
    llvm::pointee_iterator<ItrTy>>::type;
  struct Size {};
  // This map is used to sort memory locations according to preorder numbering
  // of alias tree nodes. This simplify devision of nodes into classes of
  // equivalence. Two locations are related to the same class if they may alias.
  // Reverse postorder numbering is used further to determine boundary of each
  // class.
  std::map<std::size_t,
    bcl::tagged_pair<
      bcl::tagged<std::size_t, ReversePostorder>,
      bcl::tagged<uint64_t, Size>>> PreorderTraversal;
  for (PointeeItr I = BeginItr, E = EndItr; I != E; ++I) {
    if (EM.isDescendantOf(*I))
      return true;
    if (EM.isUnreachable(*I))
      continue;
    if (!I->getSize().hasValue())
      return true;
    auto NodeItr = Numbers.find(I->getAliasNode(AT));
    assert(NodeItr != Numbers.end() && "Number of an alias node must be set!");
    PreorderTraversal.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(NodeItr->template get<Preorder>()),
      std::forward_as_tuple(
        NodeItr->template get<ReversePostorder>(), I->getSize().getValue()));
  }
  // Fixed size locations do not cover location of unknown size.
  if (!EM.getSize().hasValue())
    return false;
  uint64_t TotalSize = 0;
  auto PreorderItr = PreorderTraversal.begin();
  auto PreorderEndItr = PreorderTraversal.end();
  auto SubTreeRoot = PreorderItr->second.template get<ReversePostorder>();
  auto CurrentSize = PreorderItr->second.template get<Size>();
  for (++PreorderItr; PreorderItr != PreorderEndItr; ++PreorderItr) {
    if (SubTreeRoot < PreorderItr->second.template get<ReversePostorder>()) {
      CurrentSize = std::max(
        CurrentSize, PreorderItr->second.template get<Size>());
      continue;
    }
    TotalSize += CurrentSize;
    CurrentSize = PreorderItr->second.template get<Size>();
  }
  TotalSize += CurrentSize;
  return TotalSize >= EM.getSize().getValue();
}
}
#endif//TSAR_MEMORY_COVERAGE_H
