//===------- DataFlow.h ----- Data-Flow Framework ---------------*- C++ -*-===//
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
// This file defines abstract representation of data-flow problem. The data-flow
// problem is to find a solution to a set of constraints on data-flow values for
// all nodes of the specified directed graph. The data-flow value represents an
// abstraction of the set of all possible states that can be observed for
// the node. The data-flow value before and after each node n is denoted by
// IN[n] and OUT[n], respectively. In general case data-flow problem can be
// solved by an iterative algorithm. The input of the algorithm is a data-flow
// framework. Details can be found in the book
// "Compilers: Principles, Techniques, and Tools" written by Alfred V. Aho,
// Monica S. Lam, Ravi Sethi, and Jeffrey D. Ullman.
//
// There are following main elements in this file:
//  * DataFlowTraits - It must be specialized to determine data-flow framework.
//  * RegionDFTraits - It must be specialized to determine data-flow framework
//                     for a hierarchy of regions.
//  * solveDataFlow...() - It should be used to solve data-flow problem.
//  * SmallDFNode - It can be inherited to represent nodes of a data-flow graph.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DATA_FLOW_H
#define TSAR_DATA_FLOW_H

#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <algorithm>
#include <iterator>
#include <type_traits>
#include <vector>
#include <bcl/utility.h>

namespace tsar {
/// \brief Data-flow framework.
///
/// This class should be specialized by different framework types
/// which is why the default version is empty. The specialization is used
/// to solve forward or backward data-flow problems. A direction of data-flow
/// should be explicitly set via definitions of functions that allow iteration
/// over all children of the specified node.
/// The following elements should be provided:
/// - typedef GraphType -
///     Type of a data-flow graph. The llvm::GraphTraits class from
///     llvm/ADT/GraphTraits.h should be specialized by GraphType and by
///     llvm::Inverse<GraphType> (if it is necessary to solve a data-flow
///     problem in a topological order.
///     Note that definition of nodes_begin() and nodes_end()
///     should iterate over all nodes in the graph excepted the entry node.
///     If a specialization of GraphTraits already exists and does not meet
///     this requirement specialize it by Forward<GraphType> or
///     Backward<GraphType> instead.
/// - typedef ValueType - Type of a data-flow value.
/// - static ValueType topElement(DFFwk &, GraphType &) -
///     Returns top element for the data-flow framework.
/// - static ValueType boundaryCondition(DFFwk &, GraphType &) -
///     Returns boundary condition for the data-flow framework.
/// - static void setValue(ValueType, NodeRef, DFFwk &),
///   static ValueType getValue(NodeRef, DFFwk &) -
///     Allow to access data-flow value for the specified node.
/// - static void initialize(NodeRef, DFFwk &, GraphType &) -
///     Initializes auxiliary information which is necessary
///     to perform analysis. For example, it is possible to allocate
///     some memory or attributes to each node, etc.
///     Do not set initial data-flow values in this function because they will
///     be overwritten by the data-flow solver function.
/// - static meetOperator(const ValueType &, ValueType &, DFFwk &, GraphType &) -
///     Evaluates a meet operator, the result is stored in the second
///     parameter.
/// - static bool transferFunction(ValueType, NodeRef, DFFwk &, GraphType &)
///     Evaluates a transfer function for the specified node. This returns
///     true if produced data-flow value differs from the data-flow value
///     produced on previous iteration of the data-flow analysis algorithm.
///     For the first iteration the new value compares with the initial one.
///
/// Direction of the data-flow is specified by child_begin and child_end
/// functions which is defined in the llvm::GraphTraits<GraphType> class.
template <class DFFwk> struct DataFlowTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename DFFwk::UnknownFrameworkError GraphType;
};


/// \brief This class is used as a little marker class to tell
/// the data-flow solver to solve a data-flow problem in forward direction.
///
/// The GraphTraits class should be specialized by the Forward<GraphType>.
template <class GraphType> struct Forward {
  const GraphType &Graph;
  inline Forward(const GraphType &G) : Graph(G) {}
};

/// \brief This class is used as a little marker class to tell
/// the data-flow solver to solve a data-flow problem in backward direction.
///
/// The GraphTraits class should be specialized by the Backward<GraphType>.
template <class GraphType> struct Backward {
  const GraphType &Graph;
  inline Backward(const GraphType &G) : Graph(G) {}
};

