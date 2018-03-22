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

#include "PrivateServerPass.h"
#include "InterprocAnalysis.h"
#include "PerfectLoop.h"
#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "tsar_loop_matcher.h"
#include "tsar_memory_matcher.h"
#include "Messages.h"
#include "tsar_pass_provider.h"
#include "tsar_private.h"
#include "tsar_trait.h"
#include "tsar_transformation.h"
#include <bcl/cell.h>
#include <bcl/IntrusiveConnection.h>
#include <bcl/Json.h>
#include <bcl/RedirectIO.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/Passes.h>
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
  ~Statistic() override = default;

  Statistic(const Statistic &) = default;
  Statistic & operator=(const Statistic &) = default;
  Statistic(Statistic &&) = default;
  Statistic & operator=(Statistic &&) = default;
JSON_OBJECT_END(Statistic)

JSON_OBJECT_BEGIN(Location)
JSON_OBJECT_PAIR_4(Location,
  Line, unsigned,
  Column, unsigned,
  MacroLine, unsigned,
  MacroColumn, unsigned)

  Location() = default;
  ~Location() = default;

  Location(const Location &) = default;
  Location & operator=(const Location &) = default;
  Location(Location &&) = default;
  Location & operator=(Location &&) = default;
JSON_OBJECT_END(Location)

JSON_OBJECT_BEGIN(LoopTraits)
JSON_OBJECT_PAIR_3(LoopTraits,
  IsAnalyzed, Analysis,
  Perfect, Analysis,
  InOut, Analysis)

  LoopTraits() :
    JSON_INIT(LoopTraits, Analysis::No, Analysis::No) {}
  ~LoopTraits() = default;

  LoopTraits(const LoopTraits &) = default;
  LoopTraits & operator=(const LoopTraits &) = default;
  LoopTraits(LoopTraits &&) = default;
  LoopTraits & operator=(LoopTraits &&) = default;
JSON_OBJECT_END(LoopTraits)

enum class LoopType : short {
  First = 0,
  For = First,
  While,
  DoWhile,
  Implicit,
  Invalid,
  Number = Invalid
};

JSON_OBJECT_BEGIN(MainLoopInfo)
JSON_OBJECT_PAIR_7(MainLoopInfo,
  ID, unsigned,
  StartLocation, Location,
  EndLocation, Location,
  Traits, LoopTraits,
  Exit, unsigned,
  Level, unsigned,
  Type, LoopType)

  MainLoopInfo() = default;
  ~MainLoopInfo() = default;

  MainLoopInfo(const MainLoopInfo &) = default;
  MainLoopInfo & operator=(const MainLoopInfo &) = default;
  MainLoopInfo(MainLoopInfo &&) = default;
  MainLoopInfo & operator=(MainLoopInfo &&) = default;
JSON_OBJECT_END(MainLoopInfo)

JSON_OBJECT_BEGIN(LoopTree)
JSON_OBJECT_ROOT_PAIR_2(LoopTree,
  ID, unsigned,
  Loops, std::vector<MainLoopInfo>)

  LoopTree() : JSON_INIT_ROOT {}
  ~LoopTree() override = default;

  LoopTree(const LoopTree &) = default;
  LoopTree & operator=(const LoopTree &) = default;
  LoopTree(LoopTree &&) = default;
  LoopTree & operator=(LoopTree &&) = default;
JSON_OBJECT_END(LoopTree)

JSON_OBJECT_BEGIN(FunctionTraits)
JSON_OBJECT_PAIR_3(FunctionTraits,
  Readonly, Analysis,
  NoReturn, Analysis,
  InOut, Analysis)

  FunctionTraits() :
    JSON_INIT(FunctionTraits, Analysis::No, Analysis::No, Analysis::No) {}
  ~FunctionTraits() = default;

  FunctionTraits(const FunctionTraits &) = default;
  FunctionTraits & operator=(const FunctionTraits &) = default;
  FunctionTraits(FunctionTraits &&) = default;
  FunctionTraits & operator=(FunctionTraits &&) = default;
JSON_OBJECT_END(FunctionTraits)

JSON_OBJECT_BEGIN(MainFuncInfo)
JSON_OBJECT_PAIR_4(MainFuncInfo,
  ID, unsigned,
  Name, std::string,
  Loops, std::vector<MainLoopInfo>,
  Traits, FunctionTraits)

  MainFuncInfo() = default;
  ~MainFuncInfo() = default;

  MainFuncInfo(const MainFuncInfo &) = default;
  MainFuncInfo & operator=(const MainFuncInfo &) = default;
  MainFuncInfo(MainFuncInfo &&) = default;
  MainFuncInfo & operator=(MainFuncInfo &&) = default;
