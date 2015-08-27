//===------- tsar_df_node.h - Represent a data-flow graph ------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===--------------------------------------------------------------------===//
//
// This file implements functions to build a data-flow graph which is
// convinient to solve data-flow problem.
//
//===--------------------------------------------------------------------===//

#include <llvm/IR/BasicBlock.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/DenseMap.h>
#include "tsar_df_graph.h"

using namespace llvm;

namespace tsar {
DFLoop * buildLoopRegion(llvm::Loop *L) {
  DFLoop *DFL = new DFLoop(L);
  DenseMap<BasicBlock *, DFNode *> Blocks;
  for (Loop::iterator I = L->begin(), E = L->end(); I != E; ++I) {
    DFLoop *N = buildLoopRegion(*I);
    DFL->addNode(N);
    for (BasicBlock *BB : (*I)->getBlocks())
      Blocks.insert(std::make_pair(BB, N));
  }
  for (BasicBlock *BB : L->getBlocks()) {
    if (Blocks.count(BB))
      continue;
    DFBlock * N = new DFBlock(BB);
    DFL->addNode(N);
    Blocks.insert(std::make_pair(BB, N));
  }
  assert(L->getHeader() && Blocks.count(L->getHeader()) &&
    "Data-flow node for the loop header is not found!");
  DFEntry *EntryNode = new DFEntry;
  DFL->addNode(EntryNode);
  DFNode *HeaderNode = Blocks.find(L->getHeader())->second;
  EntryNode->addSuccessor(HeaderNode);
  HeaderNode->addPredecessor(EntryNode);
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
        DFL->setExitingNode(BBToN.second);
      else if (*SI == L->getHeader())
        DFL->setLatchNode(BBToN.second);
      else if (SToNode->second != BBToN.second)
        BBToN.second->addSuccessor(SToNode->second);
    }
    // Predecessors outsied the loop will be ignored.
    if (BBToN.first != L->getHeader()) {
      for (pred_iterator PI = pred_begin(BBToN.first),
        PE = pred_end(BBToN.first); PI != PE; ++PI) {
        assert(Blocks.count(*PI) &&
          "Data-flow node for the specified basic block is not found!");
        DFNode *PN = Blocks.find(*PI)->second;
        // Branches inside inner loop will be ignored (for details, see above).
        if (PN != BBToN.second)
          BBToN.second->addPredecessor(PN);
      }
    }
  }
  return DFL;
}
}
