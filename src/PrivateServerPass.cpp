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
// results of loop traits analysis.
//
//===----------------------------------------------------------------------===//

#include "Attributes.h"
#include "CanonicalLoop.h"
#include "ClangMessages.h"
#include "ControlFlowTraits.h"
#include "EstimateMemory.h"
#include "InterprocAttr.h"
#include "tsar_loop_matcher.h"
#include "tsar_memory_matcher.h"
#include "tsar_pass.h"
#include "tsar_pass_provider.h"
#include "PerfectLoop.h"
#include "tsar_private.h"
#include "tsar_transformation.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/RedirectIO.h>
#include <bcl/utility.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Pass.h>
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

JSON_OBJECT_BEGIN(LoopTraits)
JSON_OBJECT_PAIR_5(LoopTraits,
  IsAnalyzed, Analysis,
  Perfect, Analysis,
  InOut, Analysis,
  Canonical, Analysis,
  UnsafeCFG, Analysis)

  LoopTraits() :
    JSON_INIT(LoopTraits,
      Analysis::No, Analysis::No, Analysis::Yes, Analysis::No, Analysis::Yes) {}
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

JSON_OBJECT_BEGIN(Loop)
JSON_OBJECT_PAIR_7(Loop,
  ID, unsigned,
  StartLocation, Location,
  EndLocation, Location,
  Traits, LoopTraits,
  Exit, Optional<unsigned>,
  Level, unsigned,
  Type, LoopType)

  Loop() = default;
  ~Loop() = default;

  Loop(const Loop &) = default;
  Loop & operator=(const Loop &) = default;
  Loop(Loop &&) = default;
  Loop & operator=(Loop &&) = default;
JSON_OBJECT_END(Loop)

JSON_OBJECT_BEGIN(LoopTree)
JSON_OBJECT_ROOT_PAIR_2(LoopTree,
  FunctionID, unsigned,
  Loops, std::vector<Loop>)

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
  UnsafeCFG, Analysis,
  InOut, Analysis,
  Loops, Analysis)

  FunctionTraits() :
    JSON_INIT(FunctionTraits,
      Analysis::No, Analysis::Yes, Analysis::Yes, Analysis::No) {}
  ~FunctionTraits() = default;

  FunctionTraits(const FunctionTraits &) = default;
  FunctionTraits & operator=(const FunctionTraits &) = default;
  FunctionTraits(FunctionTraits &&) = default;
  FunctionTraits & operator=(FunctionTraits &&) = default;
JSON_OBJECT_END(FunctionTraits)

JSON_OBJECT_BEGIN(Function)
JSON_OBJECT_PAIR_7(Function,
  ID, unsigned,
  Name, std::string,
  StartLocation, Location,
  EndLocation, Location,
  Loops, std::vector<Loop>,
  Exit, llvm::Optional<unsigned>,
  Traits, FunctionTraits)

  Function() = default;
  ~Function() = default;

  Function(const Function &) = default;
  Function & operator=(const Function &) = default;
  Function(Function &&) = default;
  Function & operator=(Function &&) = default;
JSON_OBJECT_END(Function)

JSON_OBJECT_BEGIN(FunctionList)
JSON_OBJECT_ROOT_PAIR(FunctionList,
  Functions, std::vector<Function>)

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
  Attr, CFFlags,
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
JSON_DEFAULT_TRAITS(tsar::msg::, LoopTraits)
JSON_DEFAULT_TRAITS(tsar::msg::, Loop)
JSON_DEFAULT_TRAITS(tsar::msg::, LoopTree)
JSON_DEFAULT_TRAITS(tsar::msg::, FunctionTraits)
JSON_DEFAULT_TRAITS(tsar::msg::, Function)
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

