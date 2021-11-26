//===--- DVMHWriter.cpp ---------- DVMH Writer ----------------*- C++ -*---===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements a pass to generate a DVMH program according to
// parallel variant obtained on previous steps of parallelization.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/ExpressionMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Clang/Utils.h"
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace clang;
using namespace tsar;
using namespace tsar::dvmh;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-parallel"

namespace {
using ClangDVMHWriterProvider =
    FunctionPassAAProvider<LoopInfoWrapperPass, LoopMatcherPass,
                           ClangExprMatcherPass>;

class ClangDVMHWriter : public ModulePass {
public:
  static char ID;

  ClangDVMHWriter();

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<TransformationEnginePass>();
    AU.addRequired<ClangDVMHWriterProvider>();
    AU.addRequired<DVMHParallelizationContext>();
    AU.setPreservesAll();
  }
};

struct Insertion {
  using PragmaString = SmallString<128>;
  using PragmaList = SmallVector<std::tuple<ParallelItem *, PragmaString>, 2>;
  PragmaList Before, After;
  TransformationContextBase *TfmCtx{nullptr};

  explicit Insertion(TransformationContextBase *Ctx = nullptr) : TfmCtx{Ctx} {}
};

using LocationToPragmas = DenseMap<const Stmt *, Insertion>;
}