/// \brief Iteratively solves data-flow problem.
///
/// This computes IN and OUT for each node in the specified data-flow graph
/// by successive approximation. The last computed value for each node
/// can be obtained by calling the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \param [in, out] DFG Data-flow graph specified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention The DataFlowTraits class should be specialized by DFFwk.
/// Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by
/// DataFlowTraits<DFFwk>::GraphType.
/// \pre The graph must not contain unreachable nodes.
template<class DFFwk> void solveDataFlowIteratively(DFFwk DFF,
    typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef DataFlowTraits<DFFwk> DFT;
  typedef typename DFT::ValueType ValueType;
  typedef typename DFT::GraphType GraphType;
  typedef llvm::GraphTraits<GraphType> GT;
  typedef typename GT::nodes_iterator nodes_iterator;
  typedef typename GT::ChildIteratorType ChildIteratorType;
  for (nodes_iterator I = GT::nodes_begin(DFG), E = GT::nodes_end(DFG);
       I != E; ++I) {
    DFT::initialize(*I, DFF, DFG);
    DFT::setValue(DFT::topElement(DFF, DFG), *I, DFF);
  }
  DFT::initialize(GT::getEntryNode(DFG), DFF, DFG);
  DFT::setValue(DFT::boundaryCondition(DFF, DFG), GT::getEntryNode(DFG), DFF);
  bool isChanged = true;
  do {
    isChanged = false;
    for (nodes_iterator I = GT::nodes_begin(DFG), E = GT::nodes_end(DFG);
         I != E; ++I) {
      assert((*I == GT::getEntryNode(DFG) ||
        GT::child_begin(*I) != GT::child_end(*I)) &&
        "Data-flow graph must not contain unreachable nodes!");
      ValueType Value(DFT::topElement(DFF, DFG));
      for (ChildIteratorType CI = GT::child_begin(*I), CE = GT::child_end(*I);
           CI != CE; ++CI) {
        DFT::meetOperator(DFT::getValue(*CI, DFF), Value, DFF, DFG);
      }
      isChanged =
        DFT::transferFunction(std::move(Value), *I, DFF, DFG) || isChanged;
    }
  } while (isChanged);
}

/// \brief Solves data-flow problem in topological order during one iteration.
///
/// This computes IN and OUT for each node in the specified data-flow graph
/// in a topological order, so a graph traversal is executed only two times.
/// Firstly to calculate order of nodes and secondly to solve data-flow problem.
/// The last computed value for each node can be obtained by calling
/// the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \param [in, out] DFG Data-flow graph specified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention The DataFlowTraits class should be specialized by DFFwk.
/// Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by
/// DataFlowTraits<DFFwk>::GraphType.
/// \pre The graph must not contain unreachable nodes.
template<class DFFwk> void solveDataFlowTopologicaly(DFFwk DFF,
    typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef DataFlowTraits<DFFwk> DFT;
  typedef typename DFT::ValueType ValueType;
  typedef typename DFT::GraphType GraphType;
  typedef llvm::GraphTraits<GraphType> GT;
  typedef typename GT::nodes_iterator nodes_iterator;
  typedef typename GT::ChildIteratorType ChildIteratorType;
  typedef typename GT::NodeRef NodeRef;
#ifdef LLVM_DEBUG
  for (nodes_iterator I = GT::nodes_begin(DFG), E = GT::nodes_end(DFG);
       I != E; ++I)
    assert((*I == GT::getEntryNode(DFG) ||
      GT::child_begin(*I) != GT::child_end(*I)) &&
      "Data-flow graph must not contain unreachable nodes!");
#endif
  typedef llvm::po_iterator<
    GraphType, llvm::SmallPtrSet<NodeRef, 8>, false,
    llvm::GraphTraits<llvm::Inverse<GraphType> > > po_iterator;
  typedef std::vector<NodeRef> RPOTraversal;
  typedef typename RPOTraversal::reverse_iterator rpo_iterator;
  // We do not use llvm::ReversePostOrderTraversal class because its
  // implementation requires that llvm::GraphTraits is specialized by
  // NodeRef.
  RPOTraversal RPOT;
  std::copy(po_iterator::begin(DFG), po_iterator::end(DFG),
            std::back_inserter(RPOT));
  rpo_iterator I = RPOT.rbegin(), E = RPOT.rend();
  assert(*I == GT::getEntryNode(DFG) &&
          "The first node in the topological order differs from the entry node in the data-flow framework!");
  for (++I; I != E; ++I) {
    DFT::initialize(*I, DFF, DFG);
    DFT::setValue(DFT::topElement(DFF, DFG), *I, DFF);
  }
  DFT::initialize(GT::getEntryNode(DFG), DFF, DFG);
  DFT::setValue(DFT::boundaryCondition(DFF, DFG), GT::getEntryNode(DFG), DFF);
  for (I = RPOT.rbegin(), ++I; I != E; ++I) {
    ValueType Value(DFT::topElement(DFF, DFG));
    for (ChildIteratorType CI = GT::child_begin(*I), CE = GT::child_end(*I);
         CI != CE; ++CI) {
      DFT::meetOperator(DFT::getValue(*CI, DFF), Value, DFF, DFG);
    }
    DFT::transferFunction(std::move(Value), *I, DFF, DFG);
  }
}

