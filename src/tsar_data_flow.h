//===----- tsar_data_flow.h ---- Data-Flow Framework-------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <iterator>
#include <vector>
#include <algorithm>
#include <utility.h>

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
///     llvm::Inverse<GraphType> (if it is neccessary to slove a data-flow
///     problem in a topological order.
///     Note that definition of nodes_begin() and nodes_end()
///     should iterate over all nodes in the graph excepted the entry node.
///     If a specialization of GraphTraits already exists and does not meet
///     this requirement specialize it by Forward<GraphType> or
///     Backward<GraphType> instead.
/// - static GraphType getDFG(DFFwk &) -
///     Returns data-flow graph for the data-flow framework.
/// - typedef ValueType - Type of a data-flow value.
/// - static ValueType topElement(DFFwk &) -
///     Returns top element for the data-flow framework.
/// - static ValueType boundaryCondition(DFFwk &) -
///     Returns boundary condition for the data-flow framework.
/// - static void setValue(ValueType, NodeType *),
///   static ValueType getValue(NodeType *) -
///     Allow to access data-flow value for the specified node.
/// - static void initialize(NodeType *, DFFwk &) -
///     Initializes auxiliary information which is neccessary
///     to perfrom analysis. For example, it is possible to allocate
///     some memory or attributes to each node, etc.
///     Do not set initial data-flow values in this function because they will
///     be overwritten by the data-flow solver function.
/// - static meetOperator(const ValueType &, ValueType &, DFFwk &) -
///     Evaluates a meet operator, the result is stored in the second
///     parameter.
/// - static bool transferFunction(ValueType, NodeType *, DFFwk &)
///     Evaluates a transfer function for the specified node. This returns
///     true if produced data-flow value differs from the data-flow value
///     produced on previouse iteration of the data-flow analysis algorithm.
///     For the first iteration the new value compares with the initial one.
///
/// Direction of the data-flow is specified by child_begin and child_end
/// functions which is defined is the GraphTraits<GraphType> class.
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
/// can be optained by calling the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \param [in, out] DFG Data-flow graph sepcified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention The DataFlowTraits class should be specialized by DFFwk.
/// Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by
/// DataFlowTraits<DFFwk>::GraphType.
template<class DFFwk> void solveDataFlowIteratively(DFFwk DFF,
    typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef DataFlowTraits<DFFwk> DFT;
  typedef typename DFT::ValueType ValueType;
  typedef typename DFT::GraphType GraphType;
  typedef GraphTraits<GraphType> GT;
  typedef typename GT::nodes_iterator nodes_iterator;
  typedef typename GT::ChildIteratorType ChildIteratorType;
  for (nodes_iterator I = GT::nodes_begin(DFG), E = GT::nodes_end(DFG);
       I != E; ++I) {
    DFT::initialize(*I, DFF);
    DFT::setValue(DFT::topElement(DFF), *I);
  }
  DFT::initialize(GT::getEntryNode(DFG), DFF);
  DFT::setValue(DFT::boundaryCondition(DFF), GT::getEntryNode(DFG));
  bool isChanged = true;
  do {
    isChanged = false;
    for (nodes_iterator I = GT::nodes_begin(DFG), E = GT::nodes_end(DFG);
    I != E; ++I) {
      assert(GT::child_begin(*I) != GT::child_end(*I) &&
        "Data-flow graph must not contain unreachable nodes!");
      ValueType Value(DFT::topElement(DFF));
      for (ChildIteratorType CI = GT::child_begin(*I), CE = GT::child_end(*I);
      CI != CE; ++CI) {
        DFT::meetOperator(DFT::getValue(*CI), Value, DFF);
      }
      isChanged = DFT::transferFunction(std::move(Value), *I, DFF) || isChanged;
    }
  } while (isChanged);
}

/// \brief Iteratively solves data-flow problem.
///
/// This computes IN and OUT for each node in the specified data-flow graph
/// by successive approximation. The last computed value for each node
/// can be optained by calling the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \attention The DataFlowTraits class should be specialized by DFFwk.
/// Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by
/// DataFlowTraits<DFFwk>::GraphType.
template<class DFFwk> void solveDataFlowIteratively(DFFwk DFF) {
  assert(DFF && "Data-flow framework must not be null!");
  typedef DataFlowTraits<DFFwk> DFT;
  solveDataFlowIteratively(DFF, DFT::getDFG(DFF));
}

