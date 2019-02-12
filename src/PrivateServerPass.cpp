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
#include "Attributes.h"
#include "CalleeProcLocation.h"
#include "CanonicalLoop.h"
#include "EstimateMemory.h"
#include "InterprocAttr.h"
#include "tsar_loop_matcher.h"
#include "tsar_memory_matcher.h"
#include "Messages.h"
#include "tsar_pass_provider.h"
#include "PerfectLoop.h"
#include "tsar_private.h"
#include "tsar_transformation.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/RedirectIO.h>
#include <clang/AST/ASTContext.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
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
JSON_OBJECT_PAIR_4(LoopTraits,
  IsAnalyzed, Analysis,
  Perfect, Analysis,
  InOut, Analysis,
  Canonical, Analysis)

  LoopTraits() :
    JSON_INIT(LoopTraits,
      Analysis::No, Analysis::No, Analysis::No, Analysis::No) {}
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
JSON_OBJECT_PAIR_4(FunctionTraits,
  Readonly, Analysis,
  NoReturn, Analysis,
  InOut, Analysis,
  Loops, Analysis)

  FunctionTraits() :
    JSON_INIT(FunctionTraits,
      Analysis::No, Analysis::No, Analysis::No, Analysis::No) {}
  ~FunctionTraits() = default;

  FunctionTraits(const FunctionTraits &) = default;
  FunctionTraits & operator=(const FunctionTraits &) = default;
  FunctionTraits(FunctionTraits &&) = default;
  FunctionTraits & operator=(FunctionTraits &&) = default;
JSON_OBJECT_END(FunctionTraits)

