//===------- tsar_df_loop.h - Loop Nest Data-Flow Solver  -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines functions and classes to solve a data-flow problem for
// a loop nest. The nest is treated as a hierarchy of ranges where each range
// is an abstraction of an inner loop. The data-flow problem will be solved for
// each range, but only natural loops can be examined.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LOOP_BODY_H
#define TSAR_LOOP_BODY_H

#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/Analysis/LoopInfo.h>
#include <vector>
#include <utility.h>
#include "tsar_data_flow.h"
#include "tsar_graph.h"
#include "declaration.h"

namespace tsar {
/// \brief Solves data-flow problem for the specified loop nest.
///
/// This function treats a loop nest as hierarchy of ranges and for each range
/// solves data-flow problem. Each range is an abstraction of an inner loop.
/// The specified data-flow graph (DFG) is an abstraction of the outermost loop
/// in the nest. The data-flow problems solves upward from innermost loops
/// to the loop associated with the specified data-flow graph. Before solving
/// the data-flow problem of some loop all inner loops will be collapsed
/// to a one node in a data-flow graph associated with this loop.
/// If it is possible the problem will be solved in topological order
/// in a single pass, otherwise iteratively.
/// Only natural loops will be treated as a range other loops will be ignored.
/// \param [in, out] DFG Data-flow graph associated with the outermost loop
/// in the nest, it can not be null.
/// \pre The LoopDFTraits and GraphTraits should be specialized by GraphType.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
template<class GraphType> void solveLoopDataFlow(GraphType DFG) {
  typedef LoopDFTraits<GraphType> DFT;
  typedef typename DFT::NodeType NodeType;
  DenseMap<BasicBlock *, NodeType *> Blocks;
  Loop *L = DFT::getLoop(DFG);
  for (Loop::iterator I = L->begin(), E = L->end(); I != E; ++I) {
    std::pair<GraphType, NodeType *> N = DFT::addNode(*I, DFG);
    solveLoopDataFlow(N.first);
    for (BasicBlock *BB : (*I)->getBlocks())
      Blocks.insert(std::make_pair(BB, N.second));
  }
  for (BasicBlock *BB : L->getBlocks()) {
    if (Blocks.count(BB))
      continue;
    NodeType *N = DFT::addNode(BB, DFG);
    Blocks.insert(std::make_pair(BB, N));
  }
  assert(L->getHeader() && Blocks.count(L->getHeader()) &&
         "Data-flow node for the loop header is not found!");
  NodeType *EntryNode = DFT::addEntryNode(DFG);
  NodeType *HeaderNode = Blocks.find(L->getHeader())->second;
  DFT::addSuccessor(HeaderNode, EntryNode);
  DFT::addPredecessor(EntryNode, HeaderNode);
  for (auto BBToN : Blocks) {
    for (succ_iterator SI = succ_begin(BBToN.first),
         SE = succ_end(BBToN.first); SI != SE; ++SI) {
      auto SToNode = Blocks.find(*SI);
      // First, exiting nodes will be specified.
      // Second, latch nodes will be specified. A latch node is a node
      // that contains a branch back to the header.
      // Third, successors will be specified:
      // 1. Back and exit edges will be ignored.
      // 2. Branches inside inner loops will be ignored.
      // There is branch from a data-flow node to itself
      // (SToNode->second == BBToN.second) only if this node is an abstraction
      // of an inner loop. So this branch is inside this inner loop
      // and should be ignored.
      if (SToNode == Blocks.end())
        DFT::setExitingNode(BBToN.second, DFG);
      else if (*SI == L->getHeader())
        DFT::setLatchNode(BBToN.second, DFG);
      else if (SToNode->second != BBToN.second)
        DFT::addSuccessor(SToNode->second, BBToN.second);
    }
    // Predecessors outsied the loop will be ignored.
    if (BBToN.first != L->getHeader()) {
      for (pred_iterator PI = pred_begin(BBToN.first),
           PE = pred_end(BBToN.first); PI != PE; ++PI) {
        assert(Blocks.count(*PI) &&
               "Data-flow node for the specified basic block is not found!");
        NodeType *PN = Blocks.find(*PI)->second;
        // Branches inside inner loop will be ignored (for details, see above).
        if (PN != BBToN.second)
          DFT::addPredecessor(PN, BBToN.second);
      }
    }
  }
  if (isDAG(DFG))
    solveDataFlowTopologicaly(DFG);
  else
    solveDataFlowIteratively(DFG);
  DFT::collapseLoop(DFG);
}

/// \brief Data-flow framework for loop nest.
///
/// This class should be specialized by different graph types
/// which is why the default version is empty.
/// The specialization is used to solve forward or backward data-flow problems
/// a nest of loops. The nest is treated as a hierarchy of ranges where each
/// range is an abstraction of an inner loop.
/// Note that GraphType is generally a pointer type, for example BasicBlock *.
/// \par There are two kinds of necessary elements to provide.
/// At first this is elements as for DataFlowTraits.
/// The following elements should be provided additionally:
/// - static llvm::Loop *getLoop(GraphType DFG) -
///     Returnes a loop associated with the specified graph.
/// - static NodeType * addEntryNode(GraphType DFG) -
///     Add an entry node of the data-flow graph.
/// - static std::pair<GraphType , NodeType *>
///   addNode(llvm::Loop *L, GraphType DFG) -
///     Creates an abstraction of the specified inner loop. This abstraction can
///     be treated as a new node of the specified graph and also as a data-flow
///     graph that should be analyzed.
/// - static NodeType *addNode(llvm::BasicBlock *BB, GraphType DFG) -
///     Creates an abstraction of the specified basic block.
/// - static void addSuccessor(NodeType *N, NodeType *Succ) -
///     Adds successor to the specified node.
/// - static void addPredecessor(NodeType *N, NodeType *Succ) -
///     Adds predecessor to the specified node.
/// - static void setExitingNode(NodeType *N, GrpahType DFG) -
///     Specifies one of exiting nodes of the data-flow graph.
///     Multiple nodes can be specified.
/// - static void setLatchNode(NodeType *N, GrpahType DFG) -
///     Specifies one of latch nodes of the data-flow graph.
///     A latch node is a node that contains a branch back to the header.
///     Multiple nodes can be specified.
/// \note It may be convinient to inherit DataFlowTraits to specialize this 
/// class.
template<class GraphType> struct LoopDFTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename GraphType::UnknownGraphTypeError NodeType;
};

