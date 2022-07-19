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

#include "ClangMessages.h"
#include "Passes.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/ControlFlowTraits.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/MemoryTraitJSON.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Unparse/Utils.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/RedirectIO.h>
#include <bcl/utility.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Builtins.h>
#include <clang/Basic/FileManager.h>
#include <llvm/ADT/Optional.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/InitializePasses.h>
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
/// This message provides list of all analyzed files (including implicitly
/// analyzed header files).
JSON_OBJECT_BEGIN(FileList)
JSON_OBJECT_ROOT_PAIR(FileList
  , Files, std::vector<File>
  )

  FileList() : JSON_INIT_ROOT {}
  ~FileList() = default;

  FileList(const FileList &) = default;
  FileList & operator=(const FileList &) = default;
  FileList(FileList &&) = default;
  FileList & operator=(FileList &&) = default;
JSON_OBJECT_END(FileList)

/// \brief This message provides statistic of program analysis results.
///
/// This contains number of analyzed files, functions, loops and variables and
/// number of explored traits, such as induction, reduction, data dependencies,
/// etc.
JSON_OBJECT_BEGIN(Statistic)
JSON_OBJECT_ROOT_PAIR_7(Statistic,
  Functions, unsigned,
  UserFunctions, unsigned,
  ParallelLoops, unsigned,
  Files, std::map<BCL_JOIN(std::string, unsigned)>,
  Loops, std::map<BCL_JOIN(Analysis, unsigned)>,
  Variables, std::map<BCL_JOIN(Analysis, unsigned)>,
  Traits, bcl::StaticTraitMap<BCL_JOIN(unsigned, MemoryDescriptor)>)

  struct InitTraitsFunctor {
    template<class Trait> inline void operator()(unsigned &C) { C = 0; }
  };

  Statistic() : JSON_INIT_ROOT, JSON_INIT(Statistic, 0, 0, 0) {
    (*this)[Statistic::Traits].for_each(InitTraitsFunctor());
  }
  ~Statistic() override = default;

  Statistic(const Statistic &) = default;
  Statistic & operator=(const Statistic &) = default;
  Statistic(Statistic &&) = default;
  Statistic & operator=(Statistic &&) = default;
JSON_OBJECT_END(Statistic)

JSON_OBJECT_BEGIN(LoopTraits)
JSON_OBJECT_PAIR_6(LoopTraits,
  IsAnalyzed, Analysis,
  Perfect, Analysis,
  InOut, Analysis,
  Canonical, Analysis,
  UnsafeCFG, Analysis,
  Parallel, Analysis)

  LoopTraits()
    : JSON_INIT(LoopTraits, Analysis::No, Analysis::No, Analysis::Yes,
                Analysis::No, Analysis::Yes, Analysis::No) {}
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
  ID, std::uint64_t,
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
  FunctionID, std::uint64_t,
  Loops, std::vector<Loop>)

  LoopTree() : JSON_INIT_ROOT {}
  ~LoopTree() override = default;

  LoopTree(const LoopTree &) = default;
  LoopTree & operator=(const LoopTree &) = default;
  LoopTree(LoopTree &&) = default;
  LoopTree & operator=(LoopTree &&) = default;
JSON_OBJECT_END(LoopTree)

JSON_OBJECT_BEGIN(FunctionTraits)
JSON_OBJECT_PAIR_5(FunctionTraits,
  Readonly, Analysis,
  UnsafeCFG, Analysis,
  InOut, Analysis,
  Parallel, Analysis,
  Loops, Analysis)

  FunctionTraits()
    : JSON_INIT(FunctionTraits, Analysis::No, Analysis::Yes, Analysis::Yes,
                Analysis::No, Analysis::No) {}
  ~FunctionTraits() = default;

  FunctionTraits(const FunctionTraits &) = default;
  FunctionTraits & operator=(const FunctionTraits &) = default;
  FunctionTraits(FunctionTraits &&) = default;
  FunctionTraits & operator=(FunctionTraits &&) = default;
JSON_OBJECT_END(FunctionTraits)

JSON_OBJECT_BEGIN(Function)
JSON_OBJECT_PAIR_8(Function,
  ID, std::uint64_t,
  Name, std::string,
  User, bool,
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

enum class StmtKind : uint8_t {
  First = 0,
  Break = First,
  Goto,
  Return,
  Call,
  Invalid,
  Number = Invalid
};

JSON_OBJECT_BEGIN(CalleeFuncInfo)
JSON_OBJECT_PAIR_3(CalleeFuncInfo,
  Kind, StmtKind,
  CalleeID, std::uint64_t,
  StartLocation, std::vector<Location>)

  CalleeFuncInfo() : JSON_INIT(CalleeFuncInfo, StmtKind::Invalid, 0) {}
  ~CalleeFuncInfo() = default;

  CalleeFuncInfo(const CalleeFuncInfo &) = default;
  CalleeFuncInfo & operator=(const CalleeFuncInfo &) = default;
  CalleeFuncInfo(CalleeFuncInfo &&) = default;
  CalleeFuncInfo & operator=(CalleeFuncInfo &&) = default;
JSON_OBJECT_END(CalleeFuncInfo)

JSON_OBJECT_BEGIN(CalleeFuncList)
JSON_OBJECT_ROOT_PAIR_4(CalleeFuncList,
  FuncID, std::uint64_t,
  LoopID, std::uint64_t,
  Attr, CFFlags,
  Functions, std::vector<CalleeFuncInfo>)

  CalleeFuncList() : JSON_INIT_ROOT {}
  ~CalleeFuncList() override = default;

  CalleeFuncList(const CalleeFuncList &) = default;
  CalleeFuncList & operator=(const CalleeFuncList &) = default;
  CalleeFuncList(CalleeFuncList &&) = default;
  CalleeFuncList & operator=(CalleeFuncList &&) = default;
JSON_OBJECT_END(CalleeFuncList)

JSON_OBJECT_BEGIN(SourceObject)
JSON_OBJECT_PAIR_3(SourceObject,
  ID, std::uint64_t,
  Name, std::string,
  DeclLocation, Location)

  SourceObject() : JSON_INIT(SourceObject, 0) {}
  ~SourceObject() = default;

  SourceObject(const SourceObject &) = default;
  SourceObject & operator=(const SourceObject &) = default;
  SourceObject(SourceObject &&) = default;
  SourceObject & operator=(SourceObject &&) = default;
JSON_OBJECT_END(SourceObject)

JSON_OBJECT_BEGIN(MemoryLocation)
JSON_OBJECT_PAIR_5(MemoryLocation,
  Address, std::string,
  Size, uint64_t,
  Locations, std::vector<Location>,
  Traits, DIMemoryTraitSet *,
  Object, SourceObject)

  MemoryLocation() : JSON_INIT(MemoryLocation, "", 0) {}
  ~MemoryLocation() = default;

  MemoryLocation(const MemoryLocation &) = default;
  MemoryLocation &operator=(const MemoryLocation &) = default;
  MemoryLocation(MemoryLocation &&) = default;
  MemoryLocation &operator=(MemoryLocation &&) = default;
JSON_OBJECT_END(MemoryLocation)

JSON_OBJECT_BEGIN(AliasNode)
JSON_OBJECT_PAIR_6(AliasNode,
  ID, std::uintptr_t,
  Kind, DIAliasNode::Kind,
  Coverage, bool,
  Traits, MemoryDescriptor,
  SelfMemory, std::vector<MemoryLocation>,
  CoveredMemory, std::vector<MemoryLocation>)

  AliasNode() : JSON_INIT(AliasNode, 0, DIAliasNode::INVALID_KIND, false) {}
  ~AliasNode() = default;

  AliasNode(const AliasNode &) = default;
  AliasNode & operator=(const AliasNode &) = default;
  AliasNode(AliasNode &&) = default;
  AliasNode & operator= (AliasNode &&) = default;
JSON_OBJECT_END(AliasNode)

JSON_OBJECT_BEGIN(AliasEdge)
JSON_OBJECT_PAIR_3(AliasEdge,
  From, std::uintptr_t,
  To, std::uintptr_t,
  Kind, DIAliasNode::Kind)

  AliasEdge() : JSON_INIT(AliasEdge, 0, 0, DIAliasNode::INVALID_KIND) {}
  AliasEdge(std::uintptr_t From, std::uintptr_t To, DIAliasNode::Kind Kind) :
    JSON_INIT(AliasEdge, From, To, Kind) {}
  ~AliasEdge() = default;

  AliasEdge(const AliasEdge &) = default;
  AliasEdge & operator=(const AliasEdge &) = default;
  AliasEdge(AliasEdge &&) = default;
  AliasEdge & operator=(AliasEdge &&) = default;
JSON_OBJECT_END(AliasEdge)

JSON_OBJECT_BEGIN(AliasTree)
JSON_OBJECT_ROOT_PAIR_4(AliasTree,
  FuncID, std::uint64_t,
  LoopID, std::uint64_t,
  Nodes, std::vector<AliasNode>,
  Edges, std::vector<AliasEdge>)

  AliasTree() : JSON_INIT_ROOT, JSON_INIT(AliasTree, 0) {}
  ~AliasTree() override = default;

  AliasTree(const AliasTree &) = default;
  AliasTree & operator=(const AliasTree &) = default;
  AliasTree(AliasTree &&) = default;
  AliasTree & operator=(AliasTree &&) = default;
JSON_OBJECT_END(AliasTree)

JSON_OBJECT_BEGIN(Reduction)
JSON_OBJECT_PAIR(Reduction, Kind, trait::Reduction::Kind)
  Reduction() : JSON_INIT(Reduction, trait::Reduction::RK_NoReduction) {}
JSON_OBJECT_END(Reduction)

JSON_OBJECT_BEGIN(Induction)
JSON_OBJECT_PAIR_4(Induction,
  Kind, trait::DIInduction::InductionKind,
  Start, Optional<std::int64_t>,
  End, Optional<std::int64_t>,
  Step, Optional<std::int64_t>)
  Induction() : JSON_INIT(Induction,
    trait::DIInduction::InductionKind::IK_NoInduction) {}
JSON_OBJECT_END(Induction)

JSON_OBJECT_BEGIN(Dependence)
JSON_OBJECT_PAIR_4(Dependence,
  May, bool,
  Min, Optional<std::int64_t>,
  Max, Optional<std::int64_t>,
  Causes, std::vector<std::string>)
  Dependence() : JSON_INIT(Dependence, true) {}
JSON_OBJECT_END(Dependence)
}
}

