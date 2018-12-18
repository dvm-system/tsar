#include "DynamicResultsIntegration.h"
#include "DIDependencyAnalysis.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <iostream>
#include <fstream>

using namespace llvm;

INITIALIZE_PASS_BEGIN(DynResultsIntegrationPass, "dyn-results-integration",
  "Dynamic Analysis Results Integration", false, false)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_END(DynResultsIntegrationPass, "dyn-results-integration",
  "Dynamic Analysis Results Integration", false, false)

char DynResultsIntegrationPass::ID = 0;
const std::string DynResultsIntegrationPass::DYNAMIC_RESULTS_JSON
  = "DynamicResults.json";

//Keys in DIMemoryTraitPool are MDNodes. To match them with dynamic analysis
//results we need to find appropriate DebugLoc. 
//See LoopInfo.cpp, 335
DebugLoc DynResultsIntegrationPass::getDebugLocByID(const MDNode *ID) const {
  for (unsigned i = 1, ie = ID->getNumOperands(); i < ie; ++i) {
    if (DILocation *L = dyn_cast<DILocation>(ID->getOperand(i))) {
      return DebugLoc(L);
    }
  }
  return DebugLoc();
}

DynResultsIntegrationPass::LoopsMap DynResultsIntegrationPass::getLoopsMap(
  dyna::Info& Info) const {
  LoopsMap Result;
  for (auto &Loop : Info[dyna::Info::Loops]) {
    Result.insert(std::make_pair(std::make_tuple(Loop[dyna::Loop::File],
      Loop[dyna::Loop::Line], Loop[dyna::Loop::Column]), Loop));
  }
  return Result;
}

inline bool DynResultsIntegrationPass::isSameVariable(
  const dyna::Var& DynaVar,
  const DIVariable& DIVar) const {
  //No column information in llvm::DIVariable. Ignore it
  return
    DynaVar[dyna::Var::File] == DIVar.getScope()->getFilename().str()
    && DynaVar[dyna::Var::Name] == DIVar.getName().str()
    && DynaVar[dyna::Var::Line] == DIVar.getLine();
}

bool DynResultsIntegrationPass::runOnFunction(Function &F) {
  print(std::cout);
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  std::ifstream In(DYNAMIC_RESULTS_JSON);
  std::string ResultsJson;
  In >> ResultsJson;
  In.close();
  json::Parser<> Parser(ResultsJson);
  dyna::Info Info;
  if (!Parser.parse(Info)) return true;
  auto DynaLoops = getLoopsMap(Info);
  for (auto &TraitLoop : TraitPool) {
    auto LoopID = cast<MDNode>(TraitLoop.getFirst());
    const auto &Loc = getDebugLocByID(LoopID);
    auto LoopDescr = std::make_tuple(cast<DIScope>(Loc.getScope())
      ->getFilename().str(), Loc.getLine(), Loc.getCol());
    if (DynaLoops.find(LoopDescr) == DynaLoops.end()) continue;
    for (auto &Private : DynaLoops[LoopDescr][dyna::Loop::Private]) {
      auto DynaVar = Info[dyna::Info::Vars][Private];
      for (auto &Region : *TraitLoop.getSecond()) {
        tsar::DIMemory* Memory = Region.getFirst();
        if (!tsar::DIEstimateMemory::classof(Memory)) continue;
        if (!isSameVariable(DynaVar, *cast<tsar::DIEstimateMemory>(Memory)
          ->getVariable())) continue;
        if (!Region.getSecond().is<tsar::trait::Private>()) {
          Region.getSecond().set<tsar::trait::Private>();
        }
        //Should i also unset trait::privates if they are not in dynamic
        //analysis result? Using for those cases something like 
        //"Region.getSecond().unset<tsar::trait::Private>()" does not compile.
        //
        //e.g:
        //for (int i = 0; i < 5; ++i) 
        //  if (i == 10) int n = 1;
        //n would be as private in DIMemoryTraitPoolWrapper results but not in
        //dynamic analysis results.
      }        
    }
    //for (auto &Anti : DynaLoops[LoopDescr][dyna::Loop::Anti]);
    //for (auto &Flow : DynaLoops[LoopDescr][dyna::Loop::Flow]);
    //I don't understand how flow and anti works. Variables for which flow and
    //anti traits are set by DIMemoryTraitPoolWrapper, are completely different
    //from flow and anti got by dynamic analysis. Are they represent the same
    //entities? Left it blank now.
  }
  print(std::cout);
  return true;
}

void DynResultsIntegrationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DIMemoryTraitPoolWrapper>();
}

FunctionPass * llvm::createDynResultsIntegrationPass() {
  return new DynResultsIntegrationPass();
}
