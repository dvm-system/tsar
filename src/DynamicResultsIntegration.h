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
        std::make_pair("Readonly", std::set<std::string>()),
        std::make_pair("DynamicPrivate", std::set<std::string>()),
      };
      for (auto &Region : *Loop.getSecond()) {
        tsar::DIMemory* Memory = Region.getFirst();
        if (!tsar::DIEstimateMemory::classof(Memory)) continue;
        auto Var = cast<tsar::DIEstimateMemory>(Memory)->getVariable();
        auto Expr = cast<tsar::DIEstimateMemory>(Memory)->getExpression();
        std::stringstream Variable;
        Variable << "File: " << Var->getScope()->getFilename().str()
          << " Name: " << (Expr->getNumElements() ? "*" : "")
          << Var->getName().str() << " Line: " << Var->getLine();
        if (Region.getSecond().is<tsar::trait::Flow>()) {
          auto DIDep = Region.getSecond().get<tsar::trait::Flow>();
          std::stringstream FlowVar;
          if (DIDep) {
            auto Dist = DIDep->getDistance();
            auto Flag = DIDep->getFlags() & tsar::trait::Dependence::Flag
              ::UnknownDistance;
            if (!Flag && Dist.first && Dist.second) {
              FlowVar  << " Distance: " << Dist.first->getExtValue() << ":"
                << Dist.second->getExtValue();
            } else {
              FlowVar << " Unknown Distance";
            }
          }
          Variables["Flow"].insert(Variable.str() + FlowVar.str());
        }
        if (Region.getSecond().is<tsar::trait::Anti>()) {
          auto DIDep = Region.getSecond().get<tsar::trait::Anti>();
          std::stringstream AntiVar;
          if (DIDep) {
            auto Dist = DIDep->getDistance();
            auto Flag = DIDep->getFlags() & tsar::trait::Dependence::Flag
              ::UnknownDistance;
            if (!Flag && Dist.first && Dist.second) {
              AntiVar  << " Distance: " << Dist.first->getExtValue() << ":"
                << Dist.second->getExtValue();
            } else {
              AntiVar << " Unknown Distance";
            }
          }
          Variables["Anti"].insert(Variable.str() + AntiVar.str());
        }
        if (Region.getSecond().is<tsar::trait::Private>()) {
          Variables["Private"].insert(Variable.str());
        }
        if (Region.getSecond().is<tsar::trait::Readonly>()) {
          Variables["Readonly"].insert(Variable.str());
        }
        if (Region.getSecond().is<tsar::trait::DynamicPrivate>()) {
          Variables["DynamicPrivate"].insert(Variable.str());
        }
      }
      for (auto &Var : Variables) {
        OS << "  " << Var.first << "\n";
        for (auto &Each : Var.second) {
          OS << "\t" << Each << "\n";
        }
      }
    }
  }
private:
  using LoopsMap = std::map<std::tuple<std::string, unsigned, unsigned>,
   dyna::Loop>;
  using VarDescr = std::tuple<std::string, dyna::LineTy, std::string>;
  using VarsSet = std::set<VarDescr>;
  using VarsMap = std::map<VarDescr, std::pair<dyna::DistanceTy, dyna::DistanceTy>>;

  DebugLoc getDebugLocByID(const MDNode*) const;
  LoopsMap getLoopsMap(dyna::Info&) const;

  template<class T>
  VarsMap getVarsMap(dyna::Info& Info, dyna::Loop& Loop, T Field) const {
    VarsMap Result;
    for (auto &I : Loop[Field]) {
      auto &V = Info[dyna::Info::Vars][I.first];
      auto Var = std::make_tuple(V[dyna::Var::Name], V[dyna::Var::Line],
        V[dyna::Var::File]);
      auto Dist = std::make_pair(I.second[dyna::Distance::Min],
        I.second[dyna::Distance::Max]);
      Result.insert(std::make_pair(Var, Dist));
    }
    return Result;
  }

  template<class T>
  VarsSet getVarsSet(dyna::Info& Info, dyna::Loop& Loop, T Field) const {
    VarsSet Result;
    for (auto &I : Loop[Field]) {
      auto &Var = Info[dyna::Info::Vars][I];
      Result.insert(std::make_tuple(Var[dyna::Var::Name],
        Var[dyna::Var::Line], Var[dyna::Var::File]));
    }
    return Result;
  }

  template<class T>
  void setDistance(std::pair<dyna::DistanceTy, dyna::DistanceTy> &Dist,
    tsar::DIMemoryTraitSet &MemoryTrait) {
    auto Flag = tsar::trait::Dependence::Flag::No;
    auto DI = MemoryTrait.get<T>();
    if (DI) Flag = DI->getFlags();
    Flag &= (~tsar::trait::Dependence::Flag::UnknownDistance);
    tsar::trait::DIDependence::DistanceRange Range;
    Range.first = APSInt(APInt(8 * sizeof(Dist.first), Dist.first));
    Range.second = APSInt(APInt(8 * sizeof(Dist.second), Dist.second));
    MemoryTrait.template set<T>(new tsar::trait::DIDependence(Flag, Range));
  }
};
}