/// \brief Solves data-flow problem in topological order during one iteration.
///
/// This computes IN and OUT for each node in the specified data-flow graph
/// in a topological order, so a graph traversal is executed only two times.
/// Firstly to calculate order of nodes and secondly to solve data-flow problem.
/// The last computed value for each node can be optained by calling
/// the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \param [in, out] DFG Data-flow graph sepcified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention The DataFlowTraits class should be specialized by DFFwk.
/// Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by
/// DataFlowTraits<DFFwk>::GraphType.
template<class DFFwk> void solveDataFlowTopologicaly(DFFwk DFF,
    typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef DataFlowTraits<DFFwk> DFT;
  typedef typename DFT::ValueType ValueType;
  typedef typename DFT::GraphType GraphType;
  typedef GraphTraits<GraphType> GT;
  typedef typename GT::nodes_iterator nodes_iterator;
  typedef typename GT::ChildIteratorType ChildIteratorType;
  typedef typename GT::NodeType NodeType;
  typedef llvm::po_iterator<
    GraphType, llvm::SmallPtrSet<NodeType *, 8>, false,
    llvm::GraphTraits<llvm::Inverse<GraphType> > > po_iterator;
  typedef std::vector<NodeType *> RPOTraversal;
  typedef typename RPOTraversal::reverse_iterator rpo_iterator;
  // We do not use llvm::ReversePostOrderTraversal class because
  // its implementation requires that GraphTraits is specialized by NodeType *.
  RPOTraversal RPOT;
  std::copy(po_iterator::begin(DFG), po_iterator::end(DFG),
            std::back_inserter(RPOT));
  rpo_iterator I = RPOT.rbegin(), E = RPOT.rend();
  assert(*I == GT::getEntryNode(DFG) &&
          "The first node in the topological order differs from the entry node in the data-flow framework!");
  for (++I; I != E; ++I) {
    DFT::initialize(*I, DFF);
    DFT::setValue(DFT::topElement(DFF), *I);
  }
  DFT::initialize(GT::getEntryNode(DFG), DFF);
  DFT::setValue(DFT::boundaryCondition(DFF), GT::getEntryNode(DFG));
  for (I = RPOT.rbegin(), ++I; I != E; ++I) {
    assert(GT::child_begin(*I) != GT::child_end(*I) &&
           "Data-flow graph must not contain unreachable nodes!");
    ValueType Value(DFT::topElement(DFF));
    for (ChildIteratorType CI = GT::child_begin(*I), CE = GT::child_end(*I);
         CI != CE; ++CI) {
      DFT::meetOperator(DFT::getValue(*CI), Value, DFF);
    }
    DFT::transferFunction(Value, *I, DFF);
  }
}