/// \brief Representation of a block in a data-flow framework.
///
/// Instance of this class is used to represent abstraction of a block
/// in data-flow framework. This class should be used only to solve
/// data-flow problem.
/// \tparam BlockTy Simple block which is contained in a flow graph.
/// \tparam NodeTy Representation of the block.
///
/// \attention If there are different nodes in a data-flow graph
/// (for example basic blocks and loop abstractions) the NodeTy class
/// should be the same for this nodes.
template<class BlockTy, class NodeTy>
class BlockDFBase : private Utility::Uncopyable {
public:
  /// \brief Ctreates representation of the block.
  ///
  /// \pre The block argumetn can not take a null value.
  explicit BlockDFBase(BlockTy *B) : mBlock(B) {
    assert(B && "Block must not be null!");
  }

  /// Get the block.
  BlockTy *getBlock() const { return mBlock; }

private:
  BlockTy *mBlock;
};

/// Representation of an entry node in a data-flow framework.
template<class NodeTy>
class EntryDFBase : private Utility::Uncopyable {};

/// \brief Representation of a loop in a data-flow framework.
///
/// Instance of this class is used to represent abstraction of a loop
/// in data-flow framework. This class should be used only to solve
/// data-flow problem. The loop can be collapsed to one abstract node
/// to simplify the data-flow graph that contains this loop. If this loop
/// has inner loops they also can be collapsed. So the abstraction of this loop
/// can internally contain nodes of following types:
///   * simple block (BlockTy represented as NodeTy)
///   * collapsed inner loop (abstraction of an inner loop represened as NodeTy)
///
/// \tparam BlockTy Simple block which is contained in the loop.
/// \tparam LoopTy Loop represented by this node.
/// \tparam NodeTy Representation of nodes of the loop.
/// In the simplest case it is a wrapper for basic blocks. It also can be
/// an abstraction of inner loops which are collapsed to one node.
///
/// \attention If there are different nodes in a data-flow graph
/// (for example basic blocks and loop abstractions) the NodeTy class
/// should be the same for this nodes.
template<class BlockTy, class LoopTy, class NodeTy>
class LoopDFBase : private Utility::Uncopyable {
public:
  /// This type used to iterate over all nodes in the loop body.
  typedef typename std::vector<NodeTy *>::const_iterator nodes_iterator;

  /// This type used to iterate over all exiting nodes in the loop body.
  typedef typename llvm::SmallPtrSet<NodeTy *, 8>::const_iterator
    exiting_iterator;

