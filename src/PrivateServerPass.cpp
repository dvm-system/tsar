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
#include <clang/AST/ASTContext.h>
#include <clang/AST/Stmt.h>
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
  ~Statistic() override {}

  Statistic(const Statistic &) = default;
  Statistic & operator=(const Statistic &) = default;
  Statistic(Statistic &&) = default;
  Statistic & operator=(Statistic &&) = default;
JSON_OBJECT_END(Statistic)

JSON_OBJECT_BEGIN(MainLoopInfo)
JSON_OBJECT_PAIR_5(MainLoopInfo,
  StartCol, unsigned,
  StartLine, unsigned,
  EndCol, unsigned,
  EndLine, unsigned,
  Level, unsigned)

  MainLoopInfo() = default;
  ~MainLoopInfo() = default;

  MainLoopInfo(const MainLoopInfo &) = default;
  MainLoopInfo & operator=(const MainLoopInfo &) = default;
  MainLoopInfo(MainLoopInfo &&) = default;
  MainLoopInfo & operator=(MainLoopInfo &&) = default;
JSON_OBJECT_END(MainLoopInfo)

JSON_OBJECT_BEGIN(MainFuncInfo)
JSON_OBJECT_PAIR_2(MainFuncInfo,
  Name, std::string,
  Loops, std::vector<MainLoopInfo>)

  MainFuncInfo() = default;
  ~MainFuncInfo() = default;

  MainFuncInfo(const MainFuncInfo &) = default;
  MainFuncInfo & operator=(const MainFuncInfo &) = default;
  MainFuncInfo(MainFuncInfo &&) = default;
  MainFuncInfo & operator=(MainFuncInfo &&) = default;
JSON_OBJECT_END(MainFuncInfo)

JSON_OBJECT_BEGIN(FunctionList)
JSON_OBJECT_ROOT_PAIR(FunctionList,
  Functions, std::vector<MainFuncInfo>)

  FunctionList() : JSON_INIT_ROOT {}
  ~FunctionList() override {}

  FunctionList(const FunctionList &) = default;
  FunctionList & operator=(const FunctionList &) = default;
  FunctionList(FunctionList &&) = default;
  FunctionList & operator=(FunctionList &&) = default;
JSON_OBJECT_END(FunctionList)
}
}

JSON_DEFAULT_TRAITS(tsar::msg::, Statistic)
JSON_DEFAULT_TRAITS(tsar::msg::, MainLoopInfo)
JSON_DEFAULT_TRAITS(tsar::msg::, MainFuncInfo)
JSON_DEFAULT_TRAITS(tsar::msg::, FunctionList)

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

std::string answerStatistic(llvm::PrivateServerPass * const PSP,
    llvm::Module &M, tsar::TransformationContext *TfmCtx) {
  msg::Statistic Stat;
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
  auto &MMP = PSP->getAnalysis<MemoryMatcherImmutableWrapper>();
  Stat[msg::Statistic::Variables].insert(
    std::make_pair(msg::Analysis::Yes, MMP->Matcher.size()));
  Stat[msg::Statistic::Variables].insert(
    std::make_pair(msg::Analysis::No, MMP->UnmatchedAST.size()));
  std::pair<unsigned, unsigned> Loops(0, 0);
  for (Function &F : M) {
    if (F.empty())
      continue;
    ++Stat[msg::Statistic::Functions];
    auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
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
}

void getLoopInfo(clang::Stmt *Ptr, clang::SourceManager &SrcMgr,
    msg::MainLoopInfo &Loop) {
  auto LocStart = Ptr->getLocStart();
  auto LocEnd = Ptr->getLocEnd();
  auto StartCol = SrcMgr.getExpansionColumnNumber(LocStart);
  auto StartLine = SrcMgr.getExpansionLineNumber(LocStart);
  auto EndCol = SrcMgr.getExpansionColumnNumber(LocEnd);
  auto EndLine = SrcMgr.getExpansionLineNumber(LocEnd);
  Loop[msg::MainLoopInfo::StartCol] = StartCol;
  Loop[msg::MainLoopInfo::StartLine] = StartLine;
  Loop[msg::MainLoopInfo::EndCol] = EndCol;
  Loop[msg::MainLoopInfo::EndLine] = EndLine;
}

std::string answerFunctionList(llvm::PrivateServerPass * const PSP,
    llvm::Module &M, tsar::TransformationContext *TfmCtx) {
  msg::FunctionList FuncLst;
  typedef msg::MainFuncInfo FuncInfo;
  typedef msg::MainLoopInfo LoopInfo;
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  for (Function &F : M) {
    if (F.empty())
      continue;
    msg::MainFuncInfo Func;
    Func[FuncInfo::Name] = F.getName();
    auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    msg::MainLoopInfo Loop;
    for (auto &Match : Matcher) {
      getLoopInfo(Match.get<AST>(), SrcMgr, Loop);
      Func[FuncInfo::Loops].push_back(Loop);
    }
    for (auto &Unmatch : Unmatcher) {
      getLoopInfo(Unmatch, SrcMgr, Loop);
      Func[FuncInfo::Loops].push_back(Loop);
    }
    std::sort(Func[FuncInfo::Loops].begin(),
      Func[FuncInfo::Loops].end(),
      [](msg::MainLoopInfo &LHS,
          msg::MainLoopInfo &RHS) -> bool {
      return (LHS[LoopInfo::StartLine] < RHS[LoopInfo::StartLine]) ||
          ((LHS[LoopInfo::StartLine] == RHS[LoopInfo::StartLine]) &&
          (LHS[LoopInfo::StartCol] < RHS[LoopInfo::StartCol]));
    });
    std::vector<std::pair<unsigned, unsigned>> Levels;
    for (auto &Loop : Func[FuncInfo::Loops]) {
      while (!Levels.empty() &&
          ((Levels[Levels.size() - 1].first < Loop[LoopInfo::EndLine]) ||
          ((Levels[Levels.size() - 1].first == Loop[LoopInfo::EndLine]) &&
          (Levels[Levels.size() - 1].second < Loop[LoopInfo::EndCol]))))
        Levels.pop_back();
      Loop[msg::MainLoopInfo::Level] = Levels.size() + 1;
      Levels.emplace_back(
        std::make_pair(Loop[LoopInfo::EndLine], Loop[LoopInfo::EndCol]));
    }
    FuncLst[msg::FunctionList::Functions].push_back(Func);
  }
  return json::Parser<msg::FunctionList>::unparseAsObject(FuncLst);
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
  while (mConnection->answer(
      [this, &M, &TfmCtx](const std::string &Request) -> std::string {
    msg::Diagnostic Diag(msg::Status::Error);
    if (mStdErr->isDiff()) {
      Diag[msg::Diagnostic::Terminal] += mStdErr->diff();
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    json::Parser<msg::Statistic, msg::FunctionList> P(Request);
    auto Obj = P.parse();
    assert(Obj && "error: invalid request");
    if (Obj->is<msg::Statistic>())
      return answerStatistic(this, M, TfmCtx);
    if (Obj->is<msg::FunctionList>())
      return answerFunctionList(this, M, TfmCtx);
  }));
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