template<> struct Traits<CFFlags> {
  static bool parse(CFFlags &Dest, json::Lexer &Lex) {
    Position MaxIdx, Count;
    bool Ok;
    std::tie(Count, MaxIdx, Ok) = Parser<>::numberOfKeys(Lex);
    if (!Ok)
      return false;
    Dest = CFFlags::DefaultFlags;
    return Parser<>::traverse<Traits<CFFlags>>(Dest, Lex);
  }
  static bool parse(CFFlags &Dest, json::Lexer &Lex,
      std::pair<Position, Position> Key) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest |= llvm::StringSwitch<CFFlags>(S)
        .Case("Entry", CFFlags::Entry)
        .Case("Exit", CFFlags::Exit)
        .Case("InOut", CFFlags::InOut)
        .Case("MayNoReturn", CFFlags::MayNoReturn)
        .Case("MayUnwind", CFFlags::MayUnwind)
        .Case("MayReturnTwice", CFFlags::MayReturnTwice)
        .Case("UnsafeCFG", CFFlags::UnsafeCFG)
        .Default(CFFlags::DefaultFlags);
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, CFFlags Obj) {
    JSON += '[';
    if (Obj & CFFlags::Entry)
      JSON += R"("Entry",)";
    if (Obj & CFFlags::Exit)
      JSON += R"("Exit",)";
    if (Obj & CFFlags::InOut)
      JSON += R"("InOut",)";
    if (Obj & CFFlags::MayNoReturn)
      JSON += R"("MayNoReturn",)";
    if (Obj & CFFlags::MayReturnTwice)
      JSON += R"("MayReturnTwice",)";
    if (Obj & CFFlags::MayUnwind)
      JSON += R"("MayUnwind",)";
    if (Obj & CFFlags::UnsafeCFG)
      JSON += R"("UnsafeCFG",)";
    if (JSON.back() != '[')
      JSON.back() = ']';
    else
      JSON += ']';
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
  ClangCFTraitsPass,
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
INITIALIZE_PASS_DEPENDENCY(ClangCFTraitsPass)
INITIALIZE_PROVIDER_END(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")

namespace {
/// Interacts with a client and sends result of analysis on request.
class PrivateServerPass :
  public ModulePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  PrivateServerPass() : ModulePass(ID), mConnection(nullptr) {
    initializePrivateServerPassPass(*PassRegistry::getPassRegistry());
  }

  /// Constructor.
  explicit PrivateServerPass(bcl::IntrusiveConnection &IC,
      bcl::RedirectIO &StdErr) :
    ModulePass(ID), mConnection(&IC), mStdErr(&StdErr) {
    initializePrivateServerPassPass(*PassRegistry::getPassRegistry());
  }

  /// Interacts with a client and sends result of analysis on request.
  bool runOnModule(llvm::Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::string answerStatistic(llvm::Module &M);
  std::string answerFunctionList(llvm::Module &M);
  std::string answerLoopTree(llvm::Module &M, const msg::LoopTree &Request);
  std::string answerCalleeFuncList(llvm::Module &M,
    const msg::CalleeFuncList &Request);

  bcl::IntrusiveConnection *mConnection;
  bcl::RedirectIO *mStdErr;

  TransformationContext *mTfmCtx  = nullptr;
};

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

msg::Loop getLoopInfo(clang::Stmt *S, clang::SourceManager &SrcMgr) {
  assert(S && "Statement must not be null!");
  auto LocStart = S->getLocStart();
  auto LocEnd = S->getLocEnd();
  msg::Loop Loop;
  Loop[msg::Loop::ID] = LocStart.getRawEncoding();
  if (isa<clang::ForStmt>(S))
    Loop[msg::Loop::Type] = msg::LoopType::For;
  else if (isa<clang::DoStmt>(S))
    Loop[msg::Loop::Type] = msg::LoopType::DoWhile;
  else if (isa<clang::WhileStmt>(S))
    Loop[msg::Loop::Type] = msg::LoopType::While;
  else if (isa<clang::LabelStmt>(S))
    Loop[msg::Loop::Type] = msg::LoopType::Implicit;
  else
    Loop[msg::Loop::Type] = msg::LoopType::Invalid;
  assert(Loop[msg::Loop::Type] != msg::LoopType::Invalid &&
    "Unknown loop type!");
  Loop[msg::Loop::StartLocation] = getLocation(LocStart, SrcMgr);
  Loop[msg::Loop::EndLocation] = getLocation(LocEnd, SrcMgr);
  return Loop;
}
}

char PrivateServerPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)
INITIALIZE_PASS_DEPENDENCY(ServerPrivateProvider)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)