JSON_DEFAULT_TRAITS(tsar::msg::, Statistic)
JSON_DEFAULT_TRAITS(tsar::msg::, FileList)
JSON_DEFAULT_TRAITS(tsar::msg::, LoopTraits)
JSON_DEFAULT_TRAITS(tsar::msg::, Loop)
JSON_DEFAULT_TRAITS(tsar::msg::, LoopTree)
JSON_DEFAULT_TRAITS(tsar::msg::, FunctionTraits)
JSON_DEFAULT_TRAITS(tsar::msg::, Function)
JSON_DEFAULT_TRAITS(tsar::msg::, FunctionList)
JSON_DEFAULT_TRAITS(tsar::msg::, CalleeFuncInfo)
JSON_DEFAULT_TRAITS(tsar::msg::, CalleeFuncList)
JSON_DEFAULT_TRAITS(tsar::msg::, SourceObject)
JSON_DEFAULT_TRAITS(tsar::msg::, MemoryLocation)
JSON_DEFAULT_TRAITS(tsar::msg::, AliasNode)
JSON_DEFAULT_TRAITS(tsar::msg::, AliasEdge)
JSON_DEFAULT_TRAITS(tsar::msg::, AliasTree)
JSON_DEFAULT_TRAITS(tsar::msg::, Reduction)
JSON_DEFAULT_TRAITS(tsar::msg::, Induction)
JSON_DEFAULT_TRAITS(tsar::msg::, Dependence)