/// \brief Data-flow framework for a hierarchy of regions.
///
/// This class should be specialized by different region types
/// which is why the default version is empty.
/// The specialization is used to solve forward or backward data-flow problems
/// for a hierarchy of regions. Each region is represented by a data-flow graph.
/// Nodes of this graph are simple nodes or internal regions which are
/// also represented by other data-flow graphs.
/// The following elements should be provided:
/// - static void expand(DFFwk &, GraphType &) -
///     Expands a region to a data-flow graph which represents it.
/// - static void collapse(DFFwk &, GraphType &) -
///     Collapses a data-flow graph which represents a region to a one node
///     in a data-flow graph of an outer region.
/// - typedef region_iterator,
///   static region_iterator region_begin(GraphType &G),
///   static region_iterator region_end (GraphType &G) -
///     Allow iteration over all internal regions in the specified region.
/// \note It may be convenient to inherit DataFlowTraits to specialize this
/// class.
/// \note Whether regions at different levels of hierarchy have the same type
/// or not? They have the same type. The hierarchy of subregions is regarded as
/// a single entity which is formed by regions at different levels. By analogy
/// with the graph in which all nodes have a common type, otherwise it is
/// impossible to properly implement the traversal. It is possible to implement
/// difference of nodes, for example, using inheritance. Thus collapse
/// function must receive a common type, as well as the transfer function,
/// that takes a common type of graph nodes.
template<class DFFwk > struct RegionDFTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename DFFwk::UnknownFrameworkError GraphType;
};

/// \brief Solves data-flow problem for the specified hierarchy of regions.
///
/// The data-flow problems solves upward from innermost regions to the region
/// associated with the specified data-flow graph. Before solving the data-flow
/// problem of some region all inner regions will be collapsed to a one node in
/// a data-flow graph associated with this region. The specified graph will be
/// also collapsed because the outermost graph is a graph, which contains
/// one node which is associated with the whole specified graph. When traversing
/// from the specified graph to innermost graphs, regions will be consistently
/// expanded to a data-flow graph. If it is possible the problem will be solved
/// in topological order in a single pass, otherwise iteratively.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \param [in, out] DFG Data-flow graph specified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention DataFlowTraits and RegionDFTraits classes should be specialized
/// by DFFwk. Note that DFFwk is generally a pointer type.
/// The llvm::GraphTraits class should be specialized by type of each
/// regions in the hierarchy (not only for DataFlowTraits<DFFwk>::GraphType).
/// Note that type of region is generally a pointer type.
/// \pre The graph must not contain unreachable nodes.
template<class DFFwk> void solveDataFlowUpward(DFFwk DFF,
    typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef RegionDFTraits<DFFwk> RT;
  typedef typename RT::region_iterator region_iterator;
  RT::expand(DFF, DFG);
  for (region_iterator I = RT::region_begin(DFG), E = RT::region_end(DFG);
       I != E; ++I)
    solveDataFlowUpward(DFF, *I);
  if (isDAG(DFG))
    solveDataFlowTopologicaly(DFF, DFG);
  else
    solveDataFlowIteratively(DFF, DFG);
  RT::collapse(DFF, DFG);
}

