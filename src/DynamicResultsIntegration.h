#pragma once

#include "DIMemoryTrait.h"
#include "tsar_pass.h"
#include "../../../dyna/JsonObjects.h"
#include <llvm/Pass.h>
#include <llvm/IR/DebugLoc.h>
#include <bcl/utility.h>
#include <map>
#include <set>
#include <sstream>

namespace llvm {

class DynResultsIntegrationPass : public FunctionPass, bcl::Uncopyable {
public:
  static char ID;
  const static std::string DYNAMIC_RESULTS_JSON;

  DynResultsIntegrationPass() : FunctionPass(ID) {
    initializeDynResultsIntegrationPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function&) override;

  void getAnalysisUsage(AnalysisUsage&) const override;

  template<class OStream>
  void print(OStream& OS) const {
    OS << "print\n";
    auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
    for (auto &Loop : TraitPool) {
      auto LoopID = cast<MDNode>(Loop.getFirst());
      const auto &Loc = getDebugLocByID(LoopID);
      if (!Loc) continue;
      OS << "Loop:\n\tFile: "  << cast<DIScope>(Loc.getScope())->getFilename()
        .str() << " Line: " << Loc.getLine() << " Col: " << Loc.getCol()
        << "\n";
      std::map<std::string, std::set<std::string>> Variables {
        std::make_pair("Flow", std::set<std::string>()),
        std::make_pair("Anti", std::set<std::string>()),
        std::make_pair("Private", std::set<std::string>()),
      };
      for (auto &Region : *Loop.getSecond()) {
        tsar::DIMemory* Memory = Region.getFirst();
        if (!tsar::DIEstimateMemory::classof(Memory)) continue;
        auto Var = cast<tsar::DIEstimateMemory>(Memory)->getVariable();
        std::stringstream Variable;
        Variable << "File: " << Var->getScope()->getFilename().str()
          << " Name: " << Var->getName().str() << " Line: " << Var->getLine();
        if (Region.getSecond().is<tsar::trait::Flow>()) {
          Variables["Flow"].insert(Variable.str());
        }
        if (Region.getSecond().is<tsar::trait::Anti>()) {
          Variables["Anti"].insert(Variable.str());
        }
        if (Region.getSecond().is<tsar::trait::Private>()) {
          Variables["Private"].insert(Variable.str());
        }
      }
      for (auto &Var : Variables) {
        OS << Var.first << "\n";
        for (auto &Each : Var.second) {
          OS << "\t" << Each << "\n";
        }
      }
    }
  }
private:
  using LoopsMap = std::map<std::tuple<std::string, unsigned, unsigned>, 
   dyna::Loop>;
  LoopsMap getLoopsMap(dyna::Info&) const;
  DebugLoc getDebugLocByID(const MDNode*) const;
  inline bool isSameVariable(const dyna::Var&, const DIVariable&) const;
};
}
