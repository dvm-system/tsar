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
/// This class should be specialized by different graph types
/// which is why the default version is empty. The specialization is used
/// to solve forward or backward data-flow problems. A direction of data-flow 
/// should be explicitly set via definitions of functions that allow iteration
/// over all children of the specified node.
/// \note It may be convinient to specialize this class in two steps.
/// First you need to specialize llvm::GraphTraits class from
/// llvm/ADT/GraphTraits.h. Secondly you need to inherit it and determine the
/// remaining functions.
///
/// The following elements can be provided by specialization of GraphTratis:
/// - typedef NodeType - Type of node in the graph.
/// - typedef ChildIteratorType - Type used to iterate over children in graph.
/// - static NodeType *getEntryNode(GraphType &) -
///     Returns the entry node of the graph.
/// - static ChildIteratorType child_begin(NodeType *),
///   static ChildIteratorType child_end(NodeType *) -
///     Return iterators that point to the beginning and ending of the child
///   node list for the specified node.
/// - typedef nodes_iterator,
///   static nodes_iterator nodes_begin(GraphType &G),
///   static nodes_iterator nodes_end (GraphType &G) -
///     Allow iteration over all nodes in the graph.
///   static unsigned size (GraphType &G) -
///     Returns total number of nodes in the graph.
///
/// The following elements should be additionaly provided:
/// - typedef ValueType - Type of data-flow value.
/// - static ValueType topElement(GraphType &) -
///     Returns top element for the data-flow framework.
/// - static ValueType boundaryCondition(GraphType &) -
///     Returns boundary condition for the data-flow framework.
/// - static void setValue(ValueType, NodeType *),
///   static ValueType getValue(NodeType *) -
///     Allow to access data-flow value for the specified node.
/// - static meetOperator(const ValueType &, ValueType &) -
///     Evaluates a meet operator, the result is stored in the second
///     parameter.
/// - satic bool transferFunction(ValueType, NodeType *)
///     Evaluates a transfer function for the specified node. This returns
///     true if produced data-flow value differs from the data-flow value
///     produced on previouse iteration of the data-flow analysis algorithm.
///     For the first iteration the new value compares with the initial one.
///
/// Direction of the data-flow is specified by child_begin and child_end
/// functions.
template <class GraphType> struct DataFlowTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename GraphType::UnknownGraphTypeError NodeType;
};
  