JSON_OBJECT_END(MainFuncInfo)

JSON_OBJECT_BEGIN(FunctionList)
JSON_OBJECT_ROOT_PAIR_4(FunctionList,
  FuncID, unsigned,
  LoopID, unsigned,
  Attr, unsigned,
  Functions, std::vector<MainFuncInfo>)

  FunctionList() : JSON_INIT_ROOT {}
  ~FunctionList() override = default;

  FunctionList(const FunctionList &) = default;
  FunctionList & operator=(const FunctionList &) = default;
  FunctionList(FunctionList &&) = default;
  FunctionList & operator=(FunctionList &&) = default;
JSON_OBJECT_END(FunctionList)
}
}

JSON_DEFAULT_TRAITS(tsar::msg::, Statistic)
JSON_DEFAULT_TRAITS(tsar::msg::, Location)
JSON_DEFAULT_TRAITS(tsar::msg::, LoopTraits)
JSON_DEFAULT_TRAITS(tsar::msg::, MainLoopInfo)
JSON_DEFAULT_TRAITS(tsar::msg::, LoopTree)
JSON_DEFAULT_TRAITS(tsar::msg::, FunctionTraits)
JSON_DEFAULT_TRAITS(tsar::msg::, MainFuncInfo)
JSON_DEFAULT_TRAITS(tsar::msg::, FunctionList)

namespace json {
/// Specialization of JSON serialization traits for tsar::msg::LoopType type.
template<> struct Traits<tsar::msg::LoopType> {
  static bool parse(tsar::msg::LoopType &Dest, json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::msg::LoopType>(Lex.json())
        .Case("For", tsar::msg::LoopType::For)
        .Case("While", tsar::msg::LoopType::While)
        .Case("DoWhile", tsar::msg::LoopType::DoWhile)
        .Case("Implicit", tsar::msg::LoopType::Implicit)
        .Default(tsar::msg::LoopType::Invalid);
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, tsar::msg::LoopType Obj) {
    JSON += '"';
    switch (Obj) {
      case tsar::msg::LoopType::For: JSON += "For"; break;
      case tsar::msg::LoopType::While: JSON += "While"; break;
      case tsar::msg::LoopType::DoWhile: JSON += "DoWhile"; break;
      case tsar::msg::LoopType::Implicit: JSON += "Implicit"; break;
      default: JSON += "Invalid"; break;
    }
    JSON += '"';
  }
};
}

using ServerPrivateProvider = FunctionPassProvider<
  BasicAAWrapperPass,
  PrivateRecognitionPass,
  TransformationEnginePass,
  LoopMatcherPass,
  DFRegionInfoPass,
  ClangPerfectLoopPass,
  AAResultsWrapperPass>;

