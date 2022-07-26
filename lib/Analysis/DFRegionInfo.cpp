//===- DFRegionInfo.h ----- Data-flow Regions Analysis ----------*- C++ -*-===//
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
// This implements building of a data-flow regions hierarchy.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/LoopTraits.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/InitializePasses.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "df-regions"

STATISTIC(NumRegion, "Number of regions");
STATISTIC(NumBlockRegion, "Number of simple block regions");
STATISTIC(NumLoopRegion, "Number of loop regions");
STATISTIC(NumFunctionRegion, "Number of function regions");

char DFRegionInfoPass::ID = 0;
INITIALIZE_PASS_BEGIN(DFRegionInfoPass, "df-regions",
  "Data-Flow Region Information", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(DFRegionInfoPass, "df-regions",
  "Data-Flow Region Information", true, true)

tsar::DFNode * DFRegionInfo::getRegionFor(llvm::Loop *L) const {
  typedef tsar::LoopTraits<llvm::Loop *> LT;
  auto DFN = getRegionFor(LT::getHeader(L));
  while (DFN && !isa<DFLoop>(DFN))
    DFN = DFN->getParent();
  return DFN;
}

void DFRegionInfo::recalculate(llvm::Function &F, llvm::LoopInfo &LpInfo) {
  releaseMemory();
  mTopLevelRegion = new tsar::DFFunction(&F);
  buildLoopRegion(std::make_pair(&F, &LpInfo),
    llvm::cast<tsar::DFRegion>(mTopLevelRegion));
  NumRegion = ++NumFunctionRegion + NumLoopRegion + NumBlockRegion;
}

void DFRegionInfo::recalculate(llvm::Loop &L) {
  releaseMemory();
  mTopLevelRegion = new tsar::DFLoop(&L);
  buildLoopRegion(&L, llvm::cast<tsar::DFRegion>(mTopLevelRegion));
  NumRegion = ++NumLoopRegion + NumBlockRegion;
}

template<class LoopReptn>
void DFRegionInfo::buildLoopRegion(LoopReptn L, DFRegion *R) {
  assert(R && "Region must not be null!");
  // To improve efficiency of construction the first added node
  // is entry and the last is exit (for loops the last added node is latch).
  auto *EntryNode = new DFEntry;
  auto *ExitNode = new DFExit;
  R->addNode(EntryNode);
  typedef LoopTraits<LoopReptn> LT;
  llvm::DenseMap<llvm::BasicBlock *, DFNode *> Blocks;
  for (auto I = LT::loop_begin(L), E = LT::loop_end(L); I != E; ++I) {
    auto *DFL = new DFLoop(*I);
    ++NumLoopRegion;
    buildLoopRegion(*I, DFL);
    R->addNode(DFL);
    for (llvm::BasicBlock *BB : (*I)->getBlocks())
      Blocks.insert(std::make_pair(BB, DFL));
  }
  for (auto I = LT::block_begin(L), E = LT::block_end(L); I != E; ++I) {
    if (Blocks.count(*I))
      continue;
    auto *N = new DFBlock(*I);
    ++NumBlockRegion;
    R->addNode(N);
    Blocks.insert(std::make_pair(*I, N));
    mBBToNode.insert(std::make_pair(*I, N));
  }
  R->addNode(ExitNode);
  assert(LT::getHeader(L) && Blocks.count(LT::getHeader(L)) &&
    "Data-flow node for the loop header is not found!");
  DFNode *HeaderNode = Blocks.find(LT::getHeader(L))->second;
  EntryNode->addSuccessor(HeaderNode);
  HeaderNode->addPredecessor(EntryNode);
  DFLatch *LatchNode = nullptr;
  for (auto BBToN : Blocks) {
    if (succ_begin(BBToN.first) == succ_end(BBToN.first)) {
      BBToN.second->addSuccessor(ExitNode);
      ExitNode->addPredecessor(BBToN.second);
    } else {
      for (llvm::succ_iterator SI = succ_begin(BBToN.first),
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
        if (SToNode == Blocks.end()) {
          BBToN.second->addSuccessor(ExitNode);
          ExitNode->addPredecessor(BBToN.second);
        } else if (*SI == LT::getHeader(L)) {
          if (!LatchNode) {
            LatchNode = new DFLatch;
            R->addNode(LatchNode);
          }
          BBToN.second->addSuccessor(LatchNode);
          LatchNode->addPredecessor(BBToN.second);
        } else if (SToNode->second != BBToN.second)
          BBToN.second->addSuccessor(SToNode->second);
      }
    }
    // Predecessors outside the loop will be ignored.
    if (BBToN.first != LT::getHeader(L)) {
      for (llvm::pred_iterator PI = pred_begin(BBToN.first),
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
  // If there are no explicit exits from this loop, let us build an arc from
  // the entry node to the exit node to remove unreachable nodes.
  if (ExitNode->pred_begin() == ExitNode->pred_end()) {
    EntryNode->addSuccessor(ExitNode);
    ExitNode->addPredecessor(EntryNode);
  }
  for (auto I = R->region_begin(), E = R->region_end(); I != E; ++I) {
    // If there are no explicit exits from an internal loop (it may be an
    // infinite loop), let us build an arc from a region associated with this
    // loop to an exit node of currently evaluated region to remove unreachable
    // nodes.
    if ((*I)->numberOfSuccessors() == 0) {
      (*I)->addSuccessor(ExitNode);
      ExitNode->addPredecessor(*I);
    }
  }
}

bool DFRegionInfoPass::runOnFunction(Function &F) {
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  mRegionInfo.recalculate(F, LpInfo);
  return false;
}

void DFRegionInfoPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createDFRegionInfoPass() {
  return new DFRegionInfoPass();
}