/// \brief Iteratively solves data-flow problem.
///
/// This computes IN and OUT for each node in the specified data-flow graph
/// by successive approximation. The last computed value for each node
/// can be optained by calling the DataFlowTraits::getValue() function.
/// The type of computed value (IN or OUT) depends on a data-flow direction
/// (see DataFlowTratis). In case of a forward direction it is OUT,
/// otherwise IN.
/// \param [in, out] DFG Data-flow graph, it can not be null.
/// The DataFlowTraits class should be specialized by GraphType.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
template<class GraphType> void solveDataFlowIteratively(GraphType DFG) {
  typedef DataFlowTraits<GraphType> DFT;
  typedef typename DFT::nodes_iterator nodes_iterator;
  typedef typename DFT::ChildIteratorType ChildIteratorType;
  typedef typename DFT::ValueType ValueType;
  assert(DFG && "Data-flow graph must not be null!");
  for (nodes_iterator I = DFT::nodes_begin(DFG), E = DFT::nodes_end(DFG);
       I != E; ++I) {
    DFT::setValue(DFT::topElement(DFG), *I);
  }
  DFT::setValue(DFT::boundaryCondition(DFG), DFT::getEntryNode(DFG));
  bool isChanged = true;
  do {
    isChanged = false;
    for (nodes_iterator I = DFT::nodes_begin(DFG), E = DFT::nodes_end(DFG);
         I != E; ++I) {
      assert(DFT::child_begin(*I) != DFT::child_end(*I) &&
             "Data-flow graph must not contain unreachable nodes!");
      ValueType Value(DFT::topElement(DFG));
      for (ChildIteratorType CI = DFT::child_begin(*I), CE = DFT::child_end(*I);
            CI != CE; ++CI) {
        DFT::meetOperator(DFT::getValue(*CI), Value);
      }
      isChanged = DFT::transferFunction(std::move(Value), *I) || isChanged;
    }
  } while (isChanged);
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
/// \param [in, out] DFG Data-flow graph, it can not be null.
/// \pre The DataFlowTraits class should be specialized by GraphType.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
template<class GraphType> void solveDataFlowTopologicaly(GraphType DFG) {
  typedef DataFlowTraits<GraphType> DFT;
  typedef typename DFT::nodes_iterator nodes_iterator;
  typedef typename DFT::ChildIteratorType ChildIteratorType;
  typedef typename DFT::NodeType NodeType;
  typedef typename DFT::ValueType ValueType;
  typedef llvm::po_iterator<
    GraphType, llvm::SmallPtrSet<NodeType *, 8>, false, 
    llvm::GraphTraits<llvm::Inverse<GraphType> > > po_iterator;
  typedef std::vector<NodeType *> RPOTraversal;
  typedef typename RPOTraversal::reverse_iterator rpo_iterator;
  assert(DFG && "Data-flow graph must not be null!");
  // We do not use llvm::ReversePostOrderTraversal class because
  // its implementation requires that GraphTraits is specialized by NodeType *.
  RPOTraversal RPOT;
  std::copy(po_iterator::begin(DFG), po_iterator::end(DFG),
            std::back_inserter(RPOT));
  rpo_iterator I = RPOT.rbegin(), E = RPOT.rend();
  assert(*I == DFT::getEntryNode(DFG) &&
          "The first node in the topological order differs from the entry node in the data-flow framework!");
  for (++I; I != E; ++I)
    DFT::setValue(DFT::topElement(DFG), *I);
  DFT::setValue(DFT::boundaryCondition(DFG), DFT::getEntryNode(DFG));
  for (I = RPOT.rbegin(), ++I; I != E; ++I) {
    assert(DFT::child_begin(*I) != DFT::child_end(*I) &&
           "Data-flow graph must not contain unreachable nodes!");
    ValueType Value(DFT::topElement(DFG));
    for (ChildIteratorType CI = DFT::child_begin(*I), CE = DFT::child_end(*I);
         CI != CE; ++CI) {
      DFT::meetOperator(DFT::getValue(*CI), Value);
    }
    DFT::transferFunction(Value, *I);
  }
}

/// \brief Data-flow framework for a hierarchy of regions.
///
/// This class should be specialized by different graph types
/// which is why the default version is empty.
/// The specialization is used to solve forward or backward data-flow problems
/// for a hierarchy of regions. GraphType represents a data-flow graph for a
/// region. Nodes of this graph are simple nodes or internal regions which are
/// also represented by other data-flow graphs. Note that GraphType is
/// generally a pointer type, for example BasicBlock *.
/// \par There are two kinds of necessary elements to provide.
/// At first this is elements as for DataFlowTraits.
/// The following elements should be provided additionally:
/// - static void collapse(NodeType *DFG) -
///     Collapses a data-flow graph which represents a region to a one node
///     in a data-flow graph of an outer region.
/// - typedef regions_iterator,
///   static regions_iterator regions_begin(GraphType &G),
///   static regions_iterator regions_end (GraphType &G) -
///     Allow iteration over all internal regions in the graph.
/// \note It may be convinient to inherit DataFlowTraits to specialize this 
/// class.
template<class GraphType> struct RegionDFTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename GraphType::UnknownGraphTypeError NodeType;
};

/// \brief Solves data-flow problem for the specified hierarchy of regions.
///
/// The data-flow problems solves upward from innermost regions to the region
/// associated with the specified data-flow graph. Before solving the data-flow
/// problem of some region all inner regions will be collapsed to a one node in
/// a data-flow graph associated with this region. If it is possible the
/// problem will be solved in topological order in a single pass, otherwise
/// iteratively.
/// \param [in, out] DFG Data-flow graph associated with the outermost region,
/// it can not be null.
/// \pre The RegionDFTraits and GraphTraits should be specialized by each type
/// of region in the hierarhy (not only for GraphType).
/// Note that type of region is generally a pointer type,
/// for example BasicBlock *.
template<class GraphType> void solveDataFlowUpward(GraphType DFG) {
  typedef RegionDFTraits<GraphType> DFT;
  typedef typename DFT::regions_iterator regions_iterator;
  for (regions_iterator I = DFT::regions_begin(DFG), E = DFT::regions_end(DFG);
  I != E; ++I)
    solveDataFlowUpward(*I);
  if (isDAG(DFG))
    solveDataFlowTopologicaly(DFG);
  else
    solveDataFlowIteratively(DFG);
  DFT::collapse(DFG);
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