  /// This type used to iterate over all latch nodes in the loop body.
  typedef typename llvm::SmallPtrSet<NodeTy *, 8>::const_iterator
    latch_iterator;

  /// \brief Creates representation of the loop.
  ///
  /// \pre The loop argument can not take a null value.
  explicit LoopDFBase(LoopTy *L) : mLoop(L), mEntry(NULL) {
    assert(L && "Loop must not be null!");
  }

  /// Get the loop.
  LoopTy *getLoop() const { return mLoop; }

  /// \brief Get the entry-point of the data-flow graph.
  ///
  /// The entry node is not a header of the loop, this node is a predecessor
  /// of the header. This node is not contained in a list of nodes which is
  /// a result of the getNodes() method. 
  NodeTy *getEntryNode() const {
    assert(mEntry && "There is no entry node in the graph!");
    return mEntry;
  }

  /// Get the number of nodes in this loop.
  unsigned getNumNodes() const { return mNodes.size(); }

  /// Get a list of the nodes which make up this loop body.
  const std::vector<NodeTy*> & getNodes() const { return mNodes; }

  /// Returns iterator that points to the beginning of the basic block list.
  nodes_iterator nodes_begin() const { return mNodes.begin(); }

  /// Returns iterator that points to the ending of the basic block list.
  nodes_iterator nodes_end() const { return mNodes.end(); }

  /// \brief Specifies an exiting node of the data-flow graph.
  ///
  /// Multiple nodes can be specified.
  void setExitingNode(NodeTy *N) {
    assert(N && "Node must not be null!");
    mExitingNodes.insert(N);
  }

  /// Get a list of the exiting nodes of this loop.
  const llvm::SmallPtrSet<NodeTy *, 8> & getExitingNodes() const { 
    return mExitingNodes;
  }

  /// Returns iterator that points to the beginning of the exiting nodes list.
  exiting_iterator exiting_begin() const { return mExitingNodes.begin(); }

  /// Returns iterator that points to the ending of the exiting nodes list.
  exiting_iterator exiting_end() const { return mExitingNodes.end(); }

  ///\brief  Returns true if the node is an exiting node of this loop.
  ///
  /// Exiting node is a node which is inside of the loop and 
  /// have successors outside of the loop.
  bool isLoopExiting(const NodeTy *N) { return mExitingNodes.count(N); }

  /// \brief Specifies an latch node of the data-flow graph.
  ///
  /// Multiple nodes can be specified.
  void setLatchNode(NodeTy *N) {
    assert(N && "Node must not be null!");
    mLatchNodes.insert(N);
  }

  /// Get a list of the latch nodes of this loop.
  const llvm::SmallPtrSet<NodeTy *, 8> & getLatchNodes() const {
    return mLatchNodes;
  }

  /// Returns iterator that points to the beginning of the latch nodes list.
  latch_iterator latch_begin() const { return mLatchNodes.begin(); }

  /// Returns iterator that points to the ending of the latch nodes list.
  latch_iterator latch_end() const { return mLatchNodes.end(); }

  ///\brief  Returns true if the node is an latch node of this loop.
  ///
  /// A latch node is a node that contains a branch back to the header.
  bool isLoopLatch(const NodeTy *N) { return mLatchNodes.count(N); }

protected:
  /// \brief Insert a new node at the end of the list of nodes.
  ///
  /// \pre A new node can not take a null value.
  void addNode(NodeTy *N) {
    assert(N && "Node must not be null!");
    mNodes.push_back(N); 
  }

  /// \brief Add an entry node of the data-flow graph.
  ///
  /// \pre
  /// - The node should be differ from other nodes of the graph.
  /// This node is not contained in a list of nodes which is a result
  /// of the getNodes() method.
  /// - A new node can not take a null value.
  void addEntryNode(NodeTy *N) {
    assert(N && "Node must not be null!");
#ifdef DEBUG
    for (NodeTy *Node : mNodes)
      assert(N != Node && "The entry node must not be contained in the result of getNodes() method!");
#endif
    mEntry = N;
  }

private:
  std::vector<NodeTy*> mNodes;
  llvm::SmallPtrSet<NodeTy *, 8> mExitingNodes;
  llvm::SmallPtrSet<NodeTy *, 8> mLatchNodes;
  LoopTy *mLoop;
  NodeTy *mEntry;
};
}
#endif//TSAR_LOOP_BODY_H