INITIALIZE_PROVIDER_BEGIN(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PROVIDER_END(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")

char PrivateServerPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)
INITIALIZE_PASS_DEPENDENCY(ServerPrivateProvider)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(InterprocAnalysisPass)
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

msg::MainLoopInfo getLoopInfo(clang::Stmt *Ptr, clang::SourceManager &SrcMgr) {
  auto LocStart = Ptr->getLocStart();
  auto LocEnd = Ptr->getLocEnd();
  msg::MainLoopInfo Loop;
  Loop[msg::MainLoopInfo::ID] = LocStart.getRawEncoding();
  Loop[msg::MainLoopInfo::Exit] = 0;
  if (isa<clang::ForStmt>(Ptr))
    Loop[msg::MainLoopInfo::Type] = msg::LoopType::For;
  else if (isa<clang::DoStmt>(Ptr))
    Loop[msg::MainLoopInfo::Type] = msg::LoopType::DoWhile;
  else if (isa<clang::WhileStmt>(Ptr))
    Loop[msg::MainLoopInfo::Type] = msg::LoopType::While;
  else if (isa<clang::LabelStmt>(Ptr))
    Loop[msg::MainLoopInfo::Type] = msg::LoopType::Implicit;
  else
    Loop[msg::MainLoopInfo::Type] = msg::LoopType::Invalid;
  assert(Loop[msg::MainLoopInfo::Type] != msg::LoopType::Invalid &&
    "Unknown loop type");
  Loop[msg::MainLoopInfo::StartLocation][msg::Location::Line] =
    SrcMgr.getExpansionLineNumber(LocStart);
  Loop[msg::MainLoopInfo::StartLocation][msg::Location::Column] =
    SrcMgr.getExpansionColumnNumber(LocStart);
  Loop[msg::MainLoopInfo::StartLocation][msg::Location::MacroLine] =
    SrcMgr.getSpellingLineNumber(LocStart);
  Loop[msg::MainLoopInfo::StartLocation][msg::Location::MacroColumn] =
    SrcMgr.getSpellingColumnNumber(LocStart);
  Loop[msg::MainLoopInfo::EndLocation][msg::Location::Line] =
    SrcMgr.getExpansionLineNumber(LocEnd);
  Loop[msg::MainLoopInfo::EndLocation][msg::Location::Column] =
    SrcMgr.getExpansionColumnNumber(LocEnd);
  Loop[msg::MainLoopInfo::EndLocation][msg::Location::MacroLine] =
    SrcMgr.getSpellingLineNumber(LocEnd);
  Loop[msg::MainLoopInfo::EndLocation][msg::Location::MacroColumn] =
    SrcMgr.getSpellingColumnNumber(LocEnd);
  return Loop;
}

std::string answerLoopTree(llvm::PrivateServerPass * const PSP,
    llvm::Module &M, tsar::TransformationContext *TfmCtx,
    msg::LoopTree LoopTree) {
  for (Function &F : M) {
    if (F.empty())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto FuncDecl = Decl->getAsFunction();
    if (FuncDecl->getLocStart().getRawEncoding() !=
        LoopTree[msg::LoopTree::ID])
      continue;
    typedef msg::MainLoopInfo LoopInfo;
    auto &SrcMgr = TfmCtx->getContext().getSourceManager();
    auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    auto &RegionInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto &PLoopInfo = Provider.get<ClangPerfectLoopPass>().
      getPerfectLoopInfo();
    auto &IALoopInfo = PSP->getAnalysis<InterprocAnalysisPass>().
      getInterprocAnalysisLoopInfo();
    auto &IAFuncInfo = PSP->getAnalysis<InterprocAnalysisPass>().
      getInterprocAnalysisFuncInfo();
    for (auto &Match : Matcher) {
      auto Loop = getLoopInfo(Match.get<AST>(), SrcMgr);
      auto &LT = Loop[msg::MainLoopInfo::Traits];
      LT[msg::LoopTraits::IsAnalyzed] = msg::Analysis::Yes;
      if (PLoopInfo.count(RegionInfo.getRegionFor(Match.get<IR>())))
        LT[msg::LoopTraits::Perfect] = msg::Analysis::Yes;
      auto &IEI = IALoopInfo.find(Match.first)->second;
      if (IEI.hasAttr(tsar::InterprocElemInfo::Attr::InOutFunc))
        LT[msg::LoopTraits::InOut] = msg::Analysis::Yes;
      for (auto BB : Match.get<IR>()->blocks())
        if (Match.get<IR>()->isLoopExiting(BB))
          Loop[msg::MainLoopInfo::Exit]++;
      auto &CalleeFuncLoc = IEI.getCalleeFuncLoc();
      for (auto CFL : CalleeFuncLoc)
        if (IAFuncInfo.find(CFL.first)->second.
            hasAttr(tsar::InterprocElemInfo::Attr::NoReturn))
          Loop[msg::MainLoopInfo::Exit] += CFL.second.size();
      LoopTree[msg::LoopTree::Loops].push_back(std::move(Loop));
    }
    for (auto &Unmatch : Unmatcher) {
      auto Loop = getLoopInfo(Unmatch, SrcMgr);
      auto &LT = Loop[msg::MainLoopInfo::Traits];
      auto &IEI = IALoopInfo.find(Unmatch)->second;
      if (IEI.hasAttr(tsar::InterprocElemInfo::Attr::InOutFunc))
        LT[msg::LoopTraits::InOut] = msg::Analysis::Yes;
      auto &CalleeFuncLoc = IEI.getCalleeFuncLoc();
      for (auto CFL : CalleeFuncLoc)
        if (IAFuncInfo.find(CFL.first)->second.
            hasAttr(tsar::InterprocElemInfo::Attr::NoReturn))
          Loop[msg::MainLoopInfo::Exit] += CFL.second.size();
      LoopTree[msg::LoopTree::Loops].push_back(std::move(Loop));
    }
    std::sort(LoopTree[msg::LoopTree::Loops].begin(),
      LoopTree[msg::LoopTree::Loops].end(),
      [](msg::MainLoopInfo &LHS,
         msg::MainLoopInfo &RHS) -> bool {
        return
          (LHS[LoopInfo::StartLocation][msg::Location::Line] <
              RHS[LoopInfo::StartLocation][msg::Location::Line]) ||
          ((LHS[LoopInfo::StartLocation][msg::Location::Line] ==
              RHS[LoopInfo::StartLocation][msg::Location::Line]) &&
          (LHS[LoopInfo::StartLocation][msg::Location::Column] <
              RHS[LoopInfo::StartLocation][msg::Location::Column])) ||
          ((LHS[LoopInfo::StartLocation][msg::Location::Line] ==
              RHS[LoopInfo::StartLocation][msg::Location::Line]) &&
          (LHS[LoopInfo::StartLocation][msg::Location::Column] ==
              RHS[LoopInfo::StartLocation][msg::Location::Column]) &&
          (LHS[LoopInfo::StartLocation][msg::Location::MacroLine] <
              RHS[LoopInfo::StartLocation][msg::Location::MacroLine])) ||
          ((LHS[LoopInfo::StartLocation][msg::Location::Line] ==
              RHS[LoopInfo::StartLocation][msg::Location::Line]) &&
          (LHS[LoopInfo::StartLocation][msg::Location::Column] ==
              RHS[LoopInfo::StartLocation][msg::Location::Column]) &&
          (LHS[LoopInfo::StartLocation][msg::Location::MacroLine] ==
              RHS[LoopInfo::StartLocation][msg::Location::MacroLine]) &&
          (LHS[LoopInfo::StartLocation][msg::Location::MacroColumn] <
              RHS[LoopInfo::StartLocation][msg::Location::MacroColumn]));
    });
    std::vector<std::pair<std::pair<unsigned, unsigned>,
        std::pair<unsigned, unsigned>>> Levels;
    for (auto &Loop : LoopTree[msg::LoopTree::Loops]) {
      while (!Levels.empty() &&
          ((Levels[Levels.size() - 1].first.first <
              Loop[LoopInfo::EndLocation][msg::Location::Line]) ||
          ((Levels[Levels.size() - 1].first.first ==
              Loop[LoopInfo::EndLocation][msg::Location::Line]) &&
          (Levels[Levels.size() - 1].first.second <
              Loop[LoopInfo::EndLocation][msg::Location::Column])) ||
          ((Levels[Levels.size() - 1].first.first ==
              Loop[LoopInfo::EndLocation][msg::Location::Line]) &&
          (Levels[Levels.size() - 1].first.second ==
              Loop[LoopInfo::EndLocation][msg::Location::Column]) &&
          (Levels[Levels.size() - 1].second.first <
              Loop[LoopInfo::EndLocation][msg::Location::MacroLine])) ||
          ((Levels[Levels.size() - 1].first.first ==
              Loop[LoopInfo::EndLocation][msg::Location::Line]) &&
          (Levels[Levels.size() - 1].first.second ==
              Loop[LoopInfo::EndLocation][msg::Location::Column]) &&
          (Levels[Levels.size() - 1].second.first ==
              Loop[LoopInfo::EndLocation][msg::Location::MacroLine]) &&
          (Levels[Levels.size() - 1].second.second <
              Loop[LoopInfo::EndLocation][msg::Location::MacroColumn]))))
        Levels.pop_back();
      Loop[msg::MainLoopInfo::Level] = Levels.size() + 1;
      Levels.emplace_back(
        std::piecewise_construct,
        std::forward_as_tuple(
          Loop[LoopInfo::EndLocation][msg::Location::Line],
          Loop[LoopInfo::EndLocation][msg::Location::Column]),
        std::forward_as_tuple(
          Loop[LoopInfo::EndLocation][msg::Location::MacroLine],
          Loop[LoopInfo::EndLocation][msg::Location::MacroColumn]));
    }
    break;
  }
  return json::Parser<msg::LoopTree>::unparseAsObject(LoopTree);
}

std::string answerFunctionList(llvm::PrivateServerPass * const PSP,
    llvm::Module &M, tsar::TransformationContext *TfmCtx,
    msg::FunctionList FuncList) {
  typedef msg::MainFuncInfo FuncInfo;
  if (FuncList[msg::FunctionList::FuncID]) {
    for (Function &F : M) {
      if (F.empty())
        continue;
      auto Decl = TfmCtx->getDeclForMangledName(F.getName());
      if (!Decl)
        continue;
      auto FuncDecl = Decl->getAsFunction();
      if (FuncDecl->getLocStart().getRawEncoding() !=
        FuncList[msg::FunctionList::FuncID])
        continue;
      auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
      auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
      auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
      auto &IALoopInfo = PSP->getAnalysis<InterprocAnalysisPass>().
        getInterprocAnalysisLoopInfo();
      auto &IAFuncInfo = PSP->getAnalysis<InterprocAnalysisPass>().
        getInterprocAnalysisFuncInfo();
      tsar::InterprocElemInfo IEI;
      if (FuncList[msg::FunctionList::LoopID]) {
        for (auto Match : Matcher)
          if (Match.first->getLocStart().getRawEncoding() ==
            FuncList[msg::FunctionList::LoopID])
            IEI = IALoopInfo.find(Match.first)->second;
        for (auto Unmatch : Unmatcher)
          if (Unmatch->getLocStart().getRawEncoding() ==
            FuncList[msg::FunctionList::LoopID])
            IEI = IALoopInfo.find(Unmatch)->second;
      }
      else {
        IEI = IAFuncInfo.find(&F)->second;
      }
      for (auto MapCF : IEI.getCalleeFuncLoc()) {
        for (unsigned IdxAttr = 1;
          IdxAttr < (unsigned)InterprocElemInfo::Attr::EndAttr; IdxAttr++)
          if ((FuncList[msg::FunctionList::Attr] == IdxAttr) &&
            IAFuncInfo.find(MapCF.first)->second.
            hasAttr((InterprocElemInfo::Attr)IdxAttr)) {
            msg::MainFuncInfo Func;
            Func[FuncInfo::Name] = MapCF.first->getName();
            FuncList[msg::FunctionList::Functions].push_back(std::move(Func));
          }
      }
    }
  } else {
    for (Function &F : M) {
      if (F.empty())
        continue;
      auto Decl = TfmCtx->getDeclForMangledName(F.getName());
      if (!Decl)
        continue;
      auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
      auto &AA = Provider.get<AAResultsWrapperPass>().getAAResults();
      auto FuncDecl = Decl->getAsFunction();
      auto &IEI = PSP->getAnalysis<InterprocAnalysisPass>().
        getInterprocAnalysisFuncInfo().find(&F)->second;
      msg::MainFuncInfo Func;
      Func[FuncInfo::Name] = F.getName();
      Func[FuncInfo::ID] = FuncDecl->getLocStart().getRawEncoding();
      if (AA.onlyReadsMemory(&F) && F.hasFnAttribute(Attribute::NoUnwind))
        Func[FuncInfo::Traits][msg::FunctionTraits::Readonly]
          = msg::Analysis::Yes;
      if (IEI.hasAttr(tsar::InterprocElemInfo::Attr::NoReturn))
        Func[FuncInfo::Traits][msg::FunctionTraits::NoReturn]
          = msg::Analysis::Yes;
      if (IEI.hasAttr(tsar::InterprocElemInfo::Attr::InOutFunc))
        Func[FuncInfo::Traits][msg::FunctionTraits::InOut]
          = msg::Analysis::Yes;
      FuncList[msg::FunctionList::Functions].push_back(std::move(Func));
    }
  }
  return json::Parser<msg::FunctionList>::unparseAsObject(FuncList);
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
  while (mConnection->answer(
      [this, &M, &TfmCtx](const std::string &Request) -> std::string {
    msg::Diagnostic Diag(msg::Status::Error);
    if (mStdErr->isDiff()) {
      Diag[msg::Diagnostic::Terminal] += mStdErr->diff();
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    json::Parser<msg::Statistic, msg::LoopTree, msg::FunctionList> P(Request);
    auto Obj = P.parse();
    assert(Obj && "Invalid request!");
    if (Obj->is<msg::Statistic>())
      return answerStatistic(this, M, TfmCtx);
    if (Obj->is<msg::LoopTree>())
      return answerLoopTree(this, M, TfmCtx, Obj->as<msg::LoopTree>());
    if (Obj->is<msg::FunctionList>())
      return answerFunctionList(this, M, TfmCtx, Obj->as<msg::FunctionList>());
    llvm_unreachable("Unknown request to server!");
  }));
  return false;
}

void PrivateServerPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ServerPrivateProvider>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<InterprocAnalysisPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createPrivateServerPass(
    bcl::IntrusiveConnection &IC, bcl::RedirectIO &StdErr) {
  return new PrivateServerPass(IC, StdErr);
}
