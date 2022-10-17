//===- AnalysisWriter.cpp -- Reader For DYNA Results -------------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file implements a pass to write results of analysis to a JSON-file.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/MemoryTraitJSON.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Reader/AnalysisJSON.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/OutputFile.h"
#include "tsar/Support/Tags.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include "tsar/Unparse/VariableLocation.h"
#include <bcl/utility.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

namespace {
class AnalysisWriter : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  AnalysisWriter() : ModulePass(ID) {
    initializeAnalysisWriterPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;
};
} // namespace


bool AnalysisWriter::runOnModule(Module &M) {
  std::string DataFile{"analysis.json"};
  auto OF{OutputFile::create(DataFile, false)};
  if (!OF) {
    M.getContext().diagnose(DiagnosticInfoPGOProfile(
        DataFile.data(), Twine("unable to open file: ") +
                             errorToErrorCode(OF.takeError()).message()));
    return false;
  }
  auto &TraitPool{getAnalysis<DIMemoryTraitPoolWrapper>().get()};
  trait::Info Info;
  std::map<VariableLocationT, std::size_t> VariableCache;
  for (auto &F : M) {
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto DWLang{getLanguage(F)};
    if (!DWLang)
      continue;
    SmallString<128> Path;
    trait::Function FInfo;
    FInfo[trait::Function::Name] = DISub->getLinkageName();
    if (FInfo[trait::Function::Name].empty())
      FInfo[trait::Function::Name] = DISub->getName();
    FInfo[trait::Function::File] = getAbsolutePath(*DISub, Path);
    FInfo[trait::Function::Line] = DISub->getLine();
    FInfo[trait::Function::Column] = 0;
    Info[trait::Info::Functions].push_back(std::move(FInfo));
    auto &LI{getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo()};
    for_each_loop(LI, [DWLang, &TraitPool, &VariableCache,
                       &Info](const Loop *L) {
      if (auto LoopID{L->getLoopID()})
        if (auto Loc{L->getStartLoc()}; Loc && Loc.get()) {
          auto &LInfo{[Loc, &Info]() -> trait::Loop & {
            trait::Loop LInfo;
            SmallString<128> Path;
            LInfo[trait::Loop::File] =
                getAbsolutePath(*cast<DIScope>(Loc.getScope()), Path);
            LInfo[trait::Loop::Line] = Loc.getLine();
            LInfo[trait::Loop::Column] = Loc.getCol();
            Info[trait::Info::Loops].push_back(std::move(LInfo));
            return Info[trait::Info::Loops].back();
          }()};
          auto DITraitsItr{TraitPool.find(LoopID)};
          if (DITraitsItr == TraitPool.end())
            return;
          for (auto &DITrait : *DITraitsItr->get<Pool>()) {
            if (DITrait.is<trait::NoAccess>())
              continue;
            if (auto *DIEM{dyn_cast<DIEstimateMemory>(DITrait.getMemory())}) {
              SmallString<128> Path;
              if (auto V{buildVariable(*DWLang, *DIEM, Path)}) {
                auto VarInfo{VariableCache.try_emplace(
                    *V, Info[trait::Info::Vars].size())};
                if (VarInfo.second) {
                  trait::Var VInfo;
                  VInfo[trait::Var::Name] = V->get<Identifier>();
                  VInfo[trait::Var::Line] = V->get<Line>();
                  VInfo[trait::Var::Column] = V->get<Column>();
                  VInfo[trait::Var::File] = Path.str().str();
                  Info[trait::Info::Vars].push_back(std::move(VInfo));
                }
                if (DITrait.is<trait::UseAfterLoop>())
                  LInfo[trait::Loop::UseAfterLoop].insert(
                      VarInfo.first->second);
                if (DITrait.is<trait::NoAccess>())
                  continue;
                if (DITrait.is<trait::Readonly>()) {
                  LInfo[trait::Loop::ReadOccurred].insert(
                      VarInfo.first->second);
                  continue;
                }
                LInfo[trait::Loop::ReadOccurred].insert(VarInfo.first->second);
                LInfo[trait::Loop::WriteOccurred].insert(VarInfo.first->second);
                if (DITrait.is<trait::Reduction>()) {
                  auto Red{DITrait.get<trait::Reduction>()};
                  LInfo[trait::Loop::Reduction].try_emplace(
                      VarInfo.first->second,
                      Red ? Red->getKind() : trait::Reduction::RK_NoReduction);
                  LInfo[trait::Loop::DefBeforeLoop].insert(
                      VarInfo.first->second);
                } else if (DITrait.is<trait::Induction>()) {
                  auto Itr{LInfo[trait::Loop::Induction]
                               .try_emplace(VarInfo.first->second)
                               .first};
                  if (auto I{DITrait.get<trait::Induction>()}) {
                    if (auto S{I->getStart()};
                        S &&
                        S->getSignificantBits() <=
                            sizeof Itr->second[trait::Range::Start] * CHAR_BIT)
                      Itr->second[trait::Range::Start] = S->getSExtValue();
                    if (auto E{I->getEnd()};
                        E &&
                        E->getSignificantBits() <=
                            sizeof Itr->second[trait::Range::End] * CHAR_BIT)
                      Itr->second[trait::Range::End] = E->getSExtValue();
                    if (auto S{I->getStep()};
                        S &&
                        S->getSignificantBits() <=
                            sizeof Itr->second[trait::Range::Step] * CHAR_BIT)
                      Itr->second[trait::Range::Step] = S->getSExtValue();
                  }
                  LInfo[trait::Loop::DefBeforeLoop].insert(
                      VarInfo.first->second);
                } else if (DITrait.is_any<trait::Private, trait::DynamicPrivate,
                                          trait::SecondToLastPrivate>()) {
                  LInfo[trait::Loop::Private].insert(VarInfo.first->second);
                } else if (DITrait.is<trait::FirstPrivate>()) {
                  LInfo[trait::Loop::DefBeforeLoop].insert(
                      VarInfo.first->second);
                  LInfo[trait::Loop::Private].insert(VarInfo.first->second);
                } else if (!DITrait.is<trait::Shared>()) {
                  if (!DITrait.is<trait::Output>())
                    LInfo[trait::Loop::DefBeforeLoop].insert(
                        VarInfo.first->second);
                  if (DITrait
                          .is_any<trait::Output, trait::Flow, trait::Anti>()) {
                    auto storeRange = [](auto *Dep, auto Itr) {
                      if (Dep && Dep->getLevels() > 0) {
                        auto DV{Dep->getDistance(0)};
                        if (DV.first &&
                            sizeof Itr->second[trait::Distance::Min] *
                                    CHAR_BIT >=
                                DV.first->getSignificantBits())
                          Itr->second[trait::Distance::Min] =
                              DV.first->getSExtValue();
                        if (DV.second &&
                            sizeof Itr->second[trait::Distance::Min] *
                                    CHAR_BIT >=
                                DV.second->getSignificantBits())
                          Itr->second[trait::Distance::Max] =
                              DV.second->getSExtValue();
                      }
                    };
                    if (DITrait.is<trait::Output>())
                      LInfo[trait::Loop::Output].insert(VarInfo.first->second);
                    if (DITrait.is<trait::Flow>())
                      storeRange(DITrait.get<trait::Flow>(),
                                 LInfo[trait::Loop::Flow]
                                     .try_emplace(VarInfo.first->second)
                                     .first);
                    if (DITrait.is<trait::Anti>())
                      storeRange(DITrait.get<trait::Flow>(),
                                 LInfo[trait::Loop::Anti]
                                     .try_emplace(VarInfo.first->second)
                                     .first);
                  } else {
                    LInfo[trait::Loop::Output].insert(VarInfo.first->second);
                    LInfo[trait::Loop::Flow].try_emplace(VarInfo.first->second);
                    LInfo[trait::Loop::Anti].try_emplace(VarInfo.first->second);
                  }
                }
              }
            }
          }
        }
    });
  }
  errs() << "Writing '" << DataFile << "'...\n";
  OF->getStream() << json::Parser<trait::Info>::unparseAsObject(Info);
  if (auto E{OF->clear()}) {
    std::string Msg;
    raw_string_ostream{Msg} << E;
    M.getContext().diagnose(DiagnosticInfoPGOProfile(
        DataFile.data(), Twine("unable to write file: ") + Msg));
    return false;
  }
  return false;
}

void AnalysisWriter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

void AnalysisWriter::print(raw_ostream &OS, const Module *M) const {}

char AnalysisWriter::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(
    AnalysisWriter, "print-analysis", "Analysis Results Writer", true, true,
    DefaultQueryManager::OutputPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(
    AnalysisWriter, "print-analysis", "Analysis Results Writer", true, true,
    DefaultQueryManager::OutputPassGroup::getPassRegistry())

ModulePass *llvm::createAnalysisWriter() { return new AnalysisWriter(); }