namespace json {
/// Specialization of JSON serialization traits for tsar::msg::LoopType type.
template<> struct Traits<tsar::msg::LoopType> {
  static bool parse(tsar::msg::LoopType &Dest, ::json::Lexer &Lex) noexcept {
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

/// Specialization of JSON serialization traits for tsar::msg::StmtKind type.
template<> struct Traits<tsar::msg::StmtKind> {
  static bool parse(tsar::msg::StmtKind &Dest, ::json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::msg::StmtKind>(Lex.json())
        .Case("Break", tsar::msg::StmtKind::Break)
        .Case("Goto", tsar::msg::StmtKind::Goto)
        .Case("Return", tsar::msg::StmtKind::Return)
        .Case("Call", tsar::msg::StmtKind::Call)
        .Default(tsar::msg::StmtKind::Invalid);
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, tsar::msg::StmtKind Obj) {
    JSON += '"';
    switch (Obj) {
    case tsar::msg::StmtKind::Break: JSON += "Break"; break;
    case tsar::msg::StmtKind::Goto: JSON += "Goto"; break;
    case tsar::msg::StmtKind::Return: JSON += "Return"; break;
    case tsar::msg::StmtKind::Call: JSON += "Call"; break;
    default: JSON += "Invalid"; break;
    }
    JSON += '"';
  }
};

/// Specialization of JSON serialization traits for tsar::DIAliasNode::Kind type.
template<> struct Traits<tsar::DIAliasNode::Kind> {
  static bool parse(tsar::DIAliasNode::Kind &Dest,
                    ::json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::DIAliasNode::Kind>(Lex.json())
        .Case("Top", tsar::DIAliasNode::KIND_TOP)
        .Case("Estimate", tsar::DIAliasNode::KIND_ESTIMATE)
        .Case("Unknown", tsar::DIAliasNode::KIND_UNKNOWN)
        .Default(tsar::DIAliasNode::INVALID_KIND);

    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, tsar::DIAliasNode::Kind Obj) {
    JSON += '"';
    switch (Obj) {
    case tsar::DIAliasNode::KIND_TOP: JSON += "Top"; break;
    case tsar::DIAliasNode::KIND_ESTIMATE: JSON += "Estimate"; break;
    case tsar::DIAliasNode::KIND_UNKNOWN: JSON += "Unknown"; break;
    default: JSON += "Invalid"; break;
    }
    JSON += '"';
  }
};

/// Specialization of JSON serialization traits for
/// tsar::trait::DIInduction::InductionKind type.
template<> struct Traits<trait::DIInduction::InductionKind> {
  static bool parse(trait::DIInduction::InductionKind &Dest,
      ::json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<trait::DIInduction::InductionKind>(Lex.json())
        .Case("Int", trait::DIInduction::InductionKind::IK_IntInduction)
        .Case("Float", trait::DIInduction::InductionKind::IK_FpInduction)
        .Case("Pointer", trait::DIInduction::InductionKind::IK_PtrInduction)
        .Default(trait::DIInduction::InductionKind::IK_NoInduction);
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, trait::DIInduction::InductionKind Obj) {
    JSON += '"';
    switch (Obj) {
    case trait::DIInduction::InductionKind::IK_FpInduction:
      JSON += "Float";
      break;
    case trait::DIInduction::InductionKind::IK_IntInduction:
      JSON += "Int";
      break;
    case trait::DIInduction::InductionKind::IK_PtrInduction:
      JSON += "Pointer";
      break;
    default:
      JSON += "NoInduction";
      break;
    }
    JSON += '"';
  }
};


/// Specialization of JSON serialization traits for tsar::trait::DIInduction.
template<> struct Traits<trait::DIInduction *> {
  static bool parse(trait::DIInduction *&Dest, ::json::Lexer &Lex) {
    msg::Induction TmpDest;
    auto Res = Traits<msg::Induction>::parse(TmpDest, Lex);
    if (!Res)
      return false;
    trait::DIInduction::Constant Start, End, Step;
    if (TmpDest[msg::Induction::Start])
      Start = APSInt::get(*TmpDest[msg::Induction::Start]);
    if (TmpDest[msg::Induction::End])
      End = APSInt::get(*TmpDest[msg::Induction::End]);
    if (TmpDest[msg::Induction::Step])
      Start = APSInt::get(*TmpDest[msg::Induction::Step]);
    Dest = new trait::DIInduction(TmpDest[msg::Induction::Kind]);
    return true;
  }

  static void unparse(String &JSON, const trait::DIInduction *Obj) {
    msg::Induction TmpObj;
    TmpObj[msg::Induction::Kind] = Obj->getKind();
    if (auto V = Obj->getStart()) {
      msg::json_::InductionImpl::Start::ValueType::value_type Start;
      if (castAPInt(*V, V->isSigned(), Start))
        TmpObj[msg::Induction::Start] = Start;
    }
    if (auto V = Obj->getEnd()) {
      msg::json_::InductionImpl::End::ValueType::value_type End;
      if (castAPInt(*V, V->isSigned(), End))
        TmpObj[msg::Induction::End] = End;
    }
    if (auto V = Obj->getStep()) {
      msg::json_::InductionImpl::Step::ValueType::value_type Step;
      if (castAPInt(*V, V->isSigned(), Step))
        TmpObj[msg::Induction::Step] = Step;
    }
    Traits<msg::Induction>::unparse(JSON, TmpObj);
  }
};

/// Specialization of JSON serialization traits for tsar::trait::DIIReduction.
template<> struct Traits<trait::DIReduction *> {
  static bool parse(trait::DIReduction *&Dest, ::json::Lexer &Lex) {
    msg::Reduction TmpDest;
    auto Res = Traits<msg::Reduction>::parse(TmpDest, Lex);
    if (!Res)
      return false;
    Dest = new trait::DIReduction(TmpDest[msg::Reduction::Kind]);
    return true;
  }

  static void unparse(String &JSON, const trait::DIReduction *Obj) {
    msg::Reduction TmpObj;
    TmpObj[msg::Reduction::Kind] = Obj->getKind();
    Traits<msg::Reduction>::unparse(JSON, TmpObj);
  }
};

/// Specialization of JSON serialization traits for tsar::trait::DIDependence.
template<> struct Traits<trait::DIDependence *> {
  static bool parse(trait::DIDependence *&Dest, ::json::Lexer &Lex) {
    msg::Dependence TmpDest;
    auto Res = Traits<msg::Dependence>::parse(TmpDest, Lex);
    if (!Res)
      return false;
    trait::DIDependence::Flag F;
    if (TmpDest[msg::Dependence::May])
      F |= trait::DIDependence::May;
    for (auto Cause : TmpDest[msg::Dependence::Causes])
      if (Cause == "load" || Cause == "store")
        F |= trait::DIDependence::LoadStoreCause;
      else if (Cause == "unknown")
        F |= trait::DIDependence::UnknownCause;
      else if (Cause == "call")
        F |= trait::DIDependence::CallCause;
      else if (Cause == "confused")
        F |= trait::DIDependence::ConfusedCause;
    trait::DIDependence::DistanceRange Range;
    if (TmpDest[msg::Dependence::Min])
      Range.first = APSInt::get(*TmpDest[msg::Dependence::Min]);
    if (TmpDest[msg::Dependence::Max])
      Range.first = APSInt::get(*TmpDest[msg::Dependence::Max]);
    Dest = new trait::DIDependence(F, makeArrayRef(Range));
    return true;
  }

  static void unparse(String &JSON, const trait::DIDependence *Obj) {
    msg::Dependence TmpObj;
    TmpObj[msg::Dependence::May] = Obj->isMay();
    if (Obj->getLevels() > 0) {
      auto Range = Obj->getDistance(0);
      if (auto V = Range.first) {
        msg::json_::DependenceImpl::Min::ValueType::value_type Min;
        if (castAPInt(*V, V->isSigned(), Min))
          TmpObj[msg::Dependence::Min] = Min;
      }
      if (auto V = Range.second) {
        msg::json_::DependenceImpl::Max::ValueType::value_type Max;
        if (castAPInt(*V, V->isSigned(), Max))
          TmpObj[msg::Dependence::Max] = Max;
      }
    }
    if (Obj->isUnknown())
      TmpObj[msg::Dependence::Causes].push_back("unknown");
    if (Obj->isLoadStore()) {
      TmpObj[msg::Dependence::Causes].push_back("load");
      TmpObj[msg::Dependence::Causes].push_back("store");
    }
    if (Obj->isCall())
      TmpObj[msg::Dependence::Causes].push_back("call");
    if (Obj->isConfused())
      TmpObj[msg::Dependence::Causes].push_back("confused");
    Traits<msg::Dependence>::unparse(JSON, TmpObj);
  }
};

/// Specialization of JSON serialization traits for tsar::DIMemoryTraitSet.
template<> struct Traits<DIMemoryTraitSet> {
  struct FromJSONFunctor {
    template<class Trait> void operator()() {
      if (Trait::toString() == TraitStr) {
        auto *Info = TS.get<Trait>();
        parse(Info);
        TS.set<Trait>(Info);
      }
    }
    void parse(trait::DIReduction *&Info) {
      Result = Traits<trait::DIReduction *>::parse(Info, Lex);
    }
    void parse(trait::DIInduction *&Info) {
      Result = Traits<trait::DIInduction *>::parse(Info, Lex);
    }
    void parse(trait::DIDependence *&Info) {
      Result = Traits<trait::DIDependence *>::parse(Info, Lex);
    }
    void parse(void *) { Result = true; }

    StringRef TraitStr;
    DIMemoryTraitSet &TS;
    ::json::Lexer &Lex;
    bool Result;
  };
  struct ToJSONFunctor {
    template<class Trait> void operator()() {
      JSON += ("\"" + Trait::toString() + "\":").str();
      if (auto *Info = TS.get<Trait>())
        unparse(Info);
      else
        JSON += "{}";
      JSON += ",";
    }
    void unparse(const trait::DIReduction *Info) {
      Traits<decltype(Info)>::unparse(JSON, Info);
    }
    void unparse(const trait::DIInduction *Info) {
      Traits<decltype(Info)>::unparse(JSON, Info);
    }
    void unparse(const trait::DIDependence *Info) {
      Traits<decltype(Info)>::unparse(JSON, Info);
    }
    void unparse(const void *) { JSON += "{}"; }

    const DIMemoryTraitSet &TS;
    String &JSON;
  };
  static bool parse(DIMemoryTraitSet &Dest, ::json::Lexer &Lex) {
    Dest.unset_all();
    return Parser<>::traverse<Traits<DIMemoryTraitSet>>(Dest, Lex);
  }
  static bool parse(DIMemoryTraitSet &Dest, ::json::Lexer &Lex,
      std::pair<Position, Position> Key) noexcept {
    Lex.storePosition();
    Lex.setPosition(Key.first);
    std::string ID;
    if (!Traits<std::string>::parse(ID, Lex))
      return false;
    Lex.restorePosition();
    bool R = true;
    DIMemoryTraitSet::for_each_available(FromJSONFunctor{ ID, Dest, Lex, R });
    return R;
  }
  static void unparse(String &JSON, const DIMemoryTraitSet &Obj) {
    JSON += "{";
    Obj.for_each(ToJSONFunctor{ Obj, JSON });
    if (JSON.back() != '{')
      JSON.back() = '}';
    else
      JSON += '}';
  }
};

/// Specialization of JSON serialization traits for tsar::MemoryDescriptor.
template<> struct Traits<MemoryDescriptor> {
  struct FromJSONFunctor {
    template<class Trait> void operator()() {
      if (Trait::toString() == TraitStr)
        Dptr.set<Trait>();
    }
    StringRef TraitStr;
    MemoryDescriptor &Dptr;
  };
  struct ToJSONFunctor {
    template<class Trait> void operator()() {
      JSON += ("\"" + Trait::toString() + "\",").str();
    }
    String &JSON;
  };
  static bool parse(MemoryDescriptor &Dest, ::json::Lexer &Lex) {
    Position MaxIdx, Count;
    bool Ok;
    std::tie(Count, MaxIdx, Ok) = Parser<>::numberOfKeys(Lex);
    if (!Ok)
      return false;
    Dest.unset_all();
    return Parser<>::traverse<Traits<MemoryDescriptor>>(Dest, Lex);
  }
  static bool parse(MemoryDescriptor &Dest, ::json::Lexer &Lex,
      std::pair<Position, Position> Key) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      MemoryDescriptor::for_each_available(FromJSONFunctor{ S, Dest });
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, MemoryDescriptor Obj) {
    JSON += '[';
    Obj.for_each(ToJSONFunctor{ JSON });
    if (JSON.back() != '[')
      JSON.back() = ']';
    else
      JSON += ']';
  }
};

/// Specialization of JSON serialization traits for tsar::CFFlags.
template<> struct Traits<CFFlags> {
  static bool parse(CFFlags &Dest, ::json::Lexer &Lex) {
    Position MaxIdx, Count;
    bool Ok;
    std::tie(Count, MaxIdx, Ok) = Parser<>::numberOfKeys(Lex);
    if (!Ok)
      return false;
    Dest = CFFlags::DefaultFlags;
    return Parser<>::traverse<Traits<CFFlags>>(Dest, Lex);
  }
  static bool parse(CFFlags &Dest, ::json::Lexer &Lex,
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

using ServerPrivateProvider = FunctionPassAAProvider<
  AnalysisSocketImmutableWrapper,
  ParallelLoopPass,
  TransformationEnginePass,
  LoopMatcherPass,
  DFRegionInfoPass,
  ClangPerfectLoopPass,
  CanonicalLoopPass,
  MemoryMatcherImmutableWrapper,
  LoopAttributesDeductionPass,
  ClangCFTraitsPass,
  AAResultsWrapperPass,
  ClangDIMemoryMatcherPass
>;

INITIALIZE_PROVIDER_BEGIN(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")
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

  struct Definition {
    Definition(llvm::DICompileUnit *CU, TransformationContextBase *TfmCtx,
               clang::Decl *Body, uint64_t Id)
        : CU(CU), TfmCtx(TfmCtx), Body(Body), Id(Id) {}
    llvm::DICompileUnit *CU;
    TransformationContextBase *TfmCtx;
    clang::Decl *Body;
    uint64_t Id;
  };

  using TypeToFunctionMap = StringMap<bcl::tagged_pair<
      bcl::tagged<Definition *, Definition>,
      bcl::tagged<SmallPtrSet<clang::FunctionDecl *, 4>, clang::FunctionDecl>>>;

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
  std::string answerFileList();
  std::string answerFunctionList(llvm::Module &M);
  std::string answerLoopTree(llvm::Module &M, const msg::LoopTree &Request);
  std::string answerCalleeFuncList(llvm::Module &M,
    const msg::CalleeFuncList &Request);
  std::string answerAliasTree(llvm::Module &M, const msg::AliasTree &Request);

  /// Recursively collect builtin functions in a specified context and
  /// inner contexts.
  void collectBuiltinFunctions(clang::DeclContext &DeclCtx, llvm::Module &M,
      llvm::DICompileUnit &CU, ClangTransformationContext &TfmCtx,
      TypeToFunctionMap &TypeToFunc, msg::FunctionList &FuncList);

  bcl::IntrusiveConnection *mConnection;
  bcl::RedirectIO *mStdErr;

  TransformationInfo *mTfmInfo = nullptr;
  const GlobalOptions *mGlobalOpts = nullptr;
  AnalysisSocket *mSocket = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;

  std::vector<std::unique_ptr<Definition>> mDefinitions;

  /// List of canonical function declarations which is visible to user in GUI.
  /// GUI knowns this function and it can highlight some information if
  /// necessary.
  DenseMap<clang::Decl *, Definition *> mVisibleToUser;
};

Optional<uint64_t> toId(llvm::Module *M, llvm::DICompileUnit *CU,
              clang::SourceLocation Loc) {
  assert(M && "Module must not be null!");
  assert(CU && "Compilation unit must not be null!");
  auto CUs{M->getNamedMetadata("llvm.dbg.cu")};
  unsigned CUId{1};
  for (auto I{CUs->operands().begin()}, EI{CUs->operands().end()};
       I != EI && CU != *I; ++I, ++CUId)
    ;
  assert(CUId <= CUs->getNumOperands() &&
         "Compilation unit must be a part of module!");
  uint64_t Id;
  if (bcl::shrinkPair(CUId, Loc.getRawEncoding(), Id))
    return Id;
  return None;
}

/// Increments count of analyzed traits in a specified map TM.
template<class TraitMap>
std::pair<unsigned, unsigned> incrementTraitCount(Function &F,
    const GlobalOptions &GO, ServerPrivateProvider &P, AnalysisSocket &S,
    TraitMap &TM) {
  auto RF =  S.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(F);
  assert(RF && "Dependence analysis must be available!");
  auto RM = S.getAnalysis<AnalysisClientServerMatcherWrapper>();
  assert(RM && "Client to server IR-matcher must be available!");
  auto &DIAT = RF->value<DIEstimateMemoryPass *>()->getAliasTree();
  SpanningTreeRelation<DIAliasTree *> STR(&DIAT);
  auto &DIDepInfo = RF->value<DIDependencyAnalysisPass *>()->getDependencies();
  auto &CToS = **RM->value<AnalysisClientServerMatcherWrapper *>();
  auto &LMP = P.get<LoopMatcherPass>();
  auto &ParallelInfo = P.get<ParallelLoopPass>().getParallelLoopInfo();
  unsigned ParallelLoops = 0, NotAnalyzedLoops = 0;
  for (auto &Match : LMP.getMatcher()) {
    auto *L = Match.get<IR>();
    if (ParallelInfo.count(L))
      ++ParallelLoops;
    if (!L->getLoopID()) {
      ++NotAnalyzedLoops;
      continue;
    }
    auto ServerLoopID = cast<MDNode>(*CToS.getMappedMD(L->getLoopID()));
    if (!ServerLoopID) {
      ++NotAnalyzedLoops;
      continue;
    }
    auto DIDepSet = DIDepInfo[ServerLoopID];
    DenseSet<const DIAliasNode *> Coverage;
    accessCoverage<bcl::SimpleInserter>(DIDepSet, DIAT, Coverage,
                                        GO.IgnoreRedundantMemory);
    for (auto &TS : DIDepSet) {
      if (!Coverage.count(TS.getNode()))
        continue;
      for (auto &T : make_range(TS.begin(), TS.end())) {
        if (T->is<trait::NoAccess>()) {
          if (T->is<trait::AddressAccess>())
            ++TM.template value<trait::AddressAccess>();
          continue;
        }
        TS.for_each(bcl::TraitMapConstructor<
          MemoryDescriptor, TraitMap, bcl::CountInserter>(TS, TM));
      }
    }
  }
  return std::pair(ParallelLoops, NotAnalyzedLoops);
}

msg::Loop getLoopInfo(clang::Stmt *S, clang::SourceManager &SrcMgr,
    llvm::Module &M, llvm::DICompileUnit &CU) {
  assert(S && "Statement must not be null!");
  auto LocStart = S->getBeginLoc();
  auto LocEnd = S->getEndLoc();
  msg::Loop Loop;
  Loop[msg::Loop::ID] = *toId(&M, &CU, LocStart);
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
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_END(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)

std::string PrivateServerPass::answerStatistic(llvm::Module &M) {
  msg::Statistic Stat;
  for (auto &&[CU, TfmCtxBase] : mTfmInfo->contexts()) {
    assert(CU && "Compilation unit must not be null!");
    if (!TfmCtxBase || !TfmCtxBase->hasInstance())
      continue;
    if (auto *TfmCtx{cast<ClangTransformationContext>(TfmCtxBase)}) {
      auto &Rewriter = TfmCtx->getRewriter();
      for (auto FI = Rewriter.getSourceMgr().fileinfo_begin(),
                EI = Rewriter.getSourceMgr().fileinfo_end();
           FI != EI; ++FI) {
        auto Ext = sys::path::extension(FI->first->getName());
        std::string Kind;
        if (std::find(msg::HeaderFile::extensions().begin(),
                      msg::HeaderFile::extensions().end(),
                      Ext) != msg::HeaderFile::extensions().end())
          Kind = msg::HeaderFile::name();
        else if (std::find(msg::SourceFile::extensions().begin(),
                           msg::SourceFile::extensions().end(),
                           Ext) != msg::SourceFile::extensions().end())
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
      auto &SrcMgr = Rewriter.getSourceMgr();
      for (Function &F : M) {
        if (isMemoryMarkerIntrinsic(F.getIntrinsicID()) ||
            isDbgInfoIntrinsic(F.getIntrinsicID()))
          continue;
        ++Stat[msg::Statistic::Functions];
        auto Decl = TfmCtx->getDeclForMangledName(F.getName());
        if (!Decl)
          continue;
        if (SrcMgr.getFileCharacteristic(Decl->getBeginLoc()) !=
            clang::SrcMgr::C_User)
          continue;
        ++Stat[msg::Statistic::UserFunctions];
        // Analysis are not available for functions without body.
        if (F.isDeclaration())
          continue;
        auto &Provider = getAnalysis<ServerPrivateProvider>(F);
        auto &LMP = Provider.get<LoopMatcherPass>();
        Loops.first += LMP.getMatcher().size();
        Loops.second += LMP.getUnmatchedAST().size();
        auto [ParallelLoops, NotAnalyzedLoops] = incrementTraitCount(
            F, *mGlobalOpts, Provider, *mSocket, Stat[msg::Statistic::Traits]);
        Stat[msg::Statistic::ParallelLoops] += ParallelLoops;
        Loops.first -= NotAnalyzedLoops;
        Loops.second += NotAnalyzedLoops;
      }
      Stat[msg::Statistic::Loops].insert(
          std::make_pair(msg::Analysis::Yes, Loops.first));
      Stat[msg::Statistic::Loops].insert(
          std::make_pair(msg::Analysis::No, Loops.second));
    }
  }
  return ::json::Parser<msg::Statistic>::unparseAsObject(Stat);
}

std::string PrivateServerPass::answerLoopTree(llvm::Module &M,
    const msg::LoopTree &Request) {
  for (Function &F : M) {
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto *CU{DISub->getUnit()};
    auto *TfmCtx{dyn_cast_or_null<ClangTransformationContext>(
        mTfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto CanonicalFD = Decl->getCanonicalDecl()->getAsFunction();
    auto DefItr{mVisibleToUser.find(CanonicalFD)};
    if (DefItr == mVisibleToUser.end() ||
        DefItr->second->Id != Request[msg::LoopTree::FunctionID])
      continue;
    if (F.isDeclaration())
      return ::json::Parser<msg::LoopTree>::unparseAsObject(Request);
    msg::LoopTree LoopTree;
    LoopTree[msg::LoopTree::FunctionID] = Request[msg::LoopTree::FunctionID];
    auto &SrcMgr = TfmCtx->getContext().getSourceManager();
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
    auto &ParallelInfo = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
    for (auto &Match : Matcher) {
      auto Loop = getLoopInfo(Match.get<AST>(), SrcMgr, M, *CU);
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
      if (ParallelInfo.count(Match.get<IR>()))
        LT[msg::LoopTraits::Parallel] = msg::Analysis::Yes;
      LoopTree[msg::LoopTree::Loops].push_back(std::move(Loop));
    }
    for (auto &Unmatch : Unmatcher) {
      auto Loop = getLoopInfo(Unmatch, SrcMgr, M, *CU);
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
    return ::json::Parser<msg::LoopTree>::unparseAsObject(LoopTree);
  }
  return ::json::Parser<msg::LoopTree>::unparseAsObject(Request);
}

void PrivateServerPass::collectBuiltinFunctions(
    clang::DeclContext &DeclCtx, llvm::Module &M, llvm::DICompileUnit &CU,
    ClangTransformationContext &TfmCtx, TypeToFunctionMap &TypeToFunc,
    msg::FunctionList &FuncList) {
  for (auto *D : DeclCtx.decls()) {
    // Lookup for C builtins in CXX unit.
    if (auto *LinkageCtx = dyn_cast<clang::LinkageSpecDecl>(D)) {
      collectBuiltinFunctions(*LinkageCtx, M, CU, TfmCtx, TypeToFunc, FuncList);
      continue;
    }
    clang::Decl *DeclToCheck = D;
    if (auto *UD = dyn_cast<clang::UsingShadowDecl>(D)) {
      assert(UD->getUnderlyingDecl() &&
        "Underlying declaration in using must be known!");
      DeclToCheck = UD->getUnderlyingDecl();
    }
    if (auto *FD = dyn_cast<clang::FunctionDecl>(DeclToCheck)) {
      auto ID = FD->getBuiltinID();
      if (ID == 0)
        continue;
      FD = FD->getCanonicalDecl();
      if (mVisibleToUser.count(FD))
        continue;
      auto [TypeItr, IsNew] =
          TypeToFunc.try_emplace(FD->getType().getAsString());
      if (!TypeItr->second.get<clang::FunctionDecl>().insert(FD).second)
        continue;
      const clang::FunctionDecl *DefinitionFD{FD};
      bool HasBody{FD->getBody(DefinitionFD) != nullptr};
      if (IsNew) {
        mDefinitions.push_back(std::make_unique<Definition>(
            &CU, &TfmCtx, const_cast<clang::FunctionDecl *>(DefinitionFD),
            *toId(&M, &CU, DefinitionFD->getBeginLoc())));
        TypeItr->second.get<Definition>() = mDefinitions.back().get();
      } else if (HasBody) {
        TypeItr->second.get<Definition>()->CU = &CU;
        TypeItr->second.get<Definition>()->TfmCtx = &TfmCtx;
        TypeItr->second.get<Definition>()->Body =
            const_cast<clang::FunctionDecl *>(DefinitionFD);
        TypeItr->second.get<Definition>()->Id  =
            *toId(&M, &CU, DefinitionFD->getBeginLoc());
      }
    }
  }
}

std::string PrivateServerPass::answerFileList() {
  msg::FileList FileList;
  for (auto &&[CU, TfmCtxBase] : mTfmInfo->contexts()) {
    if (!TfmCtxBase || !TfmCtxBase->hasInstance())
      continue;
    if (auto *TfmCtx{
            dyn_cast_or_null<ClangTransformationContext>(TfmCtxBase)}) {
      auto &ASTCtx = TfmCtx->getContext();
      auto &SrcMgr = ASTCtx.getSourceManager();
      for (auto &Info :
           make_range(SrcMgr.fileinfo_begin(), SrcMgr.fileinfo_end())) {
        msg::File File;
        File[msg::File::ID] = Info.first->getUniqueID();
        File[msg::File::Name] = std::string(Info.first->getName());
        FileList[msg::FileList::Files].emplace_back(std::move(File));
      }
    }
  }
  return ::json::Parser<msg::FileList>::unparseAsObject(FileList);
}

std::string PrivateServerPass::answerFunctionList(llvm::Module &M) {
  msg::FunctionList FuncList;
  for (Function &F : M) {
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto *CU{DISub->getUnit()};
    auto *TfmCtx{dyn_cast_or_null<ClangTransformationContext>(
        mTfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance())
      continue;
    auto &ASTCtx = TfmCtx->getContext();
    auto &SrcMgr = ASTCtx.getSourceManager();
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto FuncDecl = Decl->getAsFunction();
    auto *CanonicalD = FuncDecl->getCanonicalDecl();
    mDefinitions.push_back(std::make_unique<Definition>(
        CU, TfmCtx, Decl, *toId(&M, CU, CanonicalD->getBeginLoc())));
    mVisibleToUser.try_emplace(CanonicalD, mDefinitions.back().get());
    for (auto &&[CU, TfmCtxBase] : mTfmInfo->contexts()) {
      if (!TfmCtxBase || !TfmCtxBase->hasInstance() ||
          !isa<ClangTransformationContext>(TfmCtxBase))
        continue;
      auto ExternalDecl{cast<ClangTransformationContext>(TfmCtxBase)
                            ->getDeclForMangledName(F.getName())};
      if (!ExternalDecl)
        continue;
      mVisibleToUser.try_emplace(ExternalDecl->getCanonicalDecl(),
                                 mDefinitions.back().get());
    }
    assert(FuncDecl && "Function declaration must not be null!");
    msg::Function Func;
    SmallString<64> ExtraName;
    Func[msg::Function::Name] = getFunctionName(*FuncDecl, ExtraName).str();
    // Canonical declaration may differs from declaration which has a body.
    Func[msg::Function::ID] = mDefinitions.back()->Id;
    Func[msg::Function::User] =
        (SrcMgr.getFileCharacteristic(Decl->getBeginLoc()) ==
         clang::SrcMgr::C_User);
    Func[msg::Function::StartLocation] =
        getLocation(FuncDecl->getBeginLoc(), SrcMgr);
    Func[msg::Function::EndLocation] =
        getLocation(FuncDecl->getEndLoc(), SrcMgr);
    if (hasFnAttr(F, AttrKind::AlwaysReturn) &&
        F.hasFnAttribute(Attribute::NoUnwind) &&
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
      auto &PI = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
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
      if (!PI.empty())
        Func[msg::Function::Traits][msg::FunctionTraits::Parallel] =
          msg::Analysis::Yes;
    }
    FuncList[msg::FunctionList::Functions].push_back(std::move(Func));
  }
  TypeToFunctionMap TypeToFunc;
  for (auto &&[CU, TfmCtxBase] : mTfmInfo->contexts()) {
    if (!TfmCtxBase || !TfmCtxBase->hasInstance() ||
        !isa<ClangTransformationContext>(TfmCtxBase))
      continue;
    auto &TfmCtx{cast<ClangTransformationContext>(*TfmCtxBase)};
    collectBuiltinFunctions(*TfmCtx.getContext().getTranslationUnitDecl(), M,
                            *CU, TfmCtx, TypeToFunc, FuncList);
  }
  for (auto &Funcs : TypeToFunc) {
    auto *TfmCtxBase{Funcs.second.get<Definition>()->TfmCtx};
    if (!isa<ClangTransformationContext>(TfmCtxBase))
      continue;
    auto *TfmCtx{cast<ClangTransformationContext>(TfmCtxBase)};
    auto *FD{cast<clang::FunctionDecl>(Funcs.second.get<Definition>()->Body)};
    auto &ASTCtx{TfmCtx->getContext()};
    auto &SrcMgr{ASTCtx.getSourceManager()};
    msg::Function Func;
    SmallString<64> ExtraName;
    Func[msg::Function::Name] = getFunctionName(*FD, ExtraName).str();
    // Canonical declaration may differs from declaration which has a body.
    Func[msg::Function::ID] = Funcs.second.get<Definition>()->Id;
    Func[msg::Function::User] = false;
    Func[msg::Function::StartLocation] = getLocation(FD->getBeginLoc(), SrcMgr);
    Func[msg::Function::EndLocation] = getLocation(FD->getEndLoc(), SrcMgr);
    auto ID = FD->getBuiltinID();
    assert(ID != 0 && "It must be a builtin function!");
    if (ASTCtx.BuiltinInfo.isNoThrow(ID) &&
        !ASTCtx.BuiltinInfo.isNoReturn(ID) &&
        !ASTCtx.BuiltinInfo.isReturnsTwice(ID))
      Func[msg::Function::Traits][msg::FunctionTraits::UnsafeCFG] =
          msg::Analysis::No;
    FuncList[msg::FunctionList::Functions].push_back(std::move(Func));
    for (auto *FD : Funcs.second.get<clang::FunctionDecl>())
      mVisibleToUser.try_emplace(FD, Funcs.second.get<Definition>());
  }
  return ::json::Parser<msg::FunctionList>::unparseAsObject(FuncList);
}

std::string PrivateServerPass::answerCalleeFuncList(llvm::Module &M,
    const msg::CalleeFuncList &Request) {
  for (Function &F : M) {
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto *CU{DISub->getUnit()};
    auto *TfmCtx{dyn_cast_or_null<ClangTransformationContext>(
        mTfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto CanonicalFD = Decl->getCanonicalDecl()->getAsFunction();
    auto DefItr{mVisibleToUser.find(CanonicalFD)};
    if (DefItr == mVisibleToUser.end() ||
        DefItr->second->Id != Request[msg::CalleeFuncList::FuncID])
      continue;
    if (F.isDeclaration())
      return ::json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
    msg::CalleeFuncList StmtList = Request;
    auto &SrcMgr = TfmCtx->getContext().getSourceManager();
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
        if (auto Id{toId(&M, CU, Match.get<AST>()->getBeginLoc())};
            Id && *Id == StmtList[msg::CalleeFuncList::LoopID]) {
          Loop = Match;
          break;
        }
      if (!Loop.get<AST>()) {
        for (auto Unmatch : Unmatcher)
          if (auto Id{toId(&M, CU, Unmatch->getBeginLoc())};
              Id && *Id == StmtList[msg::CalleeFuncList::LoopID]) {
            Loop.get<AST>() = Unmatch;
            break;
          }
      }
      if (!Loop.get<AST>())
        return ::json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
      auto I = CFLoopInfo.find(Loop.get<AST>());
      if (I != CFLoopInfo.end())
        Info = &I->second;
    } else {
      Info = &FuncInfo;
    }
    if (!Info)
      return ::json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
    DenseMap<const clang::FunctionDecl *, msg::CalleeFuncInfo> FuncMap;
    std::array<msg::CalleeFuncInfo,
               static_cast<std::size_t>(msg::StmtKind::Number)>
        StmtMap;
    for (auto &T : *Info) {
      if (!(T.Flags & StmtList[msg::CalleeFuncList::Attr]) &&
          !(StmtList[msg::CalleeFuncList::Attr] == DefaultFlags &&
            isa<clang::CallExpr>(T)))
        continue;
      msg::CalleeFuncInfo *F = nullptr;
      if (isa<clang::BreakStmt>(T)) {
        F = &StmtMap[static_cast<std::size_t>(msg::StmtKind::Break)];
        (*F)[msg::CalleeFuncInfo::Kind] = msg::StmtKind::Break;
      } else if (isa<clang::ReturnStmt>(T)) {
        F = &StmtMap[static_cast<std::size_t>(msg::StmtKind::Return)];
        (*F)[msg::CalleeFuncInfo::Kind] = msg::StmtKind::Return;
      } else if (isa<clang::GotoStmt>(T)) {
        F = &StmtMap[static_cast<std::size_t>(msg::StmtKind::Return)];
        (*F)[msg::CalleeFuncInfo::Kind] = msg::StmtKind::Goto;
      } else if (auto CE = dyn_cast<clang::CallExpr>(T)) {
        auto FD = CE->getDirectCallee();
        auto DefItr{FD ? mVisibleToUser.find(FD = FD->getCanonicalDecl())
                       : mVisibleToUser.end()};
        if (DefItr != mVisibleToUser.end()) {
          F = &FuncMap[FD];
          (*F)[msg::CalleeFuncInfo::Kind] = msg::StmtKind::Call;
          (*F)[msg::CalleeFuncInfo::CalleeID] = DefItr->second->Id;
        } else {
          F = &StmtMap[static_cast<std::size_t>(msg::StmtKind::Call)];
          (*F)[msg::CalleeFuncInfo::Kind] = msg::StmtKind::Call;
        }
      }
      if (F)
        (*F)[msg::CalleeFuncInfo::StartLocation].push_back(
          getLocation(T.Stmt->getBeginLoc(), SrcMgr));
    }
    for (auto &CFI: StmtMap)
      if (CFI[msg::CalleeFuncInfo::Kind] != msg::StmtKind::Invalid)
        StmtList[msg::CalleeFuncList::Functions].push_back(std::move(CFI));
    for (auto &CFI: FuncMap)
      StmtList[msg::CalleeFuncList::Functions].push_back(std::move(CFI.second));
    return ::json::Parser<msg::CalleeFuncList>::unparseAsObject(StmtList);
  }
  return ::json::Parser<msg::CalleeFuncList>::unparseAsObject(Request);
}

std::string PrivateServerPass::answerAliasTree(llvm::Module &Module,
  const msg::AliasTree &Request) {
  for (Function &F : Module) {
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto *CU{DISub->getUnit()};
    auto *TfmCtx{dyn_cast_or_null<ClangTransformationContext>(
        mTfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance())
      continue;
    auto Decl = TfmCtx->getDeclForMangledName(F.getName());
    if (!Decl)
      continue;
    auto CanonicalFD = Decl->getCanonicalDecl()->getAsFunction();
    auto DefItr{mVisibleToUser.find(CanonicalFD)};
    if (DefItr == mVisibleToUser.end() ||
        DefItr->second->Id != Request[msg::AliasTree::FuncID])
      continue;
    if (F.isDeclaration())
      return ::json::Parser<msg::AliasTree>::unparseAsObject(Request);
    auto &SrcMgr = TfmCtx->getContext().getSourceManager();
    auto &Provider = getAnalysis<ServerPrivateProvider>(F);
    auto &LoopMatcher = Provider.get<LoopMatcherPass>().getMatcher();
    auto &MemoryMatcher = Provider.get<ClangDIMemoryMatcherPass>().getMatcher();
    if (Request[msg::AliasTree::LoopID]) {
      bcl::tagged_pair<
        bcl::tagged<clang::Stmt *, AST>,
        bcl::tagged<Loop *, IR>> Loop(nullptr, nullptr);
      for (auto Match : LoopMatcher)
        if (auto Id{toId(&Module, CU, Match.get<AST>()->getBeginLoc())};
            Id && *Id == Request[msg::AliasTree::LoopID]) {
          Loop = Match;
          break;
        }
      if (!Loop.get<AST>() || !Loop.get<IR>()->getLoopID())
        return ::json::Parser<msg::AliasTree>::unparseAsObject(Request);
      auto RF = mSocket->getAnalysis<
        DIEstimateMemoryPass, DIDependencyAnalysisPass>(F);
      assert(RF && "Dependence analysis must be available!");
      auto RM = mSocket->getAnalysis<
        AnalysisClientServerMatcherWrapper, ClonedDIMemoryMatcherWrapper>();
      assert(RM && "Client to server IR-matcher must be available!");
      auto &DIAT = RF->value<DIEstimateMemoryPass *>()->getAliasTree();
      SpanningTreeRelation<DIAliasTree *> STR(&DIAT);
      auto &DIDepInfo =
          RF->value<DIDependencyAnalysisPass *>()->getDependencies();
      auto &CToS = **RM->value<AnalysisClientServerMatcherWrapper *>();
      auto *ServerF = cast<Function>(CToS[&F]);
      auto *ClonedMemory =
        (**RM->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF];
      assert(ClonedMemory && "Memory matcher must not be null!");
      auto ServerLoopID =
          cast<MDNode>(*CToS.getMappedMD(Loop.get<IR>()->getLoopID()));
      if (!ServerLoopID)
        return ::json::Parser<msg::AliasTree>::unparseAsObject(Request);
      auto DIDepSet = DIDepInfo[ServerLoopID];
      DenseSet<const DIAliasNode *> Coverage;
      accessCoverage<bcl::SimpleInserter>(DIDepSet, DIAT, Coverage,
                                          mGlobalOpts->IgnoreRedundantMemory);
      msg::AliasTree Response;
      Response[msg::AliasTree::FuncID] = Request[msg::AliasTree::FuncID];
      Response[msg::AliasTree::LoopID] = Request[msg::AliasTree::LoopID];
      for (auto &TS : DIDepSet) {
        Response[msg::AliasTree::Nodes].emplace_back();
        auto &N = Response[msg::AliasTree::Nodes].back();
        N[msg::AliasNode::ID] = reinterpret_cast<std::uintptr_t>(TS.getNode());
        N[msg::AliasNode::Kind] = TS.getNode()->getKind();
        N[msg::AliasNode::Traits] = TS;
        for (auto &T : TS) {
          auto &M = TS.getNode() == T->getMemory()->getAliasNode()
            ? (N[msg::AliasNode::SelfMemory].emplace_back(),
              N[msg::AliasNode::SelfMemory].back())
            : (N[msg::AliasNode::CoveredMemory].emplace_back(),
              N[msg::AliasNode::CoveredMemory].back());
          llvm::raw_string_ostream AddressOS(M[msg::MemoryLocation::Address]);
          SmallVector<DebugLoc, 1> DbgLocs;
          T->getMemory()->getDebugLoc(DbgLocs);
          for (auto DbgLoc : DbgLocs)
            M[msg::MemoryLocation::Locations].push_back(
              getLocation(DbgLoc, SrcMgr));
          M[msg::MemoryLocation::Traits] = &*T;
          if (auto *ClonedDIEM = dyn_cast<DIEstimateMemory>(T->getMemory())) {
            auto MemoryItr = ClonedMemory->find<Clone>(
              const_cast<DIMemory *>(T->getMemory()));
            if (MemoryItr != ClonedMemory->end()) {
              auto *DIEM = cast<DIEstimateMemory>(MemoryItr->get<Origin>());
              auto *DIVar = DIEM->getVariable();
              auto Itr = MemoryMatcher.find<MD>(DIVar);
              if (Itr != MemoryMatcher.end()) {
                auto VD = Itr->get<AST>()->getCanonicalDecl();
                M[msg::MemoryLocation::Object][msg::SourceObject::ID] =
                    *toId(&Module, CU, VD->getLocation());
                M[msg::MemoryLocation::Object][msg::SourceObject::Name] =
                  VD->getName().str();
                M[msg::MemoryLocation::Object][msg::SourceObject::DeclLocation] =
                  getLocation(VD->getLocation(), SrcMgr);
              }
            }
            auto TmpLoc{DIMemoryLocation::get(
                const_cast<DIVariable *>(ClonedDIEM->getVariable()),
                const_cast<DIExpression *>(ClonedDIEM->getExpression()),
                nullptr, ClonedDIEM->isTemplate(),
                ClonedDIEM->isAfterPointer())};
            if (!TmpLoc.isValid()) {
              AddressOS << "sapfor.invalid";
            } else {
              if (!unparsePrint(dwarf::DW_LANG_C, TmpLoc, AddressOS))
                AddressOS << "?";
              auto Size = TmpLoc.getSize();
              if (Size.hasValue())
                M[msg::MemoryLocation::Size] = Size.getValue();
            }
          } else if (auto ClonedUM = dyn_cast<DIUnknownMemory>(T->getMemory())) {
            auto MemoryItr = ClonedMemory->find<Clone>(
              const_cast<DIMemory *>(T->getMemory()));
            auto MD = MemoryItr != ClonedMemory->end()
                ? cast<DIUnknownMemory>(MemoryItr->get<Origin>())->getMetadata()
                : ClonedUM->getMetadata();
            if (ClonedUM->isExec())
              AddressOS << "execution";
            else if (ClonedUM->isResult())
              AddressOS << "result";
            else
              AddressOS << "address";
            if (auto SubMD = dyn_cast<DISubprogram>(MD)) {
              M[msg::MemoryLocation::Object][msg::SourceObject::Name] =
                SubMD->getName().str();
              if (auto *D = TfmCtx->getDeclForMangledName(
                    SubMD->getLinkageName())) {
                auto *FD = D->getCanonicalDecl()->getAsFunction();
                SmallString<64> ExtraName;
                M[msg::MemoryLocation::Object][msg::SourceObject::Name] =
                    getFunctionName(*FD, ExtraName).str();
                if (auto DefItr{mVisibleToUser.find(FD)};
                    DefItr != mVisibleToUser.end())
                  M[msg::MemoryLocation::Object][msg::SourceObject::ID] =
                      DefItr->second->Id;
                M[msg::MemoryLocation::Object][msg::SourceObject::DeclLocation] =
                    getLocation(FD->getBeginLoc(), SrcMgr);
              } else if (!ClonedUM->isExec() && !ClonedUM->isResult()) {
                SmallString<32> Address("?");
                if (MD->getNumOperands() == 1)
                  if (auto Const =
                          dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
                    auto CInt = cast<ConstantInt>(Const->getValue());
                    Address.front() = '*';
                    CInt->getValue().toStringUnsigned(Address);
                  }
                AddressOS << Address;
              }
            }
          } else {
            AddressOS << "sapfor.invalid";
          }
          AddressOS.flush();
        }
        N[msg::AliasNode::Coverage] = Coverage.count(TS.getNode());
        for (auto &C : make_range(TS.getNode()->child_begin(),
                                  TS.getNode()->child_end())) {
          if (DIDepSet.find_as(&C) == DIDepSet.end())
            continue;
          Response[msg::AliasTree::Edges].emplace_back(N[msg::AliasNode::ID],
            reinterpret_cast<std::uintptr_t>(&C), N[msg::AliasNode::Kind]);
        }
      }
      return ::json::Parser<msg::AliasTree>::unparseAsObject(Response);
    }
  }
  return ::json::Parser<msg::AliasTree>::unparseAsObject(Request);
}

bool PrivateServerPass::runOnModule(llvm::Module &M) {
  if (!mConnection) {
    M.getContext().emitError("intrusive connection is not established");
    return false;
  }
  auto *CUs{M.getNamedMetadata("llvm.dbg.cu")};
  auto CXXCUItr{find_if(CUs->operands(), [](auto *MD) {
    auto *CU{dyn_cast<DICompileUnit>(MD)};
    return CU &&
           (isC(CU->getSourceLanguage()) || isCXX(CU->getSourceLanguage()));
  })};
  if (CXXCUItr == CUs->op_end()) {
    M.getContext().emitError(
        "cannot transform sources"
        ": transformation of C/C++ sources are only possible now");
    return false;
  }
  auto &TfmInfoPass{getAnalysis<TransformationEnginePass>()};
  mTfmInfo = TfmInfoPass ? &TfmInfoPass.get() : nullptr;
  if (!mTfmInfo) {
    M.getContext().emitError("cannot transform sources"
                             ": transformation context is not available");
    return false;
  }
  auto &SocketInfo = getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mSocket = SocketInfo.getActiveSocket();
  assert(mSocket && "Active socket must be specified!");
  mGlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  ServerPrivateProvider::initialize<TransformationEnginePass>(
    [this](TransformationEnginePass &TEP) {
      TEP.set(*mTfmInfo);
  });
  ServerPrivateProvider::initialize<AnalysisSocketImmutableWrapper>(
      [&SocketInfo](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(SocketInfo);
      });
  ServerPrivateProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [this](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*mGlobalsAA);
      });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  ServerPrivateProvider::initialize<MemoryMatcherImmutableWrapper>(
      [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
    Wrapper.set(*MMWrapper);
  });
  ServerPrivateProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
    Wrapper.setOptions(mGlobalOpts);
  });
  ServerPrivateProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [this](GlobalsAAResultImmutableWrapper &Wrapper) {
    Wrapper.set(*mGlobalsAA);
  });
  auto &DIMEnvWrapper = getAnalysis<DIMemoryEnvironmentWrapper>();
  ServerPrivateProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEnvWrapper](DIMemoryEnvironmentWrapper &Wrapper) {
    Wrapper.set(*DIMEnvWrapper);
  });
  auto &GAP = getAnalysis<GlobalsAccessWrapper>();
  if (GAP)
    ServerPrivateProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  while (mConnection->answer(
      [this, &M](const std::string &Request) -> std::string {
    msg::Diagnostic Diag(msg::Status::Error);
    if (mStdErr->isDiff()) {
      Diag[msg::Diagnostic::Terminal] += mStdErr->diff();
      return ::json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    ::json::Parser<msg::Statistic, msg::FileList, msg::LoopTree,
      msg::FunctionList, msg::CalleeFuncList, msg::AliasTree> P(Request);
    auto Obj = P.parse();
    assert(Obj && "Invalid request!");
    if (Obj->is<msg::Statistic>())
      return answerStatistic(M);
    if (Obj->is<msg::FileList>())
      return answerFileList();
    if (Obj->is<msg::LoopTree>())
      return answerLoopTree(M, Obj->as<msg::LoopTree>());
    if (Obj->is<msg::FunctionList>())
      return answerFunctionList(M);
    if (Obj->is<msg::CalleeFuncList>())
      return answerCalleeFuncList(M, Obj->as<msg::CalleeFuncList>());
    if (Obj->is<msg::AliasTree>())
      return answerAliasTree(M, Obj->as<msg::AliasTree>());
    llvm_unreachable("Unknown request to server!");
  }));
  return false;
}

void PrivateServerPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<ServerPrivateProvider>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.setPreservesAll();
}

ModulePass * llvm::createPrivateServerPass(
    bcl::IntrusiveConnection &IC, bcl::RedirectIO &StdErr) {
  return new PrivateServerPass(IC, StdErr);
}
