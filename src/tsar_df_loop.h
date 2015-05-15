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
  DFT::setEntry(Blocks.find(L->getHeader())->second, DFG);
  for (auto BBToN : Blocks) {
    for (succ_iterator SI = succ_begin(BBToN.first),
         SE = succ_end(BBToN.first); SI != SE; ++SI) {
      auto SToNode = Blocks.find(*SI);
      // Back and exit edges will be ignored.
      // Branches inside inner loops will be ignored.
      // There is branch from a data-flow node to itself
      // (SToNode->second == BBToN.second) only if this node is an abstraction
      // of an inner loop. So this branch is inside this inner loop
      // and should be ignored.
      if (*SI != L->getHeader() && SToNode != Blocks.end() &&
          SToNode->second != BBToN.second)
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
/// - static void setEntry(GraphType DFG, NodeType *N) -
///     Specifies an entry node of the data-flow graph.
/// - static std::pair<GraphType , NodeType *>
///   addNode(GraphType DFG, llvm::Loop *L) -
///     Creates an abstraction of the specified inner loop. This abstraction can
///     be treated as a new node of the specified graph and also as a data-flow
///     graph that should be analyzed.
/// - static NodeType *addNode(GraphType DFG, llvm::BasicBlock *BB) -
///     Creates an abstraction of the specified basic block.
/// - static void addSuccessor(NodeType *N, NodeType *Succ) -
///     Adds successor to the specified node.
/// - static void addPredecessor(NodeType *N, NodeType *Succ) -
///     Adds predecessor to the specified node.
/// \note It may be convinient to inherit DataFlowTraits to specialize this 
/// class.
template<class GraphType> struct LoopDFTraits :
public DataFlowTraits<GraphType> {
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
  /// This type used to iterate over all basic blocks in the loop body.
  typedef typename std::vector<NodeTy*>::const_iterator nodes_iterator;

  /// \brief Creates representation of the loop.
  ///
  /// \pre The loop argument can not take a null value.
  explicit LoopDFBase(LoopTy *L) : mLoop(L) {
    assert(L && "Loop must not be null!");
  }

  /// Get the loop.
  LoopTy *getLoop() const { return mLoop; }

  /// Specifies an entry node of the data-flow graph.
  void setEntry(NodeTy *N) {
    assert(N && "Node must not be null!");
    mEntry = N;
  }

  /// Get the entry-point of the loop, which is called the header.
  NodeTy *getEntry() const {
    assert(getNumNodes() && "There is no nodes in the loop!");
    return mNodes.front();
  }

  /// Get the number of nodes in this loop.
  unsigned getNumNodes() const { return mNodes.size(); }

  /// Get a list of the nodes which make up this loop body.
  const std::vector<NodeTy*> &getNodes() const { return mNodes; }

  /// Returns iterator that points to the beginning of the basic block list.
  nodes_iterator nodes_begin() const { return mNodes.begin(); }

  /// Returns iterator that points to the ending of the basic block list.
  nodes_iterator nodes_end() const { return mNodes.end(); }

protected:
  /// \brief Insert a new node at the end of the list of nodes.
  ///
  /// \pre A new node can not take a null value.
  void addNode(NodeTy *N) {
    assert(N && "Node must not be null!");
    mNodes.push_back(N); 
  }

private:
  std::vector<NodeTy*> mNodes;
  LoopTy *mLoop;
  NodeTy *mEntry;
};
}
#endif//TSAR_LOOP_BODY_H