std::string PrivateServerPass::answerStatistic(llvm::Module &M) {
  msg::Statistic Stat;
  auto &Rewriter = mTfmCtx->getRewriter();
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
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  for (Function &F : M) {
    auto Decl = mTfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    if (SrcMgr.getFileCharacteristic(Decl->getLocStart())
        != clang::SrcMgr::C_User)
      continue;
    ++Stat[msg::Statistic::Functions];
    // Analysis are not available for functions without body.
    if (F.isDeclaration())
      continue;
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
}

std::string PrivateServerPass::answerLoopTree(llvm::Module &M,
    const msg::LoopTree &Request) {
  for (Function &F : M) {
    auto Decl = mTfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto FuncDecl = Decl->getAsFunction();
    if (FuncDecl->getLocStart().getRawEncoding() !=
        Request[msg::LoopTree::FunctionID])
      continue;
    if (F.isDeclaration())
      return json::Parser<msg::LoopTree>::unparseAsObject(Request);
    msg::LoopTree LoopTree;
    LoopTree[msg::LoopTree::FunctionID] = Request[msg::LoopTree::FunctionID];
    auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
    auto &Provider = getAnalysis<ServerPrivateProvider>(F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    auto &RegionInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto &PerfectInfo = Provider.get<ClangPerfectLoopPass>().
      getPerfectLoopInfo();
    auto &CanonicalInfo = Provider.get<CanonicalLoopPass>().
      getCanonicalLoopInfo();
    auto &AttrsInfo = Provider.get<LoopAttributesDeductionPass>();
    auto &CFLoopInfo = Provider.get<ClangCFTraitsPass>().getLoopInfo();
    for (auto &Match : Matcher) {
      auto Loop = getLoopInfo(Match.get<AST>(), SrcMgr);
      auto &LT = Loop[msg::Loop::Traits];
      LT[msg::LoopTraits::IsAnalyzed] = msg::Analysis::Yes;
      auto CI = CanonicalInfo.find_as(RegionInfo.getRegionFor(Match.get<IR>()));
      if (CI != CanonicalInfo.end() && (**CI).isCanonical())
        LT[msg::LoopTraits::Canonical] = msg::Analysis::Yes;
      if (PerfectInfo.count(RegionInfo.getRegionFor(Match.get<IR>())))
        LT[msg::LoopTraits::Perfect] = msg::Analysis::Yes;
      if (AttrsInfo.hasAttr(*Match.get<IR>(), AttrKind::NoIO))
        LT[msg::LoopTraits::InOut] = msg::Analysis::No;
      if (AttrsInfo.hasAttr(*Match.get<IR>(), AttrKind::AlwaysReturn) &&
          AttrsInfo.hasAttr(*Match.get<IR>(), Attribute::NoUnwind) &&
          !AttrsInfo.hasAttr(*Match.get<IR>(), Attribute::ReturnsTwice))
        LT[msg::LoopTraits::UnsafeCFG] = msg::Analysis::No;
      Loop[msg::Loop::Exit] = 0;
      for (auto *BB : Match.get<IR>()->blocks()) {
        if (Match.get<IR>()->isLoopExiting(BB))
          ++*Loop[msg::Loop::Exit];
      }
      LoopTree[msg::LoopTree::Loops].push_back(std::move(Loop));
    }
    for (auto &Unmatch : Unmatcher) {
      auto Loop = getLoopInfo(Unmatch, SrcMgr);
      auto &LT = Loop[msg::Loop::Traits];
      LT[msg::LoopTraits::IsAnalyzed] = msg::Analysis::No;
      LoopTree[msg::LoopTree::Loops].push_back(std::move(Loop));
    }
    std::sort(LoopTree[msg::LoopTree::Loops].begin(),
      LoopTree[msg::LoopTree::Loops].end(),
      [](msg::Loop &LHS, msg::Loop &RHS) -> bool {
        return
          (LHS[msg::Loop::StartLocation][msg::Location::Line] <
              RHS[msg::Loop::StartLocation][msg::Location::Line]) ||
          ((LHS[msg::Loop::StartLocation][msg::Location::Line] ==
              RHS[msg::Loop::StartLocation][msg::Location::Line]) &&
          (LHS[msg::Loop::StartLocation][msg::Location::Column] <
              RHS[msg::Loop::StartLocation][msg::Location::Column])) ||
          ((LHS[msg::Loop::StartLocation][msg::Location::Line] ==
              RHS[msg::Loop::StartLocation][msg::Location::Line]) &&
          (LHS[msg::Loop::StartLocation][msg::Location::Column] ==
              RHS[msg::Loop::StartLocation][msg::Location::Column]) &&
          (LHS[msg::Loop::StartLocation][msg::Location::MacroLine] <
              RHS[msg::Loop::StartLocation][msg::Location::MacroLine])) ||
          ((LHS[msg::Loop::StartLocation][msg::Location::Line] ==
              RHS[msg::Loop::StartLocation][msg::Location::Line]) &&
          (LHS[msg::Loop::StartLocation][msg::Location::Column] ==
              RHS[msg::Loop::StartLocation][msg::Location::Column]) &&
          (LHS[msg::Loop::StartLocation][msg::Location::MacroLine] ==
              RHS[msg::Loop::StartLocation][msg::Location::MacroLine]) &&
          (LHS[msg::Loop::StartLocation][msg::Location::MacroColumn] <
              RHS[msg::Loop::StartLocation][msg::Location::MacroColumn]));
    });
    std::vector<msg::Location> Levels;
    for (auto &Loop : LoopTree[msg::LoopTree::Loops]) {
      while (!Levels.empty() &&
          ((Levels[Levels.size() - 1][msg::Location::Line] <
              Loop[msg::Loop::EndLocation][msg::Location::Line]) ||
          ((Levels[Levels.size() - 1][msg::Location::Line] ==
              Loop[msg::Loop::EndLocation][msg::Location::Line]) &&
          (Levels[Levels.size() - 1][msg::Location::Column] <
              Loop[msg::Loop::EndLocation][msg::Location::Column])) ||
          ((Levels[Levels.size() - 1][msg::Location::Line] ==
              Loop[msg::Loop::EndLocation][msg::Location::Line]) &&
          (Levels[Levels.size() - 1][msg::Location::Column] ==
              Loop[msg::Loop::EndLocation][msg::Location::Column]) &&
          (Levels[Levels.size() - 1][msg::Location::MacroLine] <
              Loop[msg::Loop::EndLocation][msg::Location::MacroLine])) ||
          ((Levels[Levels.size() - 1][msg::Location::Line] ==
              Loop[msg::Loop::EndLocation][msg::Location::Line]) &&
          (Levels[Levels.size() - 1][msg::Location::Column] ==
              Loop[msg::Loop::EndLocation][msg::Location::Column]) &&
          (Levels[Levels.size() - 1][msg::Location::MacroLine] ==
              Loop[msg::Loop::EndLocation][msg::Location::MacroLine]) &&
          (Levels[Levels.size() - 1][msg::Location::MacroColumn] <
              Loop[msg::Loop::EndLocation][msg::Location::MacroColumn]))))
        Levels.pop_back();
      Loop[msg::Loop::Level] = Levels.size() + 1;
      Levels.push_back(Loop[msg::Loop::EndLocation]);
    }
    return json::Parser<msg::LoopTree>::unparseAsObject(LoopTree);
  }
  return json::Parser<msg::LoopTree>::unparseAsObject(Request);
}

std::string PrivateServerPass::answerFunctionList(llvm::Module &M) {
  msg::FunctionList FuncList;
  for (Function &F : M) {
    auto Decl = mTfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
    if (SrcMgr.getFileCharacteristic(Decl->getLocStart())
        != clang::SrcMgr::C_User)
      continue;
    auto FuncDecl = Decl->getAsFunction();
    assert(FuncDecl && "Function declaration must not be null!");
    msg::Function Func;
    Func[msg::Function::Name] = FuncDecl->getName();
    Func[msg::Function::ID] = FuncDecl->getLocStart().getRawEncoding();
    Func[msg::Function::StartLocation] =
        getLocation(FuncDecl->getLocStart(), SrcMgr);
    Func[msg::Function::EndLocation] =
        getLocation(FuncDecl->getLocEnd(), SrcMgr);
    if (hasFnAttr(F, AttrKind::AlwaysReturn) ||
        F.hasFnAttribute(Attribute::NoUnwind) ||
        !F.hasFnAttribute(Attribute::ReturnsTwice))
      Func[msg::Function::Traits][msg::FunctionTraits::UnsafeCFG]
        = msg::Analysis::No;
    if (hasFnAttr(F, AttrKind::NoIO))
      Func[msg::Function::Traits][msg::FunctionTraits::InOut]
        = msg::Analysis::No;
    if (!F.isDeclaration()) {
      auto &Provider = getAnalysis<ServerPrivateProvider>(F);
      auto &LMP = Provider.get<LoopMatcherPass>();
      auto &AA = Provider.get<AAResultsWrapperPass>().getAAResults();
      if (!LMP.getMatcher().empty() || !LMP.getUnmatchedAST().empty())
        Func[msg::Function::Traits][msg::FunctionTraits::Loops]
        = msg::Analysis::Yes;
      if (AA.onlyReadsMemory(&F))
        Func[msg::Function::Traits][msg::FunctionTraits::Readonly]
        = msg::Analysis::Yes;
      auto &FuncCFInfo = Provider.get<ClangCFTraitsPass>().getFuncInfo();
      Func[msg::Function::Exit] = 0;
      if (!F.hasFnAttribute(Attribute::NoReturn))
        for (auto &I : instructions(F))
          if (isa<ReturnInst>(I))
            ++*Func[msg::Function::Exit];
    }
    FuncList[msg::FunctionList::Functions].push_back(std::move(Func));
  }
  return json::Parser<msg::FunctionList>::unparseAsObject(FuncList);
}

std::string PrivateServerPass::answerCalleeFuncList(llvm::Module &M,
    const msg::CalleeFuncList &Request) {
  for (Function &F : M) {
    auto Decl = mTfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto FuncDecl = Decl->getAsFunction();
    if (FuncDecl->getLocStart().getRawEncoding() !=
        Request[msg::CalleeFuncList::FuncID])
      continue;
    if (F.isDeclaration())
      return json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
    msg::CalleeFuncList StmtList = Request;
    auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
    auto &Provider = getAnalysis<ServerPrivateProvider>(F);
    auto &Matcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &Unmatcher = Provider.get<LoopMatcherPass>().getUnmatchedAST();
    auto &FuncInfo = Provider.get<ClangCFTraitsPass>().getFuncInfo();
    auto &CFLoopInfo = Provider.get<ClangCFTraitsPass>().getLoopInfo();
    const ClangCFTraitsPass::RegionCFInfo *Info = nullptr;
    if (StmtList[msg::CalleeFuncList::LoopID]) {
      bcl::tagged_pair<
        bcl::tagged<clang::Stmt *, AST>,
        bcl::tagged<Loop *, IR>> Loop(nullptr, nullptr);
      for (auto Match : Matcher)
        if (Match.get<AST>()->getLocStart().getRawEncoding() ==
            StmtList[msg::CalleeFuncList::LoopID]) {
          Loop = Match;
          break;
        }
      if (!Loop.get<AST>()) {
        for (auto Unmatch : Unmatcher)
          if (Unmatch->getLocStart().getRawEncoding() ==
              StmtList[msg::CalleeFuncList::LoopID]) {
            Loop.get<AST>() = Unmatch;
            break;
          }
      }
      if (!Loop.get<AST>())
        return json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
      auto I = CFLoopInfo.find(Loop.get<AST>());
      if (I != CFLoopInfo.end())
        Info = &I->second;
    } else {
      Info = &FuncInfo;
    }
    for (auto &T : *Info) {
      if (!(T.Flags & StmtList[msg::CalleeFuncList::Attr]))
        continue;
      msg::CalleeFuncInfo F;
      if (isa<clang::BreakStmt>(T))
        F[msg::CalleeFuncInfo::Name] = "break";
      else if (isa<clang::ReturnStmt>(T))
        F[msg::CalleeFuncInfo::Name] = "return";
      else if (isa<clang::GotoStmt>(T))
        F[msg::CalleeFuncInfo::Name] = "goto";
      else if (auto CE = dyn_cast<clang::CallExpr>(T))
        if (auto FD = CE->getDirectCallee())
          F[msg::CalleeFuncInfo::Name] = FD->getName();
        else
          F[msg::CalleeFuncInfo::Name] = "call";
      F[msg::CalleeFuncInfo::ID] =
        utostr(T.Stmt->getLocStart().getRawEncoding());
      F[msg::CalleeFuncInfo::Locations].
            push_back(getLocation(T.Stmt->getLocStart(), SrcMgr));
      StmtList[msg::CalleeFuncList::Functions].push_back(std::move(F));
    }
    return json::Parser<msg::CalleeFuncList>::unparseAsObject(StmtList);
  }
  return json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
}

bool PrivateServerPass::runOnModule(llvm::Module &M) {
  if (!mConnection) {
    M.getContext().emitError("intrusive connection is not established");
    return false;
  }
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("can not access sources"
        ": transformation context is not available");
    return false;
  }
  ServerPrivateProvider::initialize<TransformationEnginePass>(
    [this, &M](TransformationEnginePass &TEP) {
      TEP.setContext(M, mTfmCtx);
  });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  ServerPrivateProvider::initialize<MemoryMatcherImmutableWrapper>(
      [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
    Wrapper.set(*MMWrapper);
  });
  while (mConnection->answer(
      [this, &M](const std::string &Request) -> std::string {
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
      return answerStatistic(M);
    if (Obj->is<msg::LoopTree>())
      return answerLoopTree(M, Obj->as<msg::LoopTree>());
    if (Obj->is<msg::FunctionList>())
      return answerFunctionList(M);
    if (Obj->is<msg::CalleeFuncList>())
      return answerCalleeFuncList(M, Obj->as<msg::CalleeFuncList>());
    llvm_unreachable("Unknown request to server!");
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
