//===- PrivateServerPass.cpp -- Test Result Printer -------------*- C++ -*-===//
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
// This file implements a pass to interact with client software and to provide
//
//===----------------------------------------------------------------------===//

#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "tsar_loop_matcher.h"
#include "tsar_memory_matcher.h"
#include "Messages.h"
#include "tsar_pass_provider.h"
#include "tsar_private.h"
#include "PrivateServerPass.h"
#include "tsar_trait.h"
#include "tsar_transformation.h"
#include <bcl/cell.h>
#include <bcl/IntrusiveConnection.h>
#include <bcl/Json.h>
#include <bcl/RedirectIO.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>

using namespace llvm;
using namespace tsar;
using ::llvm::Module;

#undef DEBUG_TYPE
#define DEBUG_TYPE "server-private"

namespace tsar {
namespace msg {
/// \brief This message provides statistic of program analysis results.
///
/// This contains number of analyzed files, functions, loops and variables and
/// number of explored traits, such as induction, reduction, data dependencies,
/// etc.
JSON_OBJECT_BEGIN(Statistic)
JSON_OBJECT_ROOT_PAIR_5(Statistic,
  Functions, unsigned,
  Files, std::map<BCL_JOIN(std::string, unsigned)>,
  Loops, std::map<BCL_JOIN(Analysis, unsigned)>,
  Variables, std::map<BCL_JOIN(Analysis, unsigned)>,
  Traits, bcl::StaticTraitMap<BCL_JOIN(unsigned, MemoryDescriptor)>)

  struct InitTraitsFunctor {
    template<class Trait> inline void operator()(unsigned &C) { C = 0; }
  };

  Statistic() : JSON_INIT_ROOT, JSON_INIT(Statistic, 0) {
    (*this)[Statistic::Traits].for_each(InitTraitsFunctor());
  }

  Statistic(const Statistic &) = default;
  Statistic & operator=(const Statistic &) = default;
  Statistic(Statistic &&) = default;
  Statistic & operator=(Statistic &&) = default;

JSON_OBJECT_END(Statistic)
}
}

JSON_DEFAULT_TRAITS(tsar::msg::, Statistic)

using ServerPrivateProvider = FunctionPassProvider<
  BasicAAWrapperPass,
  PrivateRecognitionPass,
  TransformationEnginePass,
  LoopMatcherPass,
  DFRegionInfoPass>;

INITIALIZE_PROVIDER_BEGIN(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PROVIDER_END(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")

char PrivateServerPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)
INITIALIZE_PASS_DEPENDENCY(ServerPrivateProvider)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)

namespace {
/// Increments count of analyzed traits in a specified map TM.
template<class TraitMap>
void incrementTraitCount(ServerPrivateProvider &P, TraitMap &TM) {
  auto &LMP = P.get<LoopMatcherPass>();
  auto &PI = P.get<PrivateRecognitionPass>().getPrivateInfo();
  auto &RI = P.get<DFRegionInfoPass>().getRegionInfo();
  for (auto &Match : LMP.getMatcher()) {
    auto N = RI.getRegionFor(Match.get<IR>());
    auto DSItr = PI.find(N);
    assert(DSItr != PI.end() && DSItr->get<DependencySet>() &&
      "Loop traits must be specified!");
    auto ATRoot = DSItr->get<DependencySet>()->getAliasTree()->getTopLevelNode();
    for (auto &AT : *DSItr->get<DependencySet>()) {
      if (ATRoot == AT.getNode())
        continue;
      for (auto &T : make_range(AT.begin(), AT.end())) {
        if (T.is<trait::NoAccess>()) {
          if (T.is<trait::AddressAccess>())
            ++TM.template value<trait::AddressAccess>();
          continue;
        }
        AT.for_each(bcl::TraitMapConstructor<
          MemoryDescriptor, TraitMap, bcl::CountInserter>(AT, TM));
      }
      for (auto &T : make_range(AT.unknown_begin(), AT.unknown_end())) {
        if (T.is<trait::NoAccess>()) {
          if (T.is<trait::AddressAccess>())
            ++TM.template value<trait::AddressAccess>();
          continue;
        }
        AT.for_each(bcl::TraitMapConstructor<
          MemoryDescriptor, TraitMap, bcl::CountInserter>(AT, TM));
      }
    }
  }
}
}

bool PrivateServerPass::runOnModule(llvm::Module &M) {
  if (!mConnection) {
    errs() << "error: intrusive connection is not specified for the module "
      << M.getName() << "\n";
    return false;
  }
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return false;
  }
  ServerPrivateProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass &TEP) {
      TEP.setContext(M, TfmCtx);
  });
  mConnection->answer(
      [this, &M, &TfmCtx](const std::string &Request) -> std::string {
    msg::Diagnostic Diag(msg::Status::Error);
    if (mStdErr->isDiff()) {
      Diag[msg::Diagnostic::Terminal] += mStdErr->diff();
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    msg::Statistic Stat;
    json::Parser<> P(Request);
    if (!P.parse(Stat)) {
      Diag.insert(msg::Diagnostic::Error, P.errors());
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    auto &Rewriter = TfmCtx->getRewriter();
    for (auto FI = Rewriter.getSourceMgr().fileinfo_begin(),
         EI = Rewriter.getSourceMgr().fileinfo_end(); FI != EI; ++FI) {
      auto Ext = sys::path::extension(FI->first->getName());
      std::string Kind;
      if (std::find(
            msg::HeaderFile::extensions().begin(),
            msg::HeaderFile::extensions().end(), Ext) !=
          msg::HeaderFile::extensions().end())
        Kind = msg::HeaderFile::name();
      else if (std::find(
            msg::SourceFile::extensions().begin(),
            msg::SourceFile::extensions().end(), Ext) !=
          msg::SourceFile::extensions().end())
        Kind = msg::SourceFile::name();
      else
        Kind = msg::OtherFile::name();
      auto I = Stat[msg::Statistic::Files].insert(std::make_pair(Kind, 1));
      if (!I.second)
        ++I.first->second;
    }
    auto &MMP = getAnalysis<MemoryMatcherImmutableWrapper>();
    Stat[msg::Statistic::Variables].insert(
      std::make_pair(msg::Analysis::Yes, MMP->Matcher.size()));
    Stat[msg::Statistic::Variables].insert(
      std::make_pair(msg::Analysis::No, MMP->UnmatchedAST.size()));
    std::pair<unsigned, unsigned> Loops(0, 0);
    for (Function &F : M) {
      if (F.empty())
        continue;
      ++Stat[msg::Statistic::Functions];
      auto &Provider = getAnalysis<ServerPrivateProvider>(F);
      auto &LMP = Provider.get<LoopMatcherPass>();
      Loops.first += LMP.getMatcher().size();
      Loops.second += LMP.getUnmatchedAST().size();
      incrementTraitCount(Provider, Stat[msg::Statistic::Traits]);
    }
    Stat[msg::Statistic::Loops].insert(
      std::make_pair(msg::Analysis::Yes, Loops.first));
    Stat[msg::Statistic::Loops].insert(
      std::make_pair(msg::Analysis::No, Loops.second));
    return json::Parser<msg::Statistic>::unparseAsObject(Stat);
  });
  return false;
}

void PrivateServerPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ServerPrivateProvider>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass * llvm::createPrivateServerPass(
    bcl::IntrusiveConnection &IC, bcl::RedirectIO &StdErr) {
  return new PrivateServerPass(IC, StdErr);
}
