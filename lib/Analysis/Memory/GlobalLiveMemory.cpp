//===--- GlobalLiveMemory.cpp --- Global Live Memory Analysis ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file implements passes to determine global live memory locations.
//
//===----------------------------------------------------------------------===//
#include <tsar/Analysis/Memory/GlobalLiveMemory.h>
#include <tsar/Support/PassProvider.h>
#include <tsar/Analysis/Memory/LiveMemory.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/Function.h>

#include <Vector>
#include <map>

#ifdef LLVM_DEBUG
# include <llvm/IR/Dominators.h>
#endif

#undef DEBUG_TYPE
#define DEBUG_TYPE "live-mem"


using namespace llvm;
using namespace tsar;

typedef DenseMap<
  Function*, 
  std::vector<
    std::pair<
      CallInst*, 
      std::unique_ptr<LiveSet>
    >
  >
> FuncToCallInstLiveSet;

typedef FunctionPassProvider <
  DFRegionInfoPass,
  DefinedMemoryPass,
  DominatorTreeWrapperPass>
  passes;

char GlobalLiveMemory::ID = 0;

INITIALIZE_PROVIDER_BEGIN(passes, "GLM-FP", "global-live-mem-func-provider")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(passes, "GLM-FP", "global-live-mem-func-provider")

INITIALIZE_PASS_BEGIN(GlobalLiveMemory, "GLM", "global-live-mem", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(passes)
INITIALIZE_PASS_END(GlobalLiveMemory, "GLM", "global-live-mem", true, true)



void GlobalLiveMemory::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<passes>();
  AU.setPreservesAll();
}

ModulePass* llvm::createGlobalLiveMemoryPass() {
  return new GlobalLiveMemory();
}

void printFuncToCAllInstLiveSet(FuncToCallInstLiveSet* Map) {
  dbgs() << "[GLOBAL_LIVE_MEMORY]: SUMMARY MAP\n";
  for (auto& CurrFunc = Map->begin(), LastFunc = Map->end();
    CurrFunc != LastFunc;
    ++CurrFunc) {
    LLVM_DEBUG(
      dbgs() << CurrFunc->getFirst()->getName() << " calls from:\n";
    );

    auto& Vector = CurrFunc->getSecond();
    for (auto& CurrPair = Vector.begin(), LastPair = Vector.end();
      CurrPair != LastPair;
      ++CurrPair) {
      LLVM_DEBUG(
        dbgs() <<"    " << CurrPair->first->getFunction()->getName() << "\n";
      );
    }
  }
}

void addFuncInMap(
    FuncToCallInstLiveSet& Map, 
    CallGraphNode::iterator CurrCallRecord) {

  Function* CalledFunc = CurrCallRecord->second->getFunction();
  if (!Map.count(CalledFunc)) {

    std::vector< 
      std::pair<CallInst*, std::unique_ptr<LiveSet>>
    > CallInstLiveSet;
    CallInstLiveSet.
      push_back(
        std::make_pair(cast<CallInst>(CurrCallRecord->first), nullptr)
      );
    Map.insert(std::make_pair(CalledFunc, std::move(CallInstLiveSet)));
  }
  else {
    Map[CalledFunc].push_back(
      std::make_pair(cast<CallInst>(CurrCallRecord->first), nullptr));
  }
}

void updateFuncToCallInstLiveSet(FuncToCallInstLiveSet& Map,
    LiveDFFwk& LiveFwk, 
    DFRegionInfo& RegInfoForF, 
    Function* F) {
  for (auto& RegMemSet = LiveFwk.getLiveInfo().begin(),
    LastRegMemSet = LiveFwk.getLiveInfo().end();
    RegMemSet != LastRegMemSet; ++RegMemSet) {

    for (auto& FuncVector = Map.begin(),
      LastFuncVector = Map.end();
      FuncVector != LastFuncVector; ++FuncVector) {

      for (auto CurrCall = FuncVector->second.begin(),
        LastCall = FuncVector->second.end();
        CurrCall != LastCall; ++CurrCall) {

        DFBlock* DFF;
        if (CurrCall->first->getFunction() == F) {
          DFF = cast<DFBlock>(
            RegInfoForF.getRegionFor(CurrCall->first->getParent())
          );
          if (DFF == RegMemSet->first) {
            CurrCall->second = std::move(RegMemSet->second);
          }
        }
      }
    }
  }
}

