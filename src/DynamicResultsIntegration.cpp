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

DynResultsIntegrationPass::VarsSet DynResultsIntegrationPass::getPrivateVarsSet(
  dyna::Info& Info,
  dyna::Loop& Loop) const {
  VarsSet Result;
  for (auto &Private : Loop[dyna::Loop::Private]) {
    auto &Var = Info[dyna::Info::Vars][Private];
    Result.insert(std::make_tuple(Var[dyna::Var::Name],
      Var[dyna::Var::Line], Var[dyna::Var::File]));
  }
  return Result;
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
    auto DynaVars = getPrivateVarsSet(Info, DynaLoops[LoopDescr]);
    for (auto &Region : *TraitLoop.getSecond()) {
      tsar::DIMemory* Memory = Region.getFirst();
      if (!tsar::DIEstimateMemory::classof(Memory)) continue;
      auto DIVar = cast<tsar::DIEstimateMemory>(Memory)->getVariable();
      auto PointerPrefix = (cast<tsar::DIEstimateMemory>(Memory)
        ->getExpression()->getNumElements() ? "^" : "");
      auto VarDescr = std::make_tuple(PointerPrefix + DIVar->getName().str(),
        DIVar->getLine(), DIVar->getScope()->getFilename().str());
      if (Region.getSecond().is<tsar::trait::Private>() &&
        DynaVars.find(VarDescr) == DynaVars.end()) {
        Region.getSecond().set<tsar::trait::Readonly>();
      } else if (!Region.getSecond().is<tsar::trait::Private>() &&
        DynaVars.find(VarDescr) != DynaVars.end()) {
        Region.getSecond().set<tsar::trait::DynamicPrivate>();
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
