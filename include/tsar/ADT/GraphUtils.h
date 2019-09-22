//===----- Graph.h -------- Graph Properties ---------------*- C++ -*-===//
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
//
//===----------------------------------------------------------------------===//
//
// This file defines functions to evaluate different properties of the graph.
// Various bits of information can be calculated, for example:
//  * whether the directed graph is acyclic (DAG)
//  * back edges of the graph
//  * etc.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_GRAPH_H
#define TSAR_GRAPH_H

#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/DenseMap.h>

namespace tsar {
/// Color of a node which depth-first search (DFS) has visited.
enum DFSColor {
  FIRST_COLOR,
  COLOR_WHITE = FIRST_COLOR,
  COLOR_GRAY,
  COLOR_BLACK,
  LAST_COLOR = COLOR_BLACK,
  INVALID_COLOR,
  NUMBER_COLOR = INVALID_COLOR
};

/// Color of a node which breadth-first search (BFS) has visited.
typedef DFSColor BFSColor;

namespace detail {
/// \brief Returns first back edge in the specified directed graph.
///
/// This function implements a preorder traversal using DFS and calculates
/// the nearest back edge to this node. The DFS starts from the specified node
/// assuming that nodes from the specified list have been already visited and
/// colored.
/// \param [in] N Node that must be visited.
/// \param [in, out] VisitedNodes Already visited nodes and the new node N
/// that must be visited.
/// \return Back edge represented as a pair of two nodes: successor and
/// predecessor.
/// If back edge in not found this function returns a pair of nullptrs
/// \pre The llvm::GraphTraits class should be specialized by GraphType.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
/// The new node N that must be visited should be located in the VisitedNodes
/// collection.
/// \note It is not possible to use generic depth first graph iterator
/// which is implemented in LLVM, because this implementation does not collect
/// information about color of the node. So we can not determine whether all
/// children of some node have been already visited.
template<class GraphType>
std::pair<
  typename llvm::GraphTraits<GraphType>::NodeRef,
  typename llvm::GraphTraits<GraphType>::NodeRef> findBackEdge(
    typename llvm::GraphTraits<GraphType>::NodeRef N,
    llvm::DenseMap<typename llvm::GraphTraits<GraphType>::NodeRef, DFSColor>
    &VisitedNodes) {
  typedef llvm::GraphTraits<GraphType> GT;
  typedef typename GT::NodeRef NodeRef;
  assert(N && "Node must not be null!");
  assert(VisitedNodes.count(N) &&
    "Node must be located in the VisitedNodes collection!");
  for (auto CI = GT::child_begin(N), CE = GT::child_end(N); CI != CE; ++CI) {
    // The new node is inserted in VisitedNodes before a recursive call of
    // the findBackEdge() function to reduce number of searches.
    auto VI = VisitedNodes.insert(std::make_pair(*CI, COLOR_GRAY));
    if (VI.second) {
      auto BE = findBackEdge<GraphType>(*CI, VisitedNodes);
      if (BE.first)
        return BE;
      // The color of the node becomes black only if the back edge is not found,
      // otherwise there is no assurance that all children have been opened.
      // The new search is necessary because the iterators in a DenseMap
      // are invalidated  whenever an insertion occurs.
      VisitedNodes.find(*CI)->second = COLOR_BLACK;
    } else if (VI.first->second == COLOR_GRAY) {
      return std::make_pair(N, *CI);
    }
  }
  return std::pair<NodeRef, NodeRef>(nullptr, nullptr);
}
}

/// \brief Returns first back edge in the specified directed graph.
///
/// This function implements a preorder traversal using DFS and calculates
/// the nearest back edge to the graph entry node.
/// \param [in] G Directed graph, it can not be null.
/// \return Back edge represented as a pair of two nodes: successor and
/// predecessor.
/// If back edge in not found this function returns a pair of nullptrs
/// \pre The llvm::GraphTraits class should be specialized by GraphType.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
template<class GraphType>
inline std::pair<
  typename llvm::GraphTraits<GraphType>::NodeRef,
  typename llvm::GraphTraits<GraphType>::NodeRef> findBackEdge(GraphType G) {
  typedef llvm::GraphTraits<GraphType > GT;
  typedef typename GT::NodeRef NodeRef;
  llvm::DenseMap<NodeRef, DFSColor> VisitedNodes;
  VisitedNodes.insert(std::make_pair(GT::getEntryNode(G), COLOR_GRAY));
  return detail::findBackEdge<GraphType>(GT::getEntryNode(G), VisitedNodes);
}

/// \brief Returns true if the specified directed graph is acyclic.
///
/// \param [in] G Directed graph, it can not be null.
/// \pre The llvm::GraphTraits class should be specialized by GraphType.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
template<class GraphType> inline bool isDAG(GraphType G) {
  return !findBackEdge<GraphType>(G).first;
}
}
#endif//TSAR_GRAPH_H