/// \brief Solves data-flow problem for the specified hierarchy of regions.
///
/// The data-flow problems solves downward from to the region associated with
/// the specified data-flow graph to innermost regions. Before solving
/// the data-flow problem of some region the node associated with this region in
/// outer graph will be expanded to a data-flow graph. The outermost graph is
/// a graph, which contains one node which is associated with the whole
/// specified graph. The specified graph is treated as expansion of this node.
/// After solving the data-flow problem of some region it will be collapsed to
/// a one node in a data-flow graph associated with this region.
/// The specified graph will be also collapsed
/// If it is possible the problem will be solved in topological order
/// in a single pass, otherwise iteratively.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \param [in, out] DFG Data-flow graph specified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention DataFlowTraits and RegionDFTraits classes should b e specialized
/// by DFFwk. Note that DFFwk is generally a pointer type.
/// The llvm::GraphTraits class should be specialized by type of each
/// regions in the hierarchy (not only for DataFlowTraits<DFFwk>::GraphType).
/// Note that type of region is generally a pointer type.
/// \pre The graph must not contain unreachable nodes.
template<class DFFwk> void solveDataFlowDownward(DFFwk DFF,
  typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef RegionDFTraits<DFFwk> RT;
  typedef typename RT::region_iterator region_iterator;
  RT::expand(DFF, DFG);
  if (isDAG(DFG))
    solveDataFlowTopologicaly(DFF, DFG);
  else
    solveDataFlowIteratively(DFF, DFG);
  for (region_iterator I = RT::region_begin(DFG), E = RT::region_end(DFG);
       I != E; ++I)
    solveDataFlowDownward(DFF, *I);
  RT::collapse(DFF, DFG);
}

namespace detail{
/// This covers an IN value for a data-flwo node.
template<class InTy> class DFValueIn {
public:
  /// Returns a data-flow value before the node.
  const InTy & getIn() const { return mIn; }

  /// Specifies a data-flow value before the node.
  void setIn(InTy V) { mIn = std::move(V); }

private:
  InTy mIn;
};

/// This covers an OUT value for a data-flwo node.
template<class OutTy> class DFValueOut {
public:
  /// Returns a data-flow value after the node.
  const OutTy & getOut() const { return mOut; }

  /// Specifies a data-flow value after the node.
  void setOut(OutTy V) { mOut = std::move(V); }

private:
  OutTy mOut;
};
}
/// \brief This covers IN and OUT values for a data-flow node.
///
/// \tparam Id Identifier, for example a data-flow framework which is used.
/// This is necessary to distinguish different data-flow values.
/// \tparam InTy Type of data-flow value before the node (IN).
/// \tparam OutTy Type of data-flow value after the node (OUT).
///
/// It is possible to set InTy or OutTy to void. In this Case
/// corresponding methods (get and set) are not available.
template<class Id, class InTy, class OutTy = InTy >
class DFValue :
  public std::conditional<std::is_void<InTy>::value,
    Utility::Null, detail::DFValueIn<InTy>>::type,
  public std::conditional<std::is_void<OutTy>::value,
    Utility::Null, detail::DFValueOut<OutTy>>::type {};

/// \brief Instances of this class are used to represent a node
/// with a small list of the adjacent nodes.
///
/// This is a node, optimized for the case when a number of adjacent nodes
/// is small. It contains some number of adjacent nodes in-place,
/// which allows it to avoid heap allocation when the actual number of
/// nodes is below that threshold (2*N). This allows normal "small" cases to be
/// fast without losing generality for large inputs.
///
/// Multiple edges between adjacent nodes are allowed.
/// \attention Iterator validity is the same as for operations with
/// llvm::SmallVector.
template<class NodeTy, unsigned N>
class SmallDFNode : private bcl::Uncopyable {
public:
  /// Direction to adjacent node.
  enum Direction {
    FIRST_DIRECTION = 0,
    PRED = FIRST_DIRECTION,
    SUCC,
    LAST_DIRECTION = SUCC,
    INVALID_DIRECTION,
    NUMBER_DIRECTION = INVALID_DIRECTION
  };

  /// Type used to iterate over successors.
  typedef typename llvm::SmallVectorImpl<NodeTy *>::const_iterator succ_iterator;

  /// Range of successors.
  typedef llvm::iterator_range<succ_iterator> succ_range;

  /// Type used to iterate over predecessors.
  typedef typename llvm::SmallVectorImpl<NodeTy *>::const_iterator pred_iterator;

  /// Range of predecessors.
  typedef llvm::iterator_range<pred_iterator> pred_range;

  /// Type used to represent number of adjacent nodes.
  typedef typename llvm::SmallVector<NodeTy *, N>::size_type size_type;