bool GlobalLiveMemory::runOnModule(Module &SCC) {
  LLVM_DEBUG(
    dbgs() << "[GLOBAL_LIVE_MEMORY]: Begin of GlobalLiveMemoryPass\n";
  );
  auto& CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

  ReversePostOrderTraversal<CallGraph*> RPOT(&CG);
  FuncToCallInstLiveSet MapOfFuncAndCallInstWithLiveSet;
  
  for (auto& CurrNode = RPOT.begin(), LastNode = RPOT.end(); 
    CurrNode != LastNode; 
    CurrNode++) {
    CallGraphNode* CGN = *CurrNode;
    if (auto F = CGN->getFunction()) {
      if (F->hasName() && !F->empty()) {
        LLVM_DEBUG(
          dbgs() << "[GLOBAL_LIVE_MEMORY]: analyzing " << F->getName() << "\n";
        );

        auto& PassesInfo = getAnalysis<passes>(*F);
        auto& RegInfoForF = PassesInfo.get<DFRegionInfoPass>().getRegionInfo();
        auto* TopBBFforF = cast<DFFunction>(RegInfoForF.getTopLevelRegion());
        auto& DefInfo = PassesInfo.get<DefinedMemoryPass>().getDefInfo();

        DominatorTree* DT = nullptr;
        LLVM_DEBUG(
          auto & DTPass = PassesInfo.get<DominatorTreeWrapperPass>();
          DT = &DTPass.getDomTree();
        );

        LiveMemoryInfo LiveInfo;
        auto LiveItr = LiveInfo.insert(
          std::make_pair(TopBBFforF, llvm::make_unique<LiveSet>())
        ).first;
        auto& LS = LiveItr->get<LiveSet>();


        for (auto CurrCallRecord = CGN->begin(), LastCallRecord = CGN->end();
          CurrCallRecord != LastCallRecord; CurrCallRecord++) {
          addFuncInMap(MapOfFuncAndCallInstWithLiveSet, CurrCallRecord);
        }
        
        auto Fout = LS->getOut();
        if (F->getName() != "main") {
          
          for (auto CurrÑall = MapOfFuncAndCallInstWithLiveSet[F].begin(),
            LastCall = MapOfFuncAndCallInstWithLiveSet[F].end();
            CurrÑall != LastCall; CurrÑall++) {
            if ((CurrÑall->first)->getCalledFunction() != F)
              continue;

            if (CurrÑall->second != nullptr) {
              MemorySet<MemoryLocationRange> DFFLSout;
              DFFLSout = CurrÑall->second->getOut();
              Fout.merge(DFFLSout);
            }
          }
          LS->setOut(Fout);
        }

        LiveDFFwk LiveFwk(LiveInfo, DefInfo, DT);
        solveDataFlowDownward(&LiveFwk, TopBBFforF);

        updateFuncToCallInstLiveSet(
          MapOfFuncAndCallInstWithLiveSet, LiveFwk, RegInfoForF, F);

        mIterprocLiveMemoryInfo.try_emplace(F, std::move(LiveInfo[TopBBFforF]));
      }
    }
  }
  LLVM_DEBUG(
    printFuncToCAllInstLiveSet(&MapOfFuncAndCallInstWithLiveSet);
  );
  LLVM_DEBUG(
    dbgs() << "[GLOBAL_LIVE_MEMORY]: End of GlobalLiveMemoryPass\n";
  );
  return false;
}