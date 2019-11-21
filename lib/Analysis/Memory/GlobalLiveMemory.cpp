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

INITIALIZE_PROVIDER_BEGIN(passes, "", "")
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(passes, "", "")

INITIALIZE_PASS_BEGIN(GlobalLiveMemory, "global-live-mem", "", true, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(passes)
INITIALIZE_PASS_END(GlobalLiveMemory, "global-live-mem", "", true, true)



void GlobalLiveMemory::getAnalysisUsage(AnalysisUsage& AU) const {
	AU.addRequired<CallGraphWrapperPass>();
	AU.addRequired<passes>();
	AU.setPreservesAll();
}

ModulePass* llvm::createGlobalLiveMemoryPass() {
	return new GlobalLiveMemory();
}

void printFuncToCAllInstLiveSet(FuncToCallInstLiveSet* Map) {
  for (auto& CurrFunc = Map->begin(), LastFunc = Map->end();
    CurrFunc != LastFunc;
    ++CurrFunc) {
    errs() << "--------------------------\n";
    errs() << CurrFunc->getFirst()->getName() << "\nCalls:\n";
    auto& Vector = CurrFunc->getSecond();
    for (auto& CurrPair = Vector.begin(), LastPair = Vector.end();
      CurrPair != LastPair;
      ++CurrPair) {
      errs() <<"\t"<< CurrPair->first->getFunction()->getName() << "\n";
    }
    errs() << "--------------------------\n";
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
    errs() << "Begin of GlobalLiveMemoryPass\n";
  );
	auto& CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

	ReversePostOrderTraversal<CallGraph*> RPOT(&CG);
	FuncToCallInstLiveSet MapOfFuncAndCallInstWithLiveSet;
	
  //ˆËÍÎ ÔÓ ÛÁÎ‡Ï CallGraph
  for (auto& CurrNode = RPOT.begin(), LastNode = RPOT.end(); 
    CurrNode != LastNode; 
    CurrNode++) {
		CallGraphNode* CGN = *CurrNode;
		if (auto F = CGN->getFunction()) {
			if (F->hasName() && !F->empty()) {
				LLVM_DEBUG(
					errs() << "===============\n";
				errs() << F->getName() << "\n";
				errs() << "===============\n";
				F->dump();
				errs() << "===============\n";
				);

				//get result of help passes
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


				//build function(F)->caller F intsruction 
				for (auto CurrCallRecord = CGN->begin(), LastCallRecord = CGN->end();
					CurrCallRecord != LastCallRecord; CurrCallRecord++) {
					addFuncInMap(MapOfFuncAndCallInstWithLiveSet, CurrCallRecord);
				}
				LLVM_DEBUG(
					//printFuncToCAllInstLiveSet(&MapOfFuncAndCallInstWithLiveSet);
				);
				//build OUT for F
				//for F vector of caller function is build
				// ÌÂ ÔÓÎÛ˜ËÎÓÒ¸ ‚˚ÌÂÒÚË ‚ ÙÛÌÍˆË˛ ÚÍ ˇ ÌÂ ÒÏÓ„ Ì‡ÈÚË ÚËÔ LS
				auto Fout = LS->getOut();
				if (F->getName() != "main") {
					//for main null
					//circle by instructions
					for (auto Curr—all = MapOfFuncAndCallInstWithLiveSet[F].begin(),
						LastCall = MapOfFuncAndCallInstWithLiveSet[F].end();
						Curr—all != LastCall; Curr—all++) {
						LLVM_DEBUG(
							errs() << "ColledFunc: "
							<< (Curr—all->first)->getCalledFunction()->getName() << "\n";
						);
						//if true than smth is bad
						if ((Curr—all->first)->getCalledFunction() != F)
							continue;

						//get liveset current call(if recursion is nullptr)
						if (Curr—all->second != nullptr) {
							MemorySet<MemoryLocationRange> DFFLSout;
							DFFLSout = Curr—all->second->getOut();
							//update LS out
							Fout.merge(DFFLSout);
						}
					}
					LS->setOut(Fout);
				}
				//mIterprocLiveMemoryInfo->try_emplace(F, std::make_unique<LiveMemoryInfo>(LiveInfo));

				//run Live Memory Analysis
				LiveDFFwk LiveFwk(LiveInfo, DefInfo, DT);
				solveDataFlowDownward(&LiveFwk, TopBBFforF);

				//update MapOfFuncAndCallInstWithLiveSet
				updateFuncToCallInstLiveSet(
					MapOfFuncAndCallInstWithLiveSet, LiveFwk, RegInfoForF, F);

				mIterprocLiveMemoryInfo->try_emplace(F, std::move(LiveInfo[TopBBFforF]));
			}
			else {
				LLVM_DEBUG(
					errs() << "function without name\n";
				);
			}

		}
    else {
      LLVM_DEBUG(
        errs() << "nullptr function\n";
      );
    }
	}
  LLVM_DEBUG(
    errs() << "End of GlobalLiveMemoryPass\n";
  );
  return false;
}