  /// Returns iterator that points to the beginning of the successor list.
  succ_iterator succ_begin() const { return mAdjacentNodes[SUCC].begin(); }

  /// Returns iterator that points to the ending of the successor list.
  succ_iterator succ_end() const { return mAdjacentNodes[SUCC].end(); }

  /// Returns range of predecessors.
  succ_range successors() const {
    return llvm::make_range(succ_begin(), succ_end());
  }

  /// Returns iterator that points to the beginning of the predecessor list.
  pred_iterator pred_begin() const { return mAdjacentNodes[PRED].begin(); }

  /// Returns iterator that points to the ending of the predecessor list.
  pred_iterator pred_end() const { return mAdjacentNodes[PRED].end(); }

  /// Returns range of predecessors.
  pred_range predecessors() const {
    return llvm::make_range(pred_begin(), pred_end());
  }

  /// Returns true if the specified node is a successor of this node.
  bool isSuccessor(NodeTy *Node) const {
    assert(Node && "Data-flow node must not be null!");
    for (NodeTy *CurrNode : mAdjacentNodes[SUCC])
      if (CurrNode == Node) return true;
    return false;
  }

  /// Returns true if the specified node is a predecessor of this node.
  bool isPredecessor(NodeTy *Node) const {
    assert(Node && "Data-flow node must not be null!");
    for (NodeTy *CurrNode : mAdjacentNodes[PRED])
      if (CurrNode == Node) return true;
    return false;
  }

  /// Returns true if the specified node is an adjacent node to this node.
  bool isAdjacent(NodeTy *Node) const {
    return isSuccessor(Node) || isPredecessor(Node);
  }

  /// Adds adjacent node in the specified direction.
  void addAdjacentNode(NodeTy *Node, Direction Dir) {
    assert(Node && "New data-flow node must not be null!");
    assert(FIRST_DIRECTION <= Dir && Dir <= LAST_DIRECTION &&
            "Direction is out of range!");
    mAdjacentNodes[Dir].push_back(Node);
  }

  /// \brief Adds predecessor.
  ///
  /// \pre A new node must not be null.
  void addPredecessor(NodeTy *Node) { addAdjacentNode(Node, PRED); }

  /// \brief Adds successor.
  ///
  /// \pre A new node must not be null.
  void addSuccessor(NodeTy *Node) { addAdjacentNode(Node, SUCC); }

  /// \brief Removes adjacent node in the specified direction.
  ///
  /// If there are multiple edges between specified and current node
  /// all edges will be removed.
  /// \pre A removed node must not be null.
  void removeAdjacentNode(NodeTy *Node, Direction Dir) {
    assert(Node && "Data-flow node must not be null!");
    assert(FIRST_DIRECTION <= Dir && Dir <= LAST_DIRECTION &&
      "Direction is out of range!");
    auto I = mAdjacentNodes[Dir].end();
    auto B = mAdjacentNodes[Dir].begin();
    if (B == I)
      return;
    --I;
    while (I != B)
      if (*I == Node) {
        auto R = I;
        --I;
        mAdjacentNodes[Dir].erase(R);
      } else {
        --I;
      }
    if (*I == Node)
      mAdjacentNodes[Dir].erase(I);
  }

  /// \brief Removes predecessor.
  ///
  /// \pre A removed node must not be null.
  void removePredecessor(NodeTy *Node) { removeAdjacentNode(Node, PRED); }

  /// \brief Removes successor.
  ///
  /// \pre A removed node must not be null.
  void removeSuccessor(NodeTy *Node) { removeAdjacentNode(Node, SUCC); }

  /// Returns number of adjacent nodes in the specified direction.
  size_type numberOfAdjacentNodes(Direction Dir) const {
    assert(FIRST_DIRECTION <= Dir && Dir <= LAST_DIRECTION &&
      "Direction is out of range!");
    return mAdjacentNodes[Dir].size();
  }

  /// Returns number of successors for the specified node.
  size_type numberOfSuccessors() const { return numberOfAdjacentNodes(SUCC); }

  /// Returns number of predecessors for the specified node.
  size_type numberOfPredecessors() const { return numberOfAdjacentNodes(PRED); }

private:
  llvm::SmallVector<NodeTy *, N> mAdjacentNodes[NUMBER_DIRECTION];
};
}

#endif//TSAR_DATA_FLOW_H
