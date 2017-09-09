//===--- MemoryCoverage.h -- Estimate Memory Coverage -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file proposes functions to explore relation between groups of
// estimate memory locations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_COVERAGE_H
#define TSAR_MEMORY_COVERAGE_H

#include "EstimateMemory.h"
#include "GraphNumbering.h"
#include <tagged.h>
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
  assert(Numbers.size() == AT.size() &&
    "A graph numbering must contain all nodes of the graph!");
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
  // equivalence. To locations are related to the same class if they may alias.
  // Reverse postorder numbering is used further to determine boundary of each
  // class.
  std::map<std::size_t,
    bcl::tagged_pair<
      bcl::tagged<std::size_t, ReversePostorder>,
      bcl::tagged<uint64_t, Size>>> PreorderTraversal;
  for (PointeeItr I = BeginItr, E = EndItr; I != E; ++I) {
    auto NodeItr = Numbers.find(I->getAliasNode(AT));
    assert(NodeItr != Numbers.end() && "Number of an alias node  must be set!");
    PreorderTraversal.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(NodeItr->get<Preorder>()),
      std::forward_as_tuple(NodeItr->get<ReversePostorder>(), I->getSize()));
  }
  uint64_t TotalSize = 0;
  auto PreorderItr = PreorderTraversal.begin();
  auto PreorderEndItr = PreorderTraversal.end();
  auto SubTreeRoot = PreorderItr->second.get<ReversePostorder>();
  auto CurrentSize = PreorderItr->second.get<Size>();
  for (++PreorderItr; PreorderItr != PreorderEndItr; ++PreorderItr) {
    if (SubTreeRoot < PreorderItr->second.get<ReversePostorder>()) {
      CurrentSize = std::max(
        CurrentSize, PreorderItr->second.get<Size>());
      continue;
    }
    TotalSize += CurrentSize;
    CurrentSize = PreorderItr->second.get<Size>();
  }
  TotalSize += CurrentSize;
  return TotalSize >= EM.getSize();
}
}
#endif//TSAR_MEMORY_COVERAGE_H