/// \brief Solves data-flow problem in topological order during one iteration.
///
/// This computes IN and OUT for each node in the specified data-flow graph
/// in a topological order, so a graph traversal is executed only two times.
/// Firstly to calculate order of nodes and secondly to solve data-flow problem.
/// The last computed value for each node can be optained by calling
/// the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFF Data-flow framework, it can not be null.
/// \attention The DataFlowTraits class should be specialized by DFFwk.
/// Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by
/// DataFlowTraits<DFFwk>::GraphType.
template<class DFFwk> void solveDataFlowTopologicaly(DFFwk DFF) {
  typedef DataFlowTraits<DFFwk> DFT;
  solveDataFlowTopologicaly(DFF, DFT::getDFG(DFF));
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
/// - static void collapse(GraphType &G, DFFwk &DFF) -
///     Collapses a data-flow graph which represents a region to a one node
///     in a data-flow graph of an outer region.
/// - typedef region_iterator,
///   static region_iterator region_begin(GraphType &G),
///   static region_iterator region_end (GraphType &G) -
///     Allow iteration over all internal regions in the specified region.
/// \note It may be convinient to inherit DataFlowTraits to specialize this 
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
/// a data-flow graph associated with this region. If it is possible the
/// problem will be solved in topological order in a single pass, otherwise
/// iteratively.
/// \param [in, out] DFF Data-flow framework, it can not be null. The getDFG()
/// method must return a graph which is associated with the outermost region.
/// \param [in, out] DFG Data-flow graph sepcified in the data-flow framework.
/// Subgraph of this graph also can be used.
/// \attention DataFlowTraits and RegionDFTraits classes should be specialized
/// by DFFwk. Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by type of each
/// regions in the hierarhy (not only for DataFlowTraits<DFFwk>::GraphType).
/// Note that type of region is generally a pointer type.
template<class DFFwk> void solveDataFlowUpward(DFFwk DFF,
    typename DataFlowTraits<DFFwk>::GraphType DFG) {
  typedef RegionDFTraits<DFFwk> RT;
  typedef typename RT::region_iterator region_iterator;
  for (region_iterator I = RT::region_begin(DFG), E = RT::region_end(DFG);
       I != E; ++I)
    solveDataFlowUpward(DFF, *I);
  if (isDAG(DFG))
    solveDataFlowTopologicaly(DFF, DFG);
  else
    solveDataFlowIteratively(DFF, DFG);
  RT::collapse(DFG, DFF);
}

/// \brief Solves data-flow problem for the specified hierarchy of regions.
///
/// The data-flow problems solves upward from innermost regions to the region
/// associated with the specified data-flow graph. Before solving the data-flow
/// problem of some region all inner regions will be collapsed to a one node in
/// a data-flow graph associated with this region. If it is possible the
/// problem will be solved in topological order in a single pass, otherwise
/// iteratively.
/// \param [in, out] DFF Data-flow framework, it can not be null. The getDFG()
/// method must return a graph which is associated with the outermost region
/// \attention DataFlowTraits and RegionDFTraits classes should be specialized
/// by DFFwk. Note that DFFwk is generally a pointer type.
/// The GraphTraits class should be specialized by type of each
/// regions in the hierarhy (not only for DataFlowTraits<DFFwk>::GraphType).
/// Note that type of region is generally a pointer type.
template<class DFFwk> void solveDataFlowUpward(DFFwk DFF) {
  typedef DataFlowTraits<DFFwk> DFT;
  solveDataFlowUpward(DFF, DFT::getDFG(DFF));
}

/// \brief Instances of this class are used to represent a node
/// with a small list of the adjacent nodes.
///
/// This is a node, optimized for the case when a number of adjacent nodes
/// is small. It contains some number of adjacent nodes in-place, 
/// which allows it to avoid heap allocation when the actual number of
/// nodes is below that threshold (2*N). This allows normal "small" cases to be
/// fast without losing generality for large inputs.
template<class NodeTy, unsigned N>
class SmallDFNode : private Utility::Uncopyable {
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

  /// Type used to iterate over predecessors.
  typedef typename llvm::SmallVectorImpl<NodeTy *>::const_iterator pred_iterator;

  /// Returns iterator that points to the beginning of the successor list.
  succ_iterator succ_begin() const { return mAdjacentNodes[SUCC].begin(); }

  /// Returns iterator that points to the ending of the successor list.
  succ_iterator succ_end() const { return mAdjacentNodes[SUCC].end(); }

  /// Returns iterator that points to the beginning of the predcessor list.
  pred_iterator pred_begin() const { return mAdjacentNodes[PRED].begin(); }

  /// Returns iterator that points to the ending of the predcessor list.
  pred_iterator pred_end() const { return mAdjacentNodes[PRED].end(); }

  /// Returns true if the specified node is a successor of this node.
  bool isSuccessor(NodeTy *Node) const {
    assert(Node && "Data-flow node must not be null!");
    for (NodeTy * N : mAdjacentNodes[SUCC])
      if (N == Node) return true;
    return false;
  }

  /// Returns true if the specified node is a predcessor of this node.
  bool isPredecessor(NodeTy *Node) const {
    assert(Node && "Data-flow node must not be null!");
    for (NodeTy * N : mAdjacentNodes[PRED])
      if (N == Node) return true;
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

private:
  llvm::SmallVector<NodeTy *, N> mAdjacentNodes[NUMBER_DIRECTION];
};
}

#endif//TSAR_DATA_FLOW_H
