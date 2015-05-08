//===----- tsar_graph.h -------- Graph Properies ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines functions to evaluate different properties of the graph.
// Various bits of information can be calculated, for example:
//  * whether the directed graph is acyclic (DAG)
//  * back edges of the the graph 
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

/// \brief Returns first back edge in the specified directed graph.
///
/// This function implements a preorder traversal using DFS and calculates
/// the nearest back edge to this node. The DFS starts from the specified node
/// assuming that nodes from the specified list have been already visited and
/// colored.
/// \param [in] N Node that must be visited.
/// \param [in, out] VisitedNodes Already visited nodes.
/// \return Back edge represented as a pair of two nodes: successor and predecessor.
/// If back edge in not found this function returns a pair of nullptrs
template<class GraphType>
std::pair<
  typename llvm::GraphTraits<GraphType *>::NodeType *,
  typename llvm::GraphTraits<GraphType *>::NodeType *> findBackEdge(
    typename llvm::GraphTraits<GraphType *>::NodeType *N,
    llvm::DenseMap<typename llvm::GraphTraits<GraphType *>::NodeType *, DFSColor>
    &VisitedNodes) {
  typedef llvm::GraphTraits<GraphType *> GT;
  typedef typename GT::NodeType NodeType;
  assert(N && "Visited node must not be null!");
  VisitedNodes.insert(std::make_pair(N, COLOR_GRAY));
  for (auto CI = GT::child_begin(N), CE = GT::child_end(N); CI != CE; ++CI) {
    auto VI = VisitedNodes.find(*CI);
    if (VI == VisitedNodes.end()) {
      std::pair<NodeType *, NodeType *> BE =
        findBackEdge<GraphType>(*CI, VisitedNodes);
      if (BE.first)
        return BE;
    } else if (VI->second == COLOR_GRAY) {
      return std::make_pair(N, *CI);
    }
    VI->second = COLOR_BLACK;
  }
  return std::pair<NodeType *, NodeType *>(nullptr, nullptr);
}

/// \brief Returns true if the specified directed graph is acyclic.
///
/// \param [in] G Directed graph, it can not be null.
/// \pre The llvm::GraphTraits class should be specialized by GraphType *.
template<class GraphType> bool isDAG(GraphType *G) {
  typedef llvm::GraphTraits<GraphType *> GT;
  typedef typename GT::NodeType NodeType;
  assert(G && "Graph must not be null!");
  llvm::DenseMap<NodeType *, DFSColor> VisitedNodes;
  return !findBackEdge<GraphType>(GT::getEntryNode(G), VisitedNodes).first;
}
}
#endif//TSAR_GRAPH_H