static std::pair<clang::Stmt *, PointerUnion<llvm::Loop *, clang::Decl *>>
findLocationToInsert(Parallelization::iterator PLocListItr,
    Parallelization::location_iterator PLocItr, const Function &F, LoopInfo &LI,
    ClangTransformationContext &TfmCtx,
    const ClangExprMatcherPass::ExprMatcher &EM,
    const LoopMatcherPass::LoopMatcher &LM) {
  if (PLocItr->Anchor.is<MDNode *>()) {
    auto *L{LI.getLoopFor(PLocListItr->get<BasicBlock>())};
    auto ID{PLocItr->Anchor.get<MDNode *>()};
    while (L->getLoopID() && L->getLoopID() != ID)
      L = L->getParentLoop();
    assert(L &&
      "A parallel directive has been attached to an unknown loop!");
    auto LMatchItr{LM.find<IR>(L)};
    assert(LMatchItr != LM.end() &&
      "Unable to find AST representation for a loop!");
    return std::pair{LMatchItr->get<AST>(), L};
  }
  assert(PLocItr->Anchor.is<Value *>() &&
         "Directives must be attached to llvm::Value!");
  if (isa<Function>(PLocItr->Anchor.get<Value *>())) {
    auto *FD{TfmCtx.getDeclForMangledName(F.getName())};
    assert(FD && "AST representation of a function must be available!");
    return std::pair{*FD->getBody()->child_begin(), FD};
  }
  auto MatchItr{EM.find<IR>(PLocItr->Anchor.get<Value *>())};
  if (MatchItr == EM.end())
    return std::pair{nullptr, nullptr};
  auto &ParentCtx{TfmCtx.getContext().getParentMapContext()};
  auto skipDecls = [&ParentCtx](auto Current) -> const Stmt * {
    for (; !Current.template get<DeclStmt>();) {
      auto Parents{ParentCtx.getParents(Current)};
      assert(!Parents.empty() &&
             "Declaration must be in declaration statement!");
      Current = *Parents.begin();
    }
    return &Current.template getUnchecked<DeclStmt>();
  };
  auto Current{MatchItr->get<AST>()};
  if (auto *D{Current.get<Decl>()})
    return std::pair{const_cast<Stmt *>(skipDecls(Current)),
                     const_cast<Decl *>(D)};
  Stmt *ToInsert{nullptr};
  if (auto *ParentStmt{Current.get<Stmt>()}) {
    for (;;) {
      ToInsert = const_cast<Stmt *>(ParentStmt);
      auto Parents{ParentCtx.getParents(*ParentStmt)};
      assert(!Parents.empty() &&
             (Parents.begin()->get<Stmt>() || Parents.begin()->get<Decl>()) &&
             "Executable statement must be in compound statement!");
      ParentStmt = Parents.begin()->get<Decl>()
                       ? skipDecls(*Parents.begin())
                       : &Parents.begin()->getUnchecked<Stmt>();
      if (isa<CompoundStmt>(ParentStmt))
        break;
      if (auto If{dyn_cast<IfStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives from Entry
        // attached to condition before the `if-stmt` and insert
        // directives from Exit at the beginning of each branch and
        // after if-stmt (if there is no `else` branch.
        if (If->getCond() == ToInsert ||
            If->getConditionVariableDeclStmt() == ToInsert)
          ToInsert = nullptr;
        break;
      }
      if (auto For{dyn_cast<ForStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives attached to another
        // parts of loops
        if (For->getBody() != ToInsert)
          ToInsert = nullptr;
        break;
      }
      if (auto While{dyn_cast<WhileStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives attached to another
        // parts of loops
        if (While->getBody() != ToInsert)
          ToInsert = nullptr;
        break;
      }
      if (auto Do{dyn_cast<DoStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives attached to another
        // parts of loops
        if (Do->getBody() != ToInsert)
          ToInsert = nullptr;
        break;
      }
    }
  }
  return std::pair{ToInsert, nullptr};
}

static bool tryToIgnoreDirectives(Parallelization::iterator PLocListItr,
    Parallelization::location_iterator PLocItr) {
  for (auto PIItr{PLocItr->Entry.begin()}, PIItrE{PLocItr->Entry.end()};
       PIItr != PIItrE; ++PIItr) {
    if (auto *PD{dyn_cast<PragmaData>(PIItr->get())}) {
      // Some data directives could be redundant.
      // So, we will emit errors later when redundant directives are
      // already ignored.
      if (!PD->isRequired()) {
        PD->invalidate();
        continue;
      }
    } else if (auto *Marker{
                   dyn_cast<ParallelMarker<PragmaData>>(PIItr->get())}) {
      auto *PD{Marker->getBase()};
      if (!PD->isRequired()) {
        PD->invalidate();
        continue;
      }
    }
    // TODO: (kaniandr@gmail.com): emit error
    LLVM_DEBUG(dbgs() << "[DVMH WRITER]: error: unable to insert on entry: "
                      << getName(static_cast<DirectiveId>((**PIItr).getKind()))
                      << "\n");
    return false;
  }
  for (auto PIItr{PLocItr->Exit.begin()}, PIItrE{PLocItr->Exit.end()};
       PIItr != PIItrE; ++PIItr) {
    if (auto *PD{dyn_cast<PragmaData>(PIItr->get())}) {
      // Some data directives could be redundant.
      // So, we will emit errors later when redundant directives are
      // already ignored.
      if (!PD->isRequired()) {
        PD->invalidate();
        continue;
      }
    } else if (auto *Marker{
                   dyn_cast<ParallelMarker<PragmaData>>(PIItr->get())}) {
      auto *PD{Marker->getBase()};
      if (!PD->isRequired()) {
        PD->invalidate();
        continue;
      }
    }
    // TODO: (kaniandr@gmail.com): emit error
    LLVM_DEBUG(dbgs() << "[DVMH WRITER]: error: unable to insert on exit: "
                      << getName(static_cast<DirectiveId>((**PIItr).getKind()))
                      << "\n");
    return false;
  }
  return true;
}

static inline void addVarList(const std::set<std::string> &VarInfoList,
                              SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I{ VarInfoList.begin() }, EI{ VarInfoList.end() };
  Clause.append(I->begin(), I->end());
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    Clause.append(I->begin(), I->end());
  }
  Clause.push_back(')');
}

static inline void addVar(const VariableT &V,
                              SmallVectorImpl<char> &Clause) {
  auto Name{V.get<AST>()->getName()};
  Clause.append(Name.begin(), Name.end());
}

template<typename FunctionT>
static void addVar(const dvmh::Align &A, FunctionT &&getIdxName,
            SmallVectorImpl<char> &Str) {
  std::visit(
      [&Str](auto &&V) {
        if constexpr (std::is_same_v<VariableT, std::decay_t<decltype(V)>>) {
          auto Name{V.template get<AST>()->getName()};
          Str.append(Name.begin(), Name.end());
        } else {
          auto Name{V->getName()};
          Str.append(Name.begin(), Name.end());
        }
      },
      A.Target);
  for (auto &A : A.Relation) {
    Str.push_back('[');
    if (A) {
      std::visit(
          [&getIdxName, &Str](auto &&V) {
            if constexpr (std::is_same_v<dvmh::Align::Axis,
                                         std::decay_t<decltype(V)>>) {
              SmallString<8> Name;
              getIdxName(V.Dimension, Name);
              if (!V.Step.isOneValue()) {
                if (V.Step.isNegative())
                  Str.push_back('(');
                V.Step.toString(Str);
                if (V.Step.isNegative())
                  Str.push_back(')');
                Str.push_back('*');
              }
              Str.append(Name.begin(), Name.end());
              if (!V.Offset.isNullValue()) {
                Str.push_back('+');
                V.Offset.toString(Str);
              }
            } else {
              V.toString(Str);
            }
          },
          *A);
    }
    Str.push_back(']');
  }
}

template<typename FunctionT>
static void addVar1(const dvmh::Align &A, FunctionT &&getIdxName,
                    SmallVectorImpl<char> &Str) {
  addVar(A, getIdxName, Str);
}

static inline unsigned addVarList(const SortedVarListT &VarInfoList,
                                  SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I{VarInfoList.begin()}, EI{VarInfoList.end()};
  addVar(*I, Clause);
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    addVar(*I, Clause);
  }
  Clause.push_back(')');
  return Clause.size();
}

static inline unsigned addVarList(const AlignVarListT &VarInfoList,
                                  SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I{VarInfoList.begin()}, EI{VarInfoList.end()};
  addVar(
      *I,
      [](unsigned Dim, SmallVectorImpl<char> &Name) {
        ("I" + Twine(Dim)).toVector(Name);
      },
      Clause);
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    addVar(
        *I,
        [](unsigned Dim, SmallVectorImpl<char> &Name) {
          ("I" + Twine(Dim)).toVector(Name);
        },
        Clause);
  }
  Clause.push_back(')');
  return Clause.size();
}

template <typename FilterT>
static inline unsigned addVarList(const AlignVarListT &VarInfoList,
    FilterT &&F, SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
  Clause.push_back('(');
  auto Itr{VarInfoList.begin()}, ItrE{VarInfoList.end()};
  for (; Itr != ItrE && !F(*Itr); ++Itr)
    ;
  if (Itr == ItrE) {
    Clause.push_back(')');
    return Count;
  }
  addVar(
      *Itr,
      [](unsigned Dim, SmallVectorImpl<char> &Name) {
        ("I" + Twine(Dim)).toVector(Name);
      },
      Clause);
  ++Count;
  for (++Itr; Itr != ItrE; ++Itr)
    if (F(*Itr)) {
      Clause.append({',', ' '});
      addVar(
          *Itr,
          [](unsigned Dim, SmallVectorImpl<char> &Name) {
            ("I" + Twine(Dim)).toVector(Name);
          },
          Clause);
      ++Count;
    }
  Clause.push_back(')');
  return Count;
}

static inline void addClauseIfNeed(StringRef Name, const SortedVarListT &Vars,
    SmallVectorImpl<char> &PragmaStr) {
  if (!Vars.empty()) {
    PragmaStr.append(Name.begin(), Name.end());
    addVarList(Vars, PragmaStr);
  }
}

/// Add clauses for all reduction variables from a specified list to
/// the end of `ParallelFor` pragma.
static void addReductionIfNeed(
    const ClangDependenceAnalyzer::ReductionVarListT &VarInfoList,
    SmallVectorImpl<char> &ParallelFor) {
  unsigned I = trait::Reduction::RK_First;
  unsigned EI = trait::Reduction::RK_NumberOf;
  for (; I < EI; ++I) {
    if (VarInfoList[I].empty())
      continue;
    SmallString<7> RedKind;
    switch (static_cast<trait::Reduction::Kind>(I)) {
    case trait::Reduction::RK_Add: RedKind += "sum"; break;
    case trait::Reduction::RK_Mult: RedKind += "product"; break;
    case trait::Reduction::RK_Or: RedKind += "or"; break;
    case trait::Reduction::RK_And: RedKind += "and"; break;
    case trait::Reduction::RK_Xor: RedKind += "xor"; break;
    case trait::Reduction::RK_Max: RedKind += "max"; break;
    case trait::Reduction::RK_Min: RedKind += "min"; break;
    default: llvm_unreachable("Unknown reduction kind!"); break;
    }
    ParallelFor.append({ 'r', 'e', 'd', 'u', 'c', 't', 'i', 'o', 'n' });
    ParallelFor.push_back('(');
    auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
    ParallelFor.append(RedKind.begin(), RedKind.end());
    ParallelFor.push_back('(');
    auto VarName{ VarItr->get<AST>()->getName() };
    ParallelFor.append(VarName.begin(), VarName.end());
    ParallelFor.push_back(')');
    for (++VarItr; VarItr != VarItrE; ++VarItr) {
      ParallelFor.push_back(',');
      ParallelFor.append(RedKind.begin(), RedKind.end());
      ParallelFor.push_back('(');
      auto VarName{ VarItr->get<AST>()->getName() };
      ParallelFor.append(VarName.begin(), VarName.end());
      ParallelFor.push_back(')');
    }
    ParallelFor.push_back(')');
  }
}

static void addParallelMapping(Loop &L, const PragmaParallel &Parallel,
                               SmallVectorImpl<char> &PragmaStr) {
  if (Parallel.getClauses().get<trait::DirectAccess>().empty())
    return;
  PragmaStr.push_back('(');
  for (auto &LToI : Parallel.getClauses().get<trait::Induction>()) {
    PragmaStr.push_back('[');
    auto Name{LToI.get<VariableT>().get<AST>()->getName()};
    PragmaStr.append(Name.begin(), Name.end());
    PragmaStr.push_back(']');
  }
  PragmaStr.push_back(')');
  // We sort arrays to ensure the same order of variables after
  // different launches of parallelization.
  std::set<std::string, std::less<std::string>> MappingStr;
  for (auto &[Var, Mapping] : Parallel.getClauses().get<trait::DirectAccess>()) {
    if (Mapping.empty())
      continue;
    SmallString<32> Tie{Var.get<AST>()->getName()};
    for (auto &Map : Mapping) {
      Tie += "[";
      if (Map.first) {
        if (!Map.second)
          Tie += "-";
        Tie += find_if(Parallel.getClauses().get<trait::Induction>(),
                       [&Map](auto &LToI) {
                         return LToI.template get<Loop>() == Map.first;
                       })
                   ->get<VariableT>()
                   .get<AST>()
                   ->getName();
      }
      Tie += "]";
    }
    MappingStr.insert(std::string(Tie));
  }
  if (MappingStr.empty())
    return;
  PragmaStr.append({ ' ', 't', 'i', 'e' });
  addVarList(MappingStr, PragmaStr);
}

static void pragmaRealignStr(const ParallelItemRef &PIRef,
                             SmallVectorImpl<char> &Str) {
  auto Realign{cast<PragmaRealign>(PIRef)};
  getPragmaText(DirectiveId::DvmRealign, Str);
  Str.resize(Str.size() - 1);
  Str.push_back('(');
  auto WhatName{Realign->what().get<AST>()->getName()};
  Str.append(WhatName.begin(), WhatName.end());
  StringRef IdxPrefix{"iEX"};
  for (unsigned I = 0, EI = Realign->getWhatDimSize(); I < EI; ++I) {
    Str.push_back('[');
    if (auto Itr{find_if(Realign->with().Relation,
                         [I](const auto &A) {
                           if (!A)
                             return false;
                           if (auto *V{std::get_if<dvmh::Align::Axis>(&*A)})
                             return V->Dimension == I;
                           return false;
                         })};
        Itr != Realign->with().Relation.end()) {
      Str.append(IdxPrefix.begin(), IdxPrefix.end());
      SmallString<2> SuffixData;
      auto Suffix{Twine(I).toStringRef(SuffixData)};
      Str.append(Suffix.begin(), Suffix.end());
    }
    Str.push_back(']');
  }
  Str.append({' ', 'w', 'i', 't', 'h', ' '});
  addVar(
      Realign->with(),
      [IdxPrefix](unsigned Dim, SmallVectorImpl<char> &Str) {
        (IdxPrefix + Twine(Dim)).toVector(Str);
      },
      Str);
  Str.push_back(')');
}

static void pragmaParallelStr(const ParallelItemRef &PIRef, Loop &L,
                              SmallVectorImpl<char> &Str) {
  auto Parallel{cast<PragmaParallel>(PIRef)};
  getPragmaText(DirectiveId::DvmParallel, Str);
  Str.resize(Str.size() - 1);
  if (Parallel->getClauses().get<dvmh::Align>()) {
    Str.push_back('(');
    for (auto &LToI : Parallel->getClauses().get<trait::Induction>()) {
      Str.push_back('[');
      auto Name{LToI.get<VariableT>().get<AST>()->getName()};
      Str.append(Name.begin(), Name.end());
      Str.push_back(']');
    }
    Str.append({' ', 'o', 'n', ' '});
    addVar(
        *Parallel->getClauses().get<dvmh::Align>(),
        [&Parallel](unsigned Dim, SmallVectorImpl<char> &Str) {
          auto &Induct{Parallel->getClauses()
                           .get<trait::Induction>()[Dim]
                           .template get<VariableT>()};
          auto Name{Induct.template get<AST>()->getName()};
          Str.append(Name.begin(), Name.end());
        },
        Str);
    Str.push_back(')');
  } else if (Parallel->getClauses().get<trait::DirectAccess>().empty()) {
    Str.push_back('(');
    auto NestSize{
        std::to_string(Parallel->getClauses().get<trait::Induction>().size())};
    Str.append(NestSize.begin(), NestSize.end());
    Str.push_back(')');
  } else {
    addParallelMapping(L, *Parallel, Str);
  }
  auto addShadow = [&Str](auto &Shadow) {
    Str.append(Shadow.first.template get<AST>()->getName().begin(),
               Shadow.first.template get<AST>()->getName().end());
    for (auto &Range :
         Shadow.second.template get<trait::DIDependence::DistanceVector>()) {
      Str.push_back('[');
      if (Range.first)
        Range.first->toString(Str);
      else
        Str.push_back('0');
      Str.push_back(':');
      if (Range.second)
        Range.second->toString(Str);
      else
        Str.push_back('0');
      Str.push_back(']');
    }
    if (Shadow.second.template get<Corner>())
      Str.append({'(', 'c', 'o', 'r', 'n', 'e', 'r', ')'});
  };
  if (!Parallel->getClauses().get<Shadow>().empty()) {
    Str.append(
        {'s', 'h', 'a', 'd', 'o', 'w', '_', 'r', 'e', 'n', 'e', 'w', '('});
    for (auto &Shadow : Parallel->getClauses().get<Shadow>()) {
      addShadow(Shadow);
      Str.push_back(',');
    }
    Str.pop_back();
    Str.push_back(')');
  }
  if (!Parallel->getClauses().get<trait::Dependence>().empty()) {
    Str.append({'a', 'c', 'r', 'o', 's', 's', '('});
    for (auto &Across : Parallel->getClauses().get<trait::Dependence>()) {
      addShadow(Across);
      Str.push_back(',');
    }
    Str.pop_back();
    Str.push_back(')');
  }
  if (!Parallel->getClauses().get<Remote>().empty()) {
    Str.append(
        {'r', 'e', 'm', 'o', 't', 'e', '_', 'a', 'c', 'c', 'e', 's', 's', '('});
    for (auto &R : Parallel->getClauses().get<Remote>()) {
      addVar(
          R,
          [&Parallel](unsigned Dim, SmallVectorImpl<char> &Str) {
            auto &Induct{Parallel->getClauses()
                             .get<trait::Induction>()[Dim]
                             .template get<VariableT>()};
            auto Name{Induct.template get<AST>()->getName()};
            Str.append(Name.begin(), Name.end());
          },
          Str);
      Str.push_back(',');
    }
    Str.pop_back();
    Str.push_back(')');
  }
  addClauseIfNeed(" private", Parallel->getClauses().get<trait::Private>(),
                  Str);
  addReductionIfNeed(Parallel->getClauses().get<trait::Reduction>(), Str);
}

static void pragmaRegionStr(const ParallelItemRef &PIRef,
                            SmallVectorImpl<char> &Str) {
  auto R{cast<PragmaRegion>(PIRef)};
  getPragmaText(DirectiveId::DvmRegion, Str);
  Str.resize(Str.size() - 1);
  addClauseIfNeed(" in", R->getClauses().get<trait::ReadOccurred>(), Str);
  addClauseIfNeed(" out", R->getClauses().get<trait::WriteOccurred>(), Str);
  addClauseIfNeed(" local", R->getClauses().get<trait::Private>(), Str);
  if (R->isHostOnly()) {
    StringRef Targets{" targets(HOST)"};
    Str.append(Targets.begin(), Targets.end());
  }
  Str.push_back('\n');
  Str.push_back('{');
}

static void
addPragmaToStmt(const Stmt *ToInsert,
                PointerUnion<llvm::Loop *, clang::Decl *> Scope,
                Parallelization::iterator PLocListItr,
                Parallelization::location_iterator PLocItr,
                TransformationContextBase *TfmCtx,
                DenseMap<ParallelItemRef, const Stmt *> &DeferredPragmas,
                std::vector<ParallelItem *> &NotOptimizedPragmas,
                LocationToPragmas &PragmasToInsert) {
  auto PragmaLoc{ToInsert ? PragmasToInsert.try_emplace(ToInsert, TfmCtx).first
                          : PragmasToInsert.end()};
  for (auto PIItr{PLocItr->Entry.begin()}, PIItrE{PLocItr->Entry.end()};
       PIItr != PIItrE; ++PIItr) {
    ParallelItemRef PIRef{PLocListItr, PLocItr, PIItr, true};
    SmallString<128> PragmaStr;
    if (isa<PragmaParallel>(PIRef)) {
      pragmaParallelStr(PIRef, *Scope.get<Loop *>(), PragmaStr);
    } else if (isa<PragmaRegion>(PIRef)) {
      pragmaRegionStr(PIRef, PragmaStr);
    } else if (isa<PragmaRealign>(PIRef)) {
      pragmaRealignStr(PIRef, PragmaStr);
    } else if (isa<PragmaData>(PIRef) ||
               isa<ParallelMarker<PragmaData>>(PIRef)) {
      // Even if this directive cannot be inserted (it is invalid) it should
      // be processed later. If it is replaced with some other directives,
      // this directive changes status to CK_Skip. The new status may allow us
      // to ignore some other directives later.
      DeferredPragmas.try_emplace(PIRef, ToInsert);
      if (auto *PD{dyn_cast<PragmaData>(PIRef)}; PD && PD->parent_empty())
        NotOptimizedPragmas.push_back(PD);
      // Mark the position of the directive in the source code. It will be later
      // created their only if necessary.
      if (ToInsert)
        PragmaLoc->second.Before.emplace_back(PIItr->get(), "");
      continue;
    } else {
      llvm_unreachable("An unknown pragma has been attached to a loop!");
    }
    PragmaStr += "\n";
    assert(ToInsert && "Insertion location must be known!");
    PragmaLoc->second.Before.emplace_back(PIItr->get(), std::move(PragmaStr));
  }
  if (PLocItr->Exit.empty())
    return;
  for (auto PIItr{PLocItr->Exit.begin()}, PIItrE{PLocItr->Exit.end()};
       PIItr != PIItrE; ++PIItr) {
    ParallelItemRef PIRef{PLocListItr, PLocItr, PIItr, false};
    SmallString<128> PragmaStr{"\n"};
    if (isa<PragmaRealign>(PIRef)) {
      pragmaRealignStr(PIRef, PragmaStr);
    } else if (auto *Marker{dyn_cast<ParallelMarker<PragmaRegion>>(PIRef)}) {
      PragmaStr = "}";
    } else if (isa<PragmaData>(PIRef) ||
               isa<ParallelMarker<PragmaData>>(PIRef)) {
      DeferredPragmas.try_emplace(PIRef, ToInsert);
      if (auto *PD{dyn_cast<PragmaData>(PIRef)}; PD && PD->parent_empty())
        NotOptimizedPragmas.push_back(PD);
      if (ToInsert)
        PragmaLoc->second.After.emplace_back(PIItr->get(), "");
      continue;
    } else {
      llvm_unreachable("An unknown pragma has been attached to a loop!");
    }
    assert(ToInsert && "Insertion location must be known!");
    PragmaLoc->second.After.emplace_back(PIItr->get(), std::move(PragmaStr));
  }
}

template<typename VisitorT>
static void traversePragmaDataPO(ParallelItem *PI,
    SmallPtrSetImpl<ParallelItem *> &Visited, VisitorT &&POVisitor) {
  if (!Visited.insert(PI).second)
    return;
  if (auto *PD{dyn_cast<PragmaData>(PI)}) {
    for (auto *Child : PD->children())
      traversePragmaDataPO(Child, Visited, std::forward<VisitorT>(POVisitor));
  }
  POVisitor(PI);
}

// This implements a post-ordered traversal of a forest of data transfer
// directives and applies a specified function to an each directive being
// visited.
template<typename VisitorT>
static inline void traversePragmaDataPO(ArrayRef<ParallelItem *> Roots,
    VisitorT &&POVisitor) {
  SmallPtrSet<ParallelItem *, 32> Visited;
  for (auto *PI : Roots)
    traversePragmaDataPO(PI, Visited, std::forward<VisitorT>(POVisitor));
}

template <typename FilterT>
static bool pragmaDataStr(FilterT Filter, const ParallelItemRef &PDRef,
    SmallVectorImpl<char> &Str) {
  auto PD{cast_or_null<PragmaData>(PDRef)};
  if (!PD || PD->getMemory().empty())
    return false;
  getPragmaText(static_cast<DirectiveId>(PD->getKind()), Str);
  // Remove the last '\n'.
  Str.pop_back();
  struct OnReturnT {
    ~OnReturnT() {
      if (isa<PragmaRemoteAccess>(PDRef))
        Str.append({'\n', '{'});
    }
    const ParallelItemRef &PDRef;
    SmallVectorImpl<char> &Str;
  } OnReturn{PDRef, Str};
  if constexpr (std::is_same_v<decltype(Filter), std::true_type>)
    addVarList(PD->getMemory(), Str);
  else if (addVarList(PD->getMemory(), std::move(Filter), Str) == 0)
    return false;
  return true;
}

static inline bool pragmaDataStr(ParallelItemRef &PDRef,
    SmallVectorImpl<char> &Str) {
  return pragmaDataStr(std::true_type{}, PDRef, Str);
}

static void
insertPragmaData(ArrayRef<PragmaData *> POTraverse,
                 DenseMap<ParallelItemRef, const Stmt *> DeferredPragmas,
                 LocationToPragmas &PragmasToInsert) {
  for (auto *PD : POTraverse) {
    // Do not skip directive (even if it is marked as skipped) if its children
    // are invalid.
    if (PD->isInvalid() || PD->isSkipped())
      continue;
    auto &&[PIRef, ToInsert] = *DeferredPragmas.find_as(PD);
    assert(PragmasToInsert.count(ToInsert) &&
           "Pragma position must be cached!");
    auto &Position{PragmasToInsert[ToInsert]};
    if (PIRef.isOnEntry()) {
      auto BeforeItr{
          find_if(Position.Before, [PI = PIRef.getPI()->get()](auto &Pragma) {
            return std::get<ParallelItem *>(Pragma) == PI;
          })};
      assert(BeforeItr != Position.Before.end() &&
             "Pragma position must be cached!");
      auto &PragmaStr{std::get<Insertion::PragmaString>(*BeforeItr)};
      if (auto *DS{dyn_cast<DeclStmt>(ToInsert)})
        pragmaDataStr(
            [DS](const dvmh::Align &A) {
              if (auto *V{std::get_if<VariableT>(&A.Target)})
                return !is_contained(DS->getDeclGroup(), V->get<AST>());
              return true;
            },
            PIRef, PragmaStr);
      else
        pragmaDataStr(PIRef, PragmaStr);
      PragmaStr += "\n";
    } else {
      auto AfterItr{
          find_if(Position.After, [PI = PIRef.getPI()->get()](auto &Pragma) {
            return std::get<ParallelItem *>(Pragma) == PI;
          })};
      assert(AfterItr != Position.After.end() &&
             "Pragma position must be cached!");
      auto &PragmaStr{std::get<Insertion::PragmaString>(*AfterItr)};
      PragmaStr += "\n";
      pragmaDataStr(PIRef, PragmaStr);
    }
  }
}

static inline clang::SourceLocation
shiftTokenIfSemi(clang::SourceLocation Loc, const clang::ASTContext &Ctx) {
  Token SemiTok;
  return (!getRawTokenAfter(Loc, Ctx.getSourceManager(), Ctx.getLangOpts(),
                            SemiTok) &&
          SemiTok.is(tok::semi))
             ? SemiTok.getLocation()
             : Loc;
}

static void printReplacementTree(
    ArrayRef<ParallelItem *> NotOptimizedPragmas,
    const DenseMap<ParallelItemRef, const Stmt *> &DeferredPragmas,
    LocationToPragmas &PragmasToInsert) {
  traversePragmaDataPO(
      NotOptimizedPragmas,
      [&DeferredPragmas, &PragmasToInsert](ParallelItem *PI) {
        auto PD{cast<PragmaData>(PI)};
        dbgs() << "id: " << PD << " ";
        dbgs() << "skiped: " << PD->isSkipped() << " ";
        dbgs() << "invalid: " << PD->isInvalid() << " ";
        dbgs() << "final: " << PD->isFinal() << " ";
        dbgs() << "required: " << PD->isRequired() << " ";
        if (isa<PragmaActual>(PD))
          dbgs() << "actual: ";
        else if (isa<PragmaGetActual>(PD))
          dbgs() << "get_actual: ";
        if (PD->getMemory().empty())
          dbgs() << "- ";
        for (auto &Align : PD->getMemory()) {
          SmallString<16> Var;
          addVar(
              Align,
              [](unsigned Dim, SmallVectorImpl<char> &Str) {
                ("I" + Twine(Dim)).toVector(Str);
              },
              Var);
          dbgs() << Var << " ";
        }
        if (!PD->child_empty()) {
          dbgs() << "children: ";
          for (auto *Child : PD->children())
            dbgs() << Child << " ";
        }
        if (auto PIItr{DeferredPragmas.find_as(PI)};
            PIItr != DeferredPragmas.end()) {
          auto [PIRef, ToInsert] = *PIItr;
          if (ToInsert) {
            dbgs() << "location: ";
            if (auto TfmCtx{dyn_cast<ClangTransformationContext>(
                    PragmasToInsert[ToInsert].TfmCtx)})
              ToInsert->getBeginLoc().print(
                  dbgs(), TfmCtx->getContext().getSourceManager());
            else
              llvm_unreachable("Unsupported type of a "
                               "transformation context!");
          }
        } else {
          dbgs() << "stub ";
        }
        dbgs() << "\n";
      });
}

bool ClangDVMHWriter::runOnModule(llvm::Module &M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  ClangDVMHWriterProvider::initialize<TransformationEnginePass>(
      [&TfmInfo](TransformationEnginePass &Wrapper) {
        Wrapper.set(TfmInfo.get());
      });
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: insert data transfer directives\n");
  std::vector<ParallelItem *> NotOptimizedPragmas;
  DenseMap<ParallelItemRef, const Stmt *> DeferredPragmas;
  LocationToPragmas PragmasToInsert;
  auto &PIP{getAnalysis<DVMHParallelizationContext>()};
  for (auto F : make_range(PIP.getParallelization().func_begin(),
                           PIP.getParallelization().func_end())) {
    LLVM_DEBUG(dbgs() << "[DVMH SM]: process function " << F->getName()
                      << "\n");
    auto emitTfmError = [F]() {
      F->getContext().emitError("cannot transform sources: transformation "
                                "context is not available for the '" +
                                F->getName() + "' function");
    };
    auto *DISub{findMetadata(F)};
    if (!DISub) {
      emitTfmError();
      return false;
    }
    auto *CU{DISub->getUnit()};
    if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage())) {
      emitTfmError();
      return false;
    }
    auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                               TfmInfo->getContext(*CU))
                         : nullptr};
    if (!TfmCtx || !TfmCtx->hasInstance()) {
      emitTfmError();
      return false;
    }
    auto &Provider{getAnalysis<ClangDVMHWriterProvider>(*F)};
    auto &LI{Provider.get<LoopInfoWrapperPass>().getLoopInfo()};
    auto &LM{Provider.get<LoopMatcherPass>().getMatcher()};
    auto &EM{Provider.get<ClangExprMatcherPass>().getMatcher()};
    for (auto &BB : *F) {
      auto PLocListItr{PIP.getParallelization().find(&BB)};
      if (PLocListItr == PIP.getParallelization().end())
        continue;
      for (auto PLocItr{PLocListItr->get<ParallelLocation>().begin()},
           PLocItrE{PLocListItr->get<ParallelLocation>().end()};
           PLocItr != PLocItrE; ++PLocItr) {
        auto [ToInsert, Scope] =
            findLocationToInsert(PLocListItr, PLocItr, *F, LI, *TfmCtx, EM, LM);
          LLVM_DEBUG(
            if (!ToInsert) {
              dbgs() << "[DVMH WRITER]: unable to insert directive to: ";
              if (auto *MD{PLocItr->Anchor.dyn_cast<MDNode *>()}) {
                MD->print(dbgs());
              } else if (auto *V{PLocItr->Anchor.dyn_cast<Value *>()}) {
                if (auto *F{dyn_cast<Function>(V)}) {
                  dbgs() << " function " << F->getName();
                } else {
                  V->print(dbgs());
                  if (auto *I{dyn_cast<Instruction>(V)})
                    dbgs() << " (function "
                           << I->getFunction()->getName() << ")";
                }
              }
              dbgs() << "\n";
            }
          );
        if (!ToInsert && !tryToIgnoreDirectives(PLocListItr, PLocItr))
          return false;
        addPragmaToStmt(ToInsert, Scope, PLocListItr, PLocItr, TfmCtx,
                        DeferredPragmas, NotOptimizedPragmas, PragmasToInsert);
      }
    }
  }
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: IPO root ID: " << &PIP.getIPORoot()
                    << "\n";
             dbgs() << "[DVMH WRITER]: initial replacement tree:\n";
             printReplacementTree(NotOptimizedPragmas, DeferredPragmas,
                                  PragmasToInsert));
  std::vector<PragmaData *> POTraverse;
  // Optimize CPU-to-GPU data transfer. Try to skip unnecessary directives.
  // We use post-ordered traversal to propagate the `skip` property in upward
  // direction.
  traversePragmaDataPO(NotOptimizedPragmas, [this, &PIP, &DeferredPragmas,
                                             &POTraverse](ParallelItem *PI) {
    auto PD{cast<PragmaData>(PI)};
    POTraverse.push_back(PD);
    if (PD->isSkipped())
      return;
    bool IsRedundant{!PD->children().empty()};
    for (auto *Child : PD->children()) {
      auto ChildPD{cast<PragmaData>(Child)};
      IsRedundant &= (!ChildPD->isInvalid() || ChildPD->isSkipped());
    }
    if (IsRedundant && !PD->isFinal() && !PD->isRequired()) {
      PD->skip();
      return;
    }
    // Enable IPO only if all children of the IPO root is valid.
    if (PI == &PIP.getIPORoot()) {
      if (!PD->children().empty())
        PIP.getIPORoot().invalidate();
      else
        PIP.getIPORoot().skip();
      return;
    }
    auto PIItr{DeferredPragmas.find_as(PI)};
    assert(PIItr != DeferredPragmas.end() &&
           "Internal pragmas must be cached!");
    auto [PIRef, ToInsert] = *PIItr;
    if (PD->getMemory().empty()) {
      PD->skip();
    } else if (PIRef.isOnEntry()) {
      // Do not mention variables in a directive if it has not been
      // declared yet.
      if (auto *DS{dyn_cast_or_null<DeclStmt>(ToInsert)};
          DS && all_of(PD->getMemory(), [DS](const dvmh::Align &Align) {
            if (auto *V{std::get_if<VariableT>(&Align.Target)})
              return is_contained(DS->getDeclGroup(), V->get<AST>());
            return false;
          }))
        PD->skip();
    }
  });
  bool IsOk{true}, IsAllOptimized{true};
  // Now we check that there is any way to actualize data for each parallel
  // region.
  for (auto *PI : NotOptimizedPragmas)
    if (!cast<PragmaData>(PI)->isSkipped()) {
      IsAllOptimized = false;
      if (cast<PragmaData>(PI)->isInvalid()) {
        IsOk = false;
        // TODO: (kaniandr@gmail.com): emit error
      }
    }
  if (!IsOk)
    return false;
  // TODO (kaniandr@gmail.com): at this moment we either makes a full IPO or
  // disable all possible optimization. Try to find a valid combination of
  // directives which yields a data transfer optimization. Is this search
  // necessary or if a final directive is invalid, some source-to-source
  // transformations can be always made to insert this directive successfully.
  //
  // We have to look up for the lowest valid levels in the forest of directives
  // and invalidate all directives below these levels.
  //
  // Let's consider the following not optimized yet example:
  //
  // function {
  //   actual
  //   region_1
  //   get_actual
  //   loop {
  //     actual
  //     region_2
  //     get_actual
  //   }
  // }
  // Optimization step produces the following forest of possible data transfer
  // directives:
  //              r2_a  r2_ga
  // r1_a r1_ga   l_a  l_ga  loop_internal_directives
  // f_a f_ga
  //
  // If we cannot insert some of directives from the
  // 'loop_internal_directives' list, we have to invalidate l_a, l_ga,
  // loop_internal_direcitves and also f_a and f_ga directives. Otherwise we
  // obtain the following optimized version which is not correct (we miss
  // directives surrounding the first region because f_a and f_ga suppress
  // them):
  // function {
  // f_a
  //   region_1
  //   loop {
  //     actual
  //     region_2
  //     get_actual
  //   }
  // f_ga
  // }
  //
  // Unfortunately, if we invalidate some directives in a function it may
  // transitively invalidate directives in another function. It is not clear
  // how to find a consistent combination of directives, so we disable
  // optimization if some of the leaf directives cannot be inserted.
  if (!IsAllOptimized) {
    for (auto *PD : llvm::reverse(POTraverse)) {
      if (PD->parent_empty())
        PD->actualize();
      else
        PD->skip();
    }
  }
  for (auto &&[Position, Insertion] : PragmasToInsert) {
    if (Insertion.Before.empty())
      continue;
    stable_sort(Insertion.Before, [](auto &LHS, auto &RHS) {
      return isa<PragmaRemoteAccess>(std::get<ParallelItem *>(LHS)) &&
        !isa<PragmaRemoteAccess>(std::get<ParallelItem *>(RHS));
    });
    if (auto Remote{dyn_cast<PragmaRemoteAccess>(
            std::get<ParallelItem *>(*Insertion.Before.begin()))}) {
      for (auto I{Insertion.Before.begin() + 1}, EI{Insertion.Before.end()};
           I != EI && isa<PragmaRemoteAccess>(std::get<ParallelItem *>(*I));
           ++I) {
        auto R{cast<PragmaRemoteAccess>(std::get<ParallelItem *>(*I))};
        Remote->getMemory().insert(R->getMemory().begin(),
                                   R->getMemory().end());
        // TODO (kaniandr@gmail.com): Do not remove this remote_access
        // directive. It cannot be removed from ParalleizationInfo, otherwise
        // the DefferedPragmas map will be invalidated because it uses iterator
        // as a key. May be we should not store iterators in DefferedPragmas
        // map. Note, that each value from the DefferedPragmas map should be
        // stored in the PragmasToInsert collection, so do not remote this
        // remote_access directive from the Insertion.Before.
        Remote->child_insert(R);
        R->parent_insert(Remote);
        R->skip();
      }
    }
  }

  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: optimized replacement tree:\n";
             printReplacementTree(NotOptimizedPragmas, DeferredPragmas,
                                  PragmasToInsert));
  // Build pragmas for necessary data transfer directives.
  insertPragmaData(POTraverse, DeferredPragmas, PragmasToInsert);
  for (auto &&[PIRef, ToInsert] : DeferredPragmas)
    if (ToInsert)
      if (auto *Marker{dyn_cast<ParallelMarker<PragmaRemoteAccess>>(PIRef)})
        if (auto *Remote{Marker->getBase()};
            !Remote->isInvalid() && !Remote->isSkipped())
          PragmasToInsert[ToInsert].After.emplace_back(Marker, "\n}");
  // Update sources.
  for (auto &&[ToInsert, Pragmas] : PragmasToInsert) {
    auto BeginLoc{ToInsert->getBeginLoc()};
    auto TfmCtx{cast<ClangTransformationContext>(Pragmas.TfmCtx)};
    bool IsBeginChanged{false};
    for (auto &&[PI, Str] : Pragmas.Before)
      if (!Str.empty()) {
        if (!IsBeginChanged) {
          auto &SM{TfmCtx->getRewriter().getSourceMgr()};
          auto Identation{Lexer::getIndentationForLine(BeginLoc, SM)};
          bool Invalid{false};
          auto Column{SM.getPresumedColumnNumber(BeginLoc, &Invalid)};
          if (Invalid || Column > Identation.size() + 1)
            TfmCtx->getRewriter().InsertTextAfter(BeginLoc, "\n");
        }
        TfmCtx->getRewriter().InsertTextAfter(BeginLoc, Str);
        IsBeginChanged = true;
      }
    auto EndLoc{shiftTokenIfSemi(ToInsert->getEndLoc(), TfmCtx->getContext())};
    bool IsEndChanged{false};
    for (auto &&[PI, Str] : Pragmas.After)
      if (!Str.empty()) {
        TfmCtx->getRewriter().InsertTextAfterToken(EndLoc, Str);
        IsEndChanged = true;
      }
    if (IsBeginChanged || IsEndChanged) {
      bool HasEndNewline{false};
      if (IsEndChanged) {
        auto &Ctx{TfmCtx->getContext()};
        auto &SM{Ctx.getSourceManager()};
        Token NextTok;
        bool IsEndInvalid{false}, IsNextInvalid{false};
        if (getRawTokenAfter(EndLoc, SM, Ctx.getLangOpts(), NextTok) ||
            SM.getPresumedLineNumber(NextTok.getLocation(), &IsNextInvalid) ==
                SM.getPresumedLineNumber(EndLoc, &IsEndInvalid) ||
            IsNextInvalid || IsEndInvalid) {
          TfmCtx->getRewriter().InsertTextAfterToken(EndLoc, "\n");
          HasEndNewline = true;
        }
      }
      auto &ParentCtx{TfmCtx->getContext().getParentMapContext()};
      auto Parents{ParentCtx.getParents(*ToInsert)};
      assert(!Parents.empty() && "Statement must be inside a function body!");
      if (!Parents.begin()->get<CompoundStmt>()) {
        TfmCtx->getRewriter().InsertTextBefore(BeginLoc, "{\n");
        TfmCtx->getRewriter().InsertTextAfterToken(EndLoc,
                                                   HasEndNewline ? "}" : "\n}");
      }
    }
  }
  return false;
}

INITIALIZE_PROVIDER(ClangDVMHWriterProvider, "clang-dvmh-writer-provider",
  "DVMH Parallelization (Provider, Writer, Clang)")

ClangDVMHWriter::ClangDVMHWriter() : ModulePass(ID) {
  initializeClangDVMHWriterProviderPass(*PassRegistry::getPassRegistry());
  initializeClangDVMHWriterPass(*PassRegistry::getPassRegistry());
}

char ClangDVMHWriter::ID = 0;
INITIALIZE_PASS_BEGIN(ClangDVMHWriter, "dvmh-writer", "DVMH Writer (Clang)", false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangExprMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DVMHParallelizationContext)
INITIALIZE_PASS_DEPENDENCY(ClangDVMHWriterProvider)
INITIALIZE_PASS_END(ClangDVMHWriter, "dvmh-writer", "DVMH Writer (Clang)", false, false)

ModulePass *llvm::createClangDVMHWriter() { return new ClangDVMHWriter; }

char DVMHParallelizationContext::ID = 0;
INITIALIZE_PASS(DVMHParallelizationContext, "dvmh-context",
  "DVMH Parallelization Context", false, false)

ImmutablePass *llvm::createDVMHParallelizationContext() {
  return new DVMHParallelizationContext;
}
