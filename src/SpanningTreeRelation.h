//===- SpanningTreeRelation.h - Nodes Connectivity Checker ------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

namespace tsar {
/// This determine relation between two nodes in a spanning tree.
template<class GraphType>
class SpanningTreeRelation {
  using NodeRef = typename llvm::GraphTraits<GraphType>::NodeRef;
public:
  /// Represents node relation.
  enum Relation : int8_t {
    FIRST_RELATION = 0,
    RELATION_ANCESTOR = FIRST_RELATION,
    RELATION_DESCENDANT,
    RELATION_EQUAL,
    RELATION_UNREACHABLE,
    LAST_RELATION = RELATION_UNREACHABLE,
    INVALID_RELATION,
    NUMBER_RELATION = INVALID_RELATION
  };

  /// Performs initialization to determine relation of two nodes.
  explicit SpanningTreeRelation(const GraphType &G) {
    numberGraph(G, &mNumbering);
  }

  /// Determines relation between two nodes in a spanning tree.
  Relation compare(NodeRef LHS, NodeRef RHS) const {
    if (LHS == RHS)
      return RELATION_EQUAL;
    auto LeftItr = mNumbering.find(LHS);
    auto RightItr = mNumbering.find(RHS);
    if (LeftItr->get<Preorder>() < RightItr->get<Preorder>() &&
        LeftItr->get<ReversePostorder>() < RightItr->get<ReversePostorder>())
      return RELATION_ANCESTOR;
    if (LeftItr->get<Preorder>() > RightItr->get<Preorder>() &&
      LeftItr->get<ReversePostorder>() > RightItr->get<ReversePostorder>())
      return RELATION_DESCENDANT;
    return RELATION_UNREACHABLE;
  }

  /// Checks whether the 'What' node in ancestor of 'Of' node.
  bool isAncestor(NodeRef What, NodeRef Of) const {
    return compare(What, Of) == RELATION_ANCESTOR;
  }

  /// Checks whether the 'What' node in descendant of 'Of' node.
  bool isDescendant(NodeRef What, NodeRef Of) const {
    return compare(What, Of) == RELATION_DESCENDANT;
  }

  /// Checks whether specified nodes are neither ancestor nor descendant.
  bool isUnreachable(NodeRef LHS, NodeRef RHS) const {
    return compare(LHS, RHS) == RELATION_UNREACHABLE;
  }

private:
  GraphNumbering<typename llvm::GraphTraits<GraphType>::NodeRef> mNumbering;
};
}
#endif//TSAR_SPANNING_TREE_RELATION_H
