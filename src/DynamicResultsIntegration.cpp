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

bool DynResultsIntegrationPass::runOnFunction(Function &F) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  std::ifstream In(DYNAMIC_RESULTS_JSON);
  std::string ResultsJson;
  In >> ResultsJson;
  In.close();
  json::Parser<> Parser(ResultsJson);
  dyna::Info Info;
  if (!Parser.parse(Info)) {
    std::cerr << "Error while parsing " << DYNAMIC_RESULTS_JSON << std::endl;
    return true;
  }
  auto DynaLoops = getLoopsMap(Info);
  for (auto &TraitLoop : TraitPool) {
    auto LoopID = cast<MDNode>(TraitLoop.getFirst());
    const auto &Loc = getDebugLocByID(LoopID);
    auto LoopDescr = std::make_tuple(cast<DIScope>(Loc.getScope())
      ->getFilename().str(), Loc.getLine(), Loc.getCol());
    if (DynaLoops.find(LoopDescr) == DynaLoops.end()) continue;
    auto DynaPrivateVars = getVarsSet(Info, DynaLoops[LoopDescr],
      dyna::Loop::Private);
    auto DynaFlowVars = getVarsMap(Info, DynaLoops[LoopDescr],
      dyna::Loop::Flow);
    auto DynaAntiVars = getVarsMap(Info, DynaLoops[LoopDescr],
      dyna::Loop::Anti);
    for (auto &Region : *TraitLoop.getSecond()) {
      tsar::DIMemory* Memory = Region.getFirst();
      if (!tsar::DIEstimateMemory::classof(Memory)) continue;
      auto DIVar = cast<tsar::DIEstimateMemory>(Memory)->getVariable();
      auto PointerPrefix = (cast<tsar::DIEstimateMemory>(Memory)
        ->getExpression()->getNumElements() ? "^" : "");
      auto VarDescr = std::make_tuple(PointerPrefix + DIVar->getName().str(),
        DIVar->getLine(), DIVar->getScope()->getFilename().str());
      bool HasStaticInfo = Region.getSecond().is<tsar::trait::Private>() ||
        Region.getSecond().is<tsar::trait::Anti>() ||
        Region.getSecond().is<tsar::trait::Flow>();
      bool HasDynamicInfo =
        !(DynaPrivateVars.find(VarDescr) == DynaPrivateVars.end() &&
        DynaFlowVars.find(VarDescr) == DynaFlowVars.end() &&
        DynaAntiVars.find(VarDescr) == DynaAntiVars.end());
      assert((HasStaticInfo || !HasDynamicInfo)
        "All variables detected in dynamic should be detected on static too");
      if (HasStaticInfo && !HasDynamicInfo) {
        Region.getSecond().template set<tsar::trait::Readonly>();
        continue;
      }
      if (!Region.getSecond().is<tsar::trait::Private>() &&
        DynaPrivateVars.find(VarDescr) != DynaPrivateVars.end() &&
        DynaAntiVars.find(VarDescr) == DynaAntiVars.end() &&
        DynaFlowVars.find(VarDescr) == DynaFlowVars.end()) {
        //Looks like it impossible to set DynamicPrivate/Private trait
        //simulteniously with Flow/Anti traits. But in dynamic analysis results
        //they usually in a such way. Decided not to set DynamicPrivate in those
        //cases.
        Region.getSecond().template set<tsar::trait::DynamicPrivate>();
      }
      if (DynaAntiVars.find(VarDescr) != DynaAntiVars.end()) {
        setDistance<tsar::trait::Anti>(DynaAntiVars[VarDescr],
          Region.getSecond());
      } else if (Region.getSecond().is<tsar::trait::Anti>()) {
        //Need to unset trait. How to compile this?
        //Region.getSecond().template unset<tsar::trait::Anti>();
      }
      if (DynaFlowVars.find(VarDescr) != DynaFlowVars.end()) {
        setDistance<tsar::trait::Flow>(DynaFlowVars[VarDescr],
          Region.getSecond());
      } else if (Region.getSecond().is<tsar::trait::Flow>()) {
        //Need to unset trait. How to compile this?
        //Region.getSecond().template unset<tsar::trait::Flow>();
      }
    }
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
