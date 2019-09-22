//===- SpanningTreeRelation.h - Nodes Connectivity Checker ------*- C++ -*-===//
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
// This file defines classes to check whether two nodes in spanning tree are
// connected and to determine ancestor and descendant.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SPANNING_TREE_RELATION_H
#define TSAR_SPANNING_TREE_RELATION_H

#include "GraphNumbering.h"
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/Optional.h>
#include <type_traits>

namespace tsar {
/// Represents node relation in a tree.
enum TreeRelation : int8_t {
  FIRST_TR = 0,
  TR_ANCESTOR = FIRST_TR,
  TR_DESCENDANT,
  TR_EQUAL,
  TR_UNREACHABLE,
  LAST_TR = TR_UNREACHABLE,
  INVALID_TR,
  NUMBER_TR = INVALID_TR
};

/// This determine relation between two nodes in a spanning tree.
template<class GraphType>
class SpanningTreeRelation {
  using NodeRef = typename llvm::GraphTraits<GraphType>::NodeRef;
public:

  /// Performs initialization to determine relation of two nodes.
  explicit SpanningTreeRelation(const GraphType &G) {
    numberGraph(G, &mNumbering);
  }

  /// Determines relation between two nodes in a spanning tree.
  TreeRelation compare(NodeRef LHS, NodeRef RHS) const {
    if (LHS == RHS)
      return TR_EQUAL;
    auto LeftItr = mNumbering.find(LHS);
    auto RightItr = mNumbering.find(RHS);
    assert(LeftItr != mNumbering.end() &&
      "LHS must be a node of a spanning tree!");
    assert(RightItr != mNumbering.end() &&
      "RHS must be a node of a spanning tree!");
    if (LeftItr->template get<Preorder>() <
            RightItr->template get<Preorder>() &&
        LeftItr->template get<ReversePostorder>() <
            RightItr->template get<ReversePostorder>())
      return TR_ANCESTOR;
    if (LeftItr->template get<Preorder>() >
            RightItr->template get<Preorder>() &&
      LeftItr->template get<ReversePostorder>() >
            RightItr->template get<ReversePostorder>())
      return TR_DESCENDANT;
    return TR_UNREACHABLE;
  }

  bool isEqual(NodeRef LHS, NodeRef RHS) const {
    return compare(LHS, RHS) == TR_EQUAL;
  }

  /// Checks whether the 'What' node is an ancestor of 'Of' node.
  bool isAncestor(NodeRef What, NodeRef Of) const {
    return compare(What, Of) == TR_ANCESTOR;
  }

  /// Checks whether the 'What' node is a descendant of 'Of' node.
  bool isDescendant(NodeRef What, NodeRef Of) const {
    return compare(What, Of) == TR_DESCENDANT;
  }

  /// Checks whether specified nodes are neither ancestor nor descendant.
  bool isUnreachable(NodeRef LHS, NodeRef RHS) const {
    return compare(LHS, RHS) == TR_UNREACHABLE;
  }

private:
  GraphNumbering<NodeRef> mNumbering;
};

/// \brief Returns a parent of a specified node in a spanning tree of a graph.
///
/// \pre llvm::GraphTraits must be available for GraphType and
/// llvm::Inverse<GraphType>. References to a node of a graph must be
/// assignable from a reference to a node in an inverse graph.
/// \return A parent of a specified node.
template<class GraphType>
llvm::Optional<
  typename llvm::GraphTraits<llvm::Inverse<GraphType>>::NodeRef> findParent(
    typename llvm::GraphTraits<llvm::Inverse<GraphType>>::NodeRef N,
    const SpanningTreeRelation<GraphType> &STR) {
  using GT = llvm::GraphTraits<GraphType>;
  using IGT = llvm::GraphTraits<llvm::Inverse<GraphType>>;
  using NodeRef = typename IGT::NodeRef;
  static_assert(std::is_assignable<typename GT::NodeRef, NodeRef>::value ||
    std::is_convertible<NodeRef, typename GT::NodeRef>::value,
    "NodeRef of a graph must be assignable from NodeRef of an inverse graph!");
  for (auto I = IGT::child_begin(N), E = IGT::child_end(N); I != E; ++I) {
    if (STR.isDescendant(N, *I))
      return *I;
  }
  return llvm::None;
}

/// \brief Finds a lowest common ancestor for specified nodes in
/// a spanning tree (result is not equal to any node).
///
/// \pre
/// - A spanning tree must contains all nodes from a [BeginItr, EndItr).
/// - The specified iterator range must not be empty.
/// - llvm::GraphTraits must be available for GraphType and
/// llvm::Inverse<GraphType>.
/// - Reference to a node of a graph must be assignable or convertible from
/// a reference to a node in an inverse graph.
/// - Reference to a node of an inverse graph must be assignable or convertible
/// from dereferenced ItrTy.
template<class GraphType, class ItrTy>
llvm::Optional<
  typename llvm::GraphTraits<llvm::Inverse<GraphType>>::NodeRef> findLCA(
    const SpanningTreeRelation<GraphType> &STR, ItrTy BeginItr, ItrTy EndItr) {
  assert(BeginItr != EndItr &&
    "At least one node must be in a iterator range!");
  using STRTy = SpanningTreeRelation<GraphType>;
  using GT = llvm::GraphTraits<GraphType>;
  using IGT = llvm::GraphTraits<llvm::Inverse<GraphType>>;
  using NodeRef = typename IGT::NodeRef;
  static_assert(std::is_assignable<typename GT::NodeRef, NodeRef>::value ||
    std::is_convertible<NodeRef, typename GT::NodeRef>::value,
    "NodeRef of a graph must be assignable from NodeRef of an inverse graph!");
  auto Parent = findParent(*BeginItr, STR);
  if (!Parent.hasValue())
    return llvm::None;
  NodeRef LCA = *Parent;
  auto CurrItr = BeginItr;
  for (++CurrItr; CurrItr != EndItr; ++CurrItr) {
    auto R = STR.compare(LCA, *CurrItr);
    for (; R == TR_UNREACHABLE || R == TR_EQUAL;
           R = STR.compare(LCA, *CurrItr)) {
      auto Parent = findParent(LCA, STR);
      if (!Parent.hasValue())
        return llvm::None;
      LCA = *Parent;
    }
    switch (R) {
    case TR_ANCESTOR: break;
    case TR_DESCENDANT:
      auto Parent = findParent(*CurrItr, STR);
      if (!Parent.hasValue())
        return llvm::None;
      LCA = *Parent;
      break;
    }
  }
  return LCA;
}
}
#endif//TSAR_SPANNING_TREE_RELATION_H