JSON_OBJECT_BEGIN(MainFuncInfo)
JSON_OBJECT_PAIR_6(MainFuncInfo,
  ID, unsigned,
  Name, std::string,
  StartLocation, Location,
  EndLocation, Location,
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
JSON_OBJECT_ROOT_PAIR(FunctionList,
  Functions, std::vector<MainFuncInfo>)

  FunctionList() : JSON_INIT_ROOT {}
  ~FunctionList() override = default;

  FunctionList(const FunctionList &) = default;
  FunctionList & operator=(const FunctionList &) = default;
  FunctionList(FunctionList &&) = default;
  FunctionList & operator=(FunctionList &&) = default;
JSON_OBJECT_END(FunctionList)

JSON_OBJECT_BEGIN(CalleeFuncInfo)
JSON_OBJECT_PAIR_3(CalleeFuncInfo,
  ID, std::string,
  Name, std::string,
  Locations, std::vector<Location>)

  CalleeFuncInfo() = default;
  ~CalleeFuncInfo() = default;

  CalleeFuncInfo(const CalleeFuncInfo &) = default;
  CalleeFuncInfo & operator=(const CalleeFuncInfo &) = default;
  CalleeFuncInfo(CalleeFuncInfo &&) = default;
  CalleeFuncInfo & operator=(CalleeFuncInfo &&) = default;
JSON_OBJECT_END(CalleeFuncInfo)

JSON_OBJECT_BEGIN(CalleeFuncList)
JSON_OBJECT_ROOT_PAIR_4(CalleeFuncList,
  FuncID, unsigned,
  LoopID, unsigned,
  Attr, unsigned,
  Functions, std::vector<CalleeFuncInfo>)

  CalleeFuncList() : JSON_INIT_ROOT {}
  ~CalleeFuncList() override = default;

  CalleeFuncList(const CalleeFuncList &) = default;
  CalleeFuncList & operator=(const CalleeFuncList &) = default;
  CalleeFuncList(CalleeFuncList &&) = default;
  CalleeFuncList & operator=(CalleeFuncList &&) = default;
JSON_OBJECT_END(CalleeFuncList)
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
JSON_DEFAULT_TRAITS(tsar::msg::, CalleeFuncInfo)
JSON_DEFAULT_TRAITS(tsar::msg::, CalleeFuncList)

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
  CanonicalLoopPass,
  MemoryMatcherImmutableWrapper,
  LoopAttributesDeductionPass,
  AAResultsWrapperPass>;

INITIALIZE_PROVIDER_BEGIN(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopAttributesDeductionPass)
INITIALIZE_PROVIDER_END(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")

char PrivateServerPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)
INITIALIZE_PASS_DEPENDENCY(ServerPrivateProvider)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(CalleeProcLocationPass)
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
  auto &SrcMgr = TfmCtx->getContext().getSourceManager();
  for (Function &F : M) {
    if (F.empty())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    if (SrcMgr.getFileCharacteristic(Decl->getLocStart())
        != clang::SrcMgr::C_User)
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

msg::Location getLocation(
    clang::SourceLocation &SLoc, clang::SourceManager &SrcMgr) {
  msg::Location MsgLoc;
  MsgLoc[msg::Location::Line] = SrcMgr.getExpansionLineNumber(SLoc);
  MsgLoc[msg::Location::Column] = SrcMgr.getExpansionColumnNumber(SLoc);
  MsgLoc[msg::Location::MacroLine] = SrcMgr.getSpellingLineNumber(SLoc);
  MsgLoc[msg::Location::MacroColumn] = SrcMgr.getSpellingColumnNumber(SLoc);
  return MsgLoc;
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
  Loop[msg::MainLoopInfo::StartLocation] = getLocation(LocStart, SrcMgr);
  Loop[msg::MainLoopInfo::EndLocation] = getLocation(LocEnd, SrcMgr);
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
    auto &CLoopInfo = Provider.get<CanonicalLoopPass>().
      getCanonicalLoopInfo();
    auto &IALoopInfo = Provider.get<LoopAttributesDeductionPass>();
    auto &LoopCPInfo = PSP->getAnalysis<CalleeProcLocationPass>().
      getLoopCalleeProcInfo();
    auto &FuncCPInfo = PSP->getAnalysis<CalleeProcLocationPass>().
      getFuncCalleeProcInfo();
    for (auto &Match : Matcher) {
      auto Loop = getLoopInfo(Match.get<AST>(), SrcMgr);
      auto &LT = Loop[msg::MainLoopInfo::Traits];
      LT[msg::LoopTraits::IsAnalyzed] = msg::Analysis::Yes;
      auto CI = CLoopInfo.find_as(RegionInfo.getRegionFor(Match.get<IR>()));
      if (CI != CLoopInfo.end() && (**CI).isCanonical())
        LT[msg::LoopTraits::Canonical] = msg::Analysis::Yes;
      if (PLoopInfo.count(RegionInfo.getRegionFor(Match.get<IR>())))
        LT[msg::LoopTraits::Perfect] = msg::Analysis::Yes;
      auto &CPL = LoopCPInfo.find(Match.first)->second;
      if (!IALoopInfo.hasAttr(*Match.get<IR>(), AttrKind::NoIO))
        LT[msg::LoopTraits::InOut] = msg::Analysis::Yes;
      for (auto BB : Match.get<IR>()->blocks())
        if (Match.get<IR>()->isLoopExiting(BB))
          Loop[msg::MainLoopInfo::Exit]++;
      auto &CalleeFuncLoc = CPL.getCalleeFuncLoc();
      for (auto CFL : CalleeFuncLoc)
        if (!hasFnAttr(*CFL.first, AttrKind::AlwaysReturn))
          Loop[msg::MainLoopInfo::Exit] += CFL.second.size();
      LoopTree[msg::LoopTree::Loops].push_back(std::move(Loop));
    }
    for (auto &Unmatch : Unmatcher) {
      auto Loop = getLoopInfo(Unmatch, SrcMgr);
      auto &LT = Loop[msg::MainLoopInfo::Traits];
      auto &CPL = LoopCPInfo.find(Unmatch)->second;
      LT[msg::LoopTraits::InOut] = msg::Analysis::Yes;
      auto &CalleeFuncLoc = CPL.getCalleeFuncLoc();
      for (auto CFL : CalleeFuncLoc)
        if (!hasFnAttr(*CFL.first, AttrKind::AlwaysReturn))
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
    llvm::Module &M, tsar::TransformationContext *TfmCtx) {
  msg::FunctionList FuncList;
  for (Function &F : M) {
    if (F.empty())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto &SrcMgr = TfmCtx->getContext().getSourceManager();
    if (SrcMgr.getFileCharacteristic(Decl->getLocStart())
        != clang::SrcMgr::C_User)
      continue;
    auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
    auto &LMP = Provider.get<LoopMatcherPass>();
    auto &AA = Provider.get<AAResultsWrapperPass>().getAAResults();
    auto FuncDecl = Decl->getAsFunction();
    msg::MainFuncInfo Func;
    Func[msg::MainFuncInfo::Name] = F.getName();
    Func[msg::MainFuncInfo::ID] = FuncDecl->getLocStart().getRawEncoding();
    Func[msg::MainFuncInfo::StartLocation] =
        getLocation(FuncDecl->getLocStart(), SrcMgr);
    Func[msg::MainFuncInfo::EndLocation] =
        getLocation(FuncDecl->getLocEnd(), SrcMgr);
    if (AA.onlyReadsMemory(&F) && F.hasFnAttribute(Attribute::NoUnwind))
      Func[msg::MainFuncInfo::Traits][msg::FunctionTraits::Readonly]
        = msg::Analysis::Yes;
    if (!hasFnAttr(F, AttrKind::AlwaysReturn))
      Func[msg::MainFuncInfo::Traits][msg::FunctionTraits::NoReturn]
        = msg::Analysis::Yes;
    if (!hasFnAttr(F, AttrKind::NoIO))
      Func[msg::MainFuncInfo::Traits][msg::FunctionTraits::InOut]
        = msg::Analysis::Yes;
    if (!LMP.getMatcher().empty() || !LMP.getUnmatchedAST().empty())
      Func[msg::MainFuncInfo::Traits][msg::FunctionTraits::Loops]
        = msg::Analysis::Yes;
    FuncList[msg::FunctionList::Functions].push_back(std::move(Func));
  }
  return json::Parser<msg::FunctionList>::unparseAsObject(FuncList);
}

std::string answerCalleeFuncList(llvm::PrivateServerPass * const PSP,
    llvm::Module &M, tsar::TransformationContext *TfmCtx,
    msg::CalleeFuncList CalleeFuncList) {
  for (Function &F : M) {
    if (F.empty())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto FuncDecl = Decl->getAsFunction();
    if (FuncDecl->getLocStart().getRawEncoding() !=
        CalleeFuncList[msg::CalleeFuncList::FuncID])
      continue;
    auto &SrcMgr = TfmCtx->getContext().getSourceManager();
    auto &Provider = PSP->getAnalysis<ServerPrivateProvider>(F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    auto &LoopCPInfo = PSP->getAnalysis<CalleeProcLocationPass>().
        getLoopCalleeProcInfo();
    auto &FuncCPInfo = PSP->getAnalysis<CalleeProcLocationPass>().
        getFuncCalleeProcInfo();
    tsar::CalleeProcLocation CPL;
    if (CalleeFuncList[msg::CalleeFuncList::LoopID]) {
      for (auto Match : Matcher)
        if (Match.first->getLocStart().getRawEncoding() ==
            CalleeFuncList[msg::CalleeFuncList::LoopID]) {
          CPL = LoopCPInfo.find(Match.first)->second;
        }
      for (auto Unmatch : Unmatcher)
        if (Unmatch->getLocStart().getRawEncoding() ==
            CalleeFuncList[msg::CalleeFuncList::LoopID]) {
          CPL = LoopCPInfo.find(Unmatch)->second;
        }
      if (CalleeFuncList[msg::CalleeFuncList::Attr] == 2) {
        if (!CPL.getBreak().empty()) {
          msg::CalleeFuncInfo Func;
          Func[msg::CalleeFuncInfo::Name] = "break";
          for (auto Loc : CPL.getBreak())
            Func[msg::CalleeFuncInfo::Locations].
                push_back(getLocation(Loc, SrcMgr));
          CalleeFuncList[msg::CalleeFuncList::Functions].
              push_back(std::move(Func));
        }
        if (!CPL.getReturn().empty()) {
          msg::CalleeFuncInfo Func;
          Func[msg::CalleeFuncInfo::Name] = "return";
          for (auto Loc : CPL.getReturn())
            Func[msg::CalleeFuncInfo::Locations].
                push_back(getLocation(Loc, SrcMgr));
          CalleeFuncList[msg::CalleeFuncList::Functions].
              push_back(std::move(Func));
        }
        if (!CPL.getGoto().empty()) {
          msg::CalleeFuncInfo Func;
          Func[msg::CalleeFuncInfo::Name] = "goto";
          for (auto Loc : CPL.getGoto())
            Func[msg::CalleeFuncInfo::Locations].
                push_back(getLocation(Loc, SrcMgr));
          CalleeFuncList[msg::CalleeFuncList::Functions].
              push_back(std::move(Func));
        }
      }
    } else {
      CPL = FuncCPInfo.find(&F)->second;
    }
    for (auto &MapCF : CPL.getCalleeFuncLoc()) {
      for_each_attr([&SrcMgr, &MapCF, &CalleeFuncList](AttrKind Kind) {
        if ((CalleeFuncList[msg::CalleeFuncList::Attr] == (unsigned)Kind) &&
            hasFnAttr(*MapCF.first, Kind)) {
          msg::CalleeFuncInfo Func;
          Func[msg::CalleeFuncInfo::ID] = std::to_string(
            MapCF.second[0].getRawEncoding());
          Func[msg::CalleeFuncInfo::Name] = MapCF.first->getName();
          for (auto Loc : MapCF.second)
            Func[msg::CalleeFuncInfo::Locations].
              push_back(getLocation(Loc, SrcMgr));
          CalleeFuncList[msg::CalleeFuncList::Functions].
            push_back(std::move(Func));
        }
      });
    }
  }
  return json::Parser<msg::CalleeFuncList>::unparseAsObject(CalleeFuncList);
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
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  ServerPrivateProvider::initialize<MemoryMatcherImmutableWrapper>(
      [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
    Wrapper.set(*MMWrapper);
  });
  while (mConnection->answer(
      [this, &M, &TfmCtx](const std::string &Request) -> std::string {
    msg::Diagnostic Diag(msg::Status::Error);
    if (mStdErr->isDiff()) {
      Diag[msg::Diagnostic::Terminal] += mStdErr->diff();
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    json::Parser<msg::Statistic, msg::LoopTree,
      msg::FunctionList, msg::CalleeFuncList> P(Request);
    auto Obj = P.parse();
    assert(Obj && "Invalid request!");
    if (Obj->is<msg::Statistic>())
      return answerStatistic(this, M, TfmCtx);
    if (Obj->is<msg::LoopTree>())
      return answerLoopTree(this, M, TfmCtx, Obj->as<msg::LoopTree>());
    if (Obj->is<msg::FunctionList>())
      return answerFunctionList(this, M, TfmCtx);
    if (Obj->is<msg::CalleeFuncList>())
      return answerCalleeFuncList(
        this, M, TfmCtx, Obj->as<msg::CalleeFuncList>());
    llvm_unreachable("Unknown request to server!");
  }));
  return false;
}

void PrivateServerPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ServerPrivateProvider>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CalleeProcLocationPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createPrivateServerPass(
    bcl::IntrusiveConnection &IC, bcl::RedirectIO &StdErr) {
  return new PrivateServerPass(IC, StdErr);
}
