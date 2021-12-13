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
#define DEBUG_TYPE "clang-dvmh-writer"

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
  enum Kind {
    // Insert in any available position.
    Default,
    // Attach to the nearest statement.
    Bind,
    // Insert only if the same directive should be inserted and it has Default
    // or Bind kind.
    Merge
  };
  using PragmaString = SmallString<128>;
  using PragmaList =
      SmallVector<std::tuple<ParallelItem *, PragmaString, Kind>, 2>;
  PragmaList Before, After;
  TransformationContextBase *TfmCtx{nullptr};

  explicit Insertion(TransformationContextBase *Ctx = nullptr) : TfmCtx{Ctx} {}
};

using LocationToPragmas = DenseMap<const Stmt *, Insertion>;
using DeferredPragmaToLocations =
    DenseMap<ParallelItemRef,
             SmallVector<bcl::tagged_pair<bcl::tagged<const Stmt *, Stmt>,
                                          bcl::tagged<bool, Begin>>,
                         1>>;

struct InsertLocation {
  using InsertionList = bcl::tagged_pair<
      bcl::tagged<SmallVector<std::tuple<clang::Stmt *, Insertion::Kind>, 1>, Begin>,
      bcl::tagged<SmallVector<std::tuple<clang::Stmt *, Insertion::Kind>, 1>, End>>;
  using ScopeT = PointerUnion<llvm::Loop *, clang::Decl *>;

  InsertionList ToInsert;
  ScopeT Scope{nullptr};

  InsertLocation() = default;

  void invalidate() {
    ToInsert.get<Begin>().clear();
    ToInsert.get<End>().clear();
  }

  operator bool() const {
    return (!ToInsert.get<Begin>().empty() || !ToInsert.get<End>().empty());
  }
};
}

template<typename Tag>
static InsertLocation
findLocationToInsert(ParallelItemRef &PIRef, const Function &F, LoopInfo &LI,
                     ClangTransformationContext &TfmCtx,
                     const ClangExprMatcherPass::ExprMatcher &EM,
                     const LoopMatcherPass::LoopMatcher &LM) {
  assert(PIRef && "Invalid parallel item!");
  assert((std::is_same_v<Tag, Begin> && PIRef.isOnEntry() ||
         std::is_same_v<Tag, End> && !PIRef.isOnEntry()) &&
             "Inconsistent call!");
  InsertLocation Loc;
  if (PIRef.getPL()->Anchor.is<MDNode *>()) {
    auto *L{LI.getLoopFor(PIRef.getPE()->get<BasicBlock>())};
    auto ID{PIRef.getPL()->Anchor.get<MDNode *>()};
    while (L->getLoopID() && L->getLoopID() != ID)
      L = L->getParentLoop();
    assert(L && "A parallel directive has been attached to an unknown loop!");
    auto LMatchItr{LM.find<IR>(L)};
    assert(LMatchItr != LM.end() &&
           "Unable to find AST representation for a loop!");
    Loc.Scope = L;
    Loc.ToInsert.get<Tag>().emplace_back(LMatchItr->get<AST>(),
                                         Insertion::Bind);
    return Loc;
  }
  assert(PIRef.getPL()->Anchor.is<Value *>() &&
         "Directives must be attached to llvm::Value!");
  if (isa<Function>(PIRef.getPL()->Anchor.get<Value *>())) {
    auto *FD{TfmCtx.getDeclForMangledName(F.getName())};
    assert(FD && "AST representation of a function must be available!");
    Loc.Scope = FD;
    Loc.ToInsert.get<Tag>().emplace_back(*FD->getBody()->child_begin(),
                                         Insertion::Default);
    return Loc;
  }
  auto MatchItr{EM.find<IR>(PIRef.getPL()->Anchor.get<Value *>())};
  if (MatchItr == EM.end())
    return {};
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
  auto addBefore = [](clang::Stmt *S) {
    if (auto *CS{dyn_cast<CompoundStmt>(S)}; CS && !CS->body_empty())
      return *CS->body_begin();
    return S;
  };
  auto addAfter = [](clang::Stmt *S) {
    if (auto *CS{dyn_cast<CompoundStmt>(S)}; CS && !CS->body_empty())
      return *CS->body_rbegin();
    return S;
  };
  auto Current{MatchItr->get<AST>()};
  if (auto *D{Current.get<Decl>()};
      D && !D->getDeclContext()->isFunctionOrMethod()) {
    // TODO (kaniandr@gmail.com): attach pragmas to global declarations.
    Loc.invalidate();
    return Loc;
  }
  auto *ParentStmt{Current.get<Decl>() ? skipDecls(Current)
                                       : &Current.getUnchecked<Stmt>()};
  for (;;) {
    auto ToInsert{const_cast<Stmt *>(ParentStmt)};
    auto Parents{ParentCtx.getParents(*ParentStmt)};
    assert(!Parents.empty() &&
           (Parents.begin()->get<Stmt>() || Parents.begin()->get<Decl>()) &&
           "Executable statement must be in compound statement!");
    ParentStmt = Parents.begin()->template get<Decl>()
                     ? skipDecls(*Parents.begin())
                     : &Parents.begin()->template getUnchecked<Stmt>();
    if (isa<CompoundStmt>(ParentStmt)) {
      Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
      return Loc;
    }
    if (auto If{dyn_cast<IfStmt>(ParentStmt)}) {
      auto BindToStmt{Insertion::Default};
      if (If->getCond() == ToInsert ||
          If->getConditionVariableDeclStmt() == ToInsert) {
        if constexpr (std::is_same_v<Tag, End>) {
          if (!PIRef->isMarker()) {
            Loc.ToInsert.get<Begin>().emplace_back(
                addBefore(const_cast<Stmt *>(If->getThen())),
                Insertion::Default);
            if (auto *Else{If->getElse()})
              Loc.ToInsert.get<Begin>().emplace_back(
                  addBefore(const_cast<Stmt *>(Else)), Insertion::Default);
            else
              Loc.ToInsert.get<End>().emplace_back(
                  const_cast<Stmt *>(ParentStmt), Insertion::Default);
          } else {
            Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
          }
        } else if (!PIRef->isMarker()) {
          Loc.ToInsert.get<Tag>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                               Insertion::Default);
        } else {
          Loc.invalidate();
        }
      } else {
        Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
      }
      return Loc;
    }
    if (auto For{dyn_cast<ForStmt>(ParentStmt)}) {
      if (For->getBody() != ToInsert) {
        if constexpr (std::is_same_v<Tag, End>) {
          if (!PIRef->isMarker()) {
            Loc.ToInsert.get<Begin>().emplace_back(
                addBefore(const_cast<Stmt *>(For->getBody())),
                ToInsert != For->getInit() ? Insertion::Default
                                           : Insertion::Merge);
            Loc.ToInsert.get<End>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                                 ToInsert == For->getInc()
                                                     ? Insertion::Merge
                                                     : Insertion::Default);
          } else {
            Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
          }
        } else if (!PIRef->isMarker()) {
          if (ToInsert != For->getInc() || isa<PragmaRemoteAccess>(PIRef))
            Loc.ToInsert.get<Begin>().emplace_back(
                const_cast<Stmt *>(ParentStmt), Insertion::Default);
          if (ToInsert != For->getInit() && !isa<PragmaRemoteAccess>(PIRef))
            Loc.ToInsert.get<End>().emplace_back(
                addAfter(const_cast<Stmt *>(For->getBody())),
                Insertion::Default);
        } else {
          Loc.invalidate();
        }
      } else {
        Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
      }
      return Loc;
    }
    if (auto While{dyn_cast<WhileStmt>(ParentStmt)}) {
      if (While->getBody() != ToInsert) {
        if constexpr (std::is_same_v<Tag, End>) {
          if (!PIRef->isMarker()) {
            Loc.ToInsert.get<Begin>().emplace_back(
                addBefore(const_cast<Stmt *>(While->getBody())),
                Insertion::Default);
            Loc.ToInsert.get<End>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                                 Insertion::Default);
          } else {
            Loc.ToInsert.get<End>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                                 Insertion::Bind);
          }
        } else if (!PIRef->isMarker()) {
          if (!isa<PragmaRemoteAccess>(PIRef))
            Loc.ToInsert.get<End>().emplace_back(
                addAfter(const_cast<Stmt *>(While->getBody())),
                Insertion::Default);
          Loc.ToInsert.get<Tag>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                               Insertion::Default);
        } else {
          Loc.invalidate();
        }
      } else {
        Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
      }
      return Loc;
    }
    if (auto Do{dyn_cast<DoStmt>(ParentStmt)}) {
      ToInsert = const_cast<Stmt *>(ParentStmt);
      if (Do->getBody() != ToInsert) {
        if (!PIRef->isMarker()) {
          if constexpr (std::is_same_v<Tag, Begin>) {
            if (isa<PragmaRemoteAccess>(PIRef))
              Loc.ToInsert.get<Begin>().emplace_back(
                  const_cast<Stmt *>(ParentStmt), Insertion::Default);
            else
              Loc.ToInsert.get<End>().emplace_back(
                  addAfter(const_cast<Stmt *>(Do->getBody())),
                  Insertion::Default);
          } else {
            Loc.ToInsert.get<Begin>().emplace_back(
                addBefore(const_cast<Stmt *>(Do->getBody())),
                Insertion::Merge);
            Loc.ToInsert.get<End>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                                 Insertion::Default);
          }
        } else {
          if constexpr (std::is_same_v<Tag, End>)
            Loc.ToInsert.get<Tag>().emplace_back(const_cast<Stmt *>(ParentStmt),
                                                 Insertion::Bind);
          else
            Loc.invalidate();
        }
      } else {
        Loc.ToInsert.get<Tag>().emplace_back(ToInsert, Insertion::Bind);
      }
      return Loc;
    }
  }
  return Loc;
}

static bool tryToIgnoreDirective(ParallelItem &PI) {
  if (auto *PD{dyn_cast<PragmaData>(&PI)}) {
    // Some data directives could be redundant.
    // So, we will emit errors later when redundant directives are
    // already ignored.
    if (!PD->isRequired()) {
      PD->invalidate();
      return true;
    }
  } else if (auto *Marker{dyn_cast<ParallelMarker<PragmaData>>(&PI)}) {
    auto *PD{Marker->getBase()};
    if (!PD->isRequired()) {
      PD->invalidate();
      return true;
    }
  }
  // TODO: (kaniandr@gmail.com): emit error
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: error: unable to insert: "
                    << getName(static_cast<DirectiveId>(PI.getKind()))
                    << "\n");
  return false;
}

static inline unsigned addVarList(const std::set<std::string> &VarInfoList,
                              SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I{ VarInfoList.begin() }, EI{ VarInfoList.end() };
  Clause.append(I->begin(), I->end());
  unsigned Count{1};
  for (++I; I != EI; ++I, ++Count) {
    Clause.append({ ',', ' ' });
    Clause.append(I->begin(), I->end());
  }
  Clause.push_back(')');
  return Count;
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

static inline unsigned addVarList(const SortedVarListT &VarInfoList,
                                  SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
  Clause.push_back('(');
  auto I{VarInfoList.begin()}, EI{VarInfoList.end()};
  addVar(*I, Clause);
  for (++I, ++Count; I != EI; ++I, ++Count) {
    Clause.append({ ',', ' ' });
    addVar(*I, Clause);
  }
  Clause.push_back(')');
  return Count;
}

static inline unsigned addVarList(const AlignVarListT &VarInfoList,
                                  SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
  Clause.push_back('(');
  auto I{VarInfoList.begin()}, EI{VarInfoList.end()};
  addVar(
      *I,
      [](unsigned Dim, SmallVectorImpl<char> &Name) {
        ("I" + Twine(Dim)).toVector(Name);
      },
      Clause);
  for (++I, ++Count; I != EI; ++I, ++Count) {
    Clause.append({ ',', ' ' });
    addVar(
        *I,
        [](unsigned Dim, SmallVectorImpl<char> &Name) {
          ("I" + Twine(Dim)).toVector(Name);
        },
        Clause);
  }
  Clause.push_back(')');
  return Count;
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
addPragmaToStmt(const InsertLocation &ToInsert,
                ParallelItemRef &PIRef,
                TransformationContextBase *TfmCtx,
                const Twine &Prefix, const Twine &Suffix,
                DeferredPragmaToLocations &DeferredPragmas,
                std::vector<ParallelItem *> &NotOptimizedPragmas,
                LocationToPragmas &PragmasToInsert) {
  SmallString<128> PragmaStr;
    Prefix.toVector(PragmaStr);
  if (isa<PragmaParallel>(PIRef)) {
    assert(ToInsert.Scope && "A scope must be known!");
    pragmaParallelStr(PIRef, *ToInsert.Scope.get<Loop *>(), PragmaStr);
  } else if (isa<PragmaRegion>(PIRef)) {
    pragmaRegionStr(PIRef, PragmaStr);
  } else if (isa<PragmaRealign>(PIRef)) {
    pragmaRealignStr(PIRef, PragmaStr);
  } else if (auto *Marker{dyn_cast<ParallelMarker<PragmaRegion>>(PIRef)}) {
    PragmaStr = "}";
  } else if (isa<PragmaData>(PIRef) || isa<ParallelMarker<PragmaData>>(PIRef)) {
    // Even if this directive cannot be inserted (it is invalid) it should
    // be processed later. If it is replaced with some other directives,
    // this directive changes status to CK_Skip. The new status may allow us
    // to ignore some other directives later.
    auto &DeferredLocs{DeferredPragmas.try_emplace(PIRef).first->second};
    for (auto [S, K] : ToInsert.ToInsert.get<Begin>()) {
      auto &PragmaLoc{*PragmasToInsert.try_emplace(S, TfmCtx).first};
      DeferredLocs.emplace_back(PragmaLoc.first, true);
      // Mark the position of the directive in the source code. It will be later
      // created their only if necessary.
      PragmaLoc.second.Before.emplace_back(&*PIRef, "", K);
    }
    for (auto [S, K] : ToInsert.ToInsert.get<End>()) {
      auto &PragmaLoc{*PragmasToInsert.try_emplace(S, TfmCtx).first};
      DeferredLocs.emplace_back(PragmaLoc.first, false);
      // Mark the position of the directive in the source code. It will be later
      // created their only if necessary.
      PragmaLoc.second.After.emplace_back(&*PIRef, "", K);
    }
    if (auto *PD{dyn_cast<PragmaData>(PIRef)}; PD && PD->parent_empty())
      NotOptimizedPragmas.push_back(PD);
    return;
  } else {
    llvm_unreachable("An unknown pragma has been attached to a loop!");
  }
  Suffix.toVector(PragmaStr);
  assert(ToInsert && "A location for a pragma must be known!");
  for (auto [S, K] : ToInsert.ToInsert.get<Begin>()) {
    auto &PragmaLoc{*PragmasToInsert.try_emplace(S, TfmCtx).first};
    PragmaLoc.second.Before.emplace_back(&*PIRef, std::move(PragmaStr), K);
  }
  for (auto [S, K] : ToInsert.ToInsert.get<End>()) {
    auto &PragmaLoc{*PragmasToInsert.try_emplace(S, TfmCtx).first};
    PragmaLoc.second.After.emplace_back(&*PIRef, std::move(PragmaStr), K);
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
    return addVarList(PD->getMemory(), Str) != 0;
  else
    return addVarList(PD->getMemory(), std::move(Filter), Str) != 0;
}

static inline bool pragmaDataStr(ParallelItemRef &PDRef,
    SmallVectorImpl<char> &Str) {
  return pragmaDataStr(std::true_type{}, PDRef, Str);
}

/// Return true if a specified variable `A` is available in a scope which
/// contains a pragma at location `Loc`.
static bool isPragmaInDeclScope(
    ParentMapContext &ParentCtx,
    const DeferredPragmaToLocations::value_type::second_type::value_type &Loc,
    const dvmh::Align &A) {
  auto skipDecls = [&ParentCtx](DynTypedNode Current) -> const Stmt * {
    for (; !Current.template get<DeclStmt>();) {
      auto Parents{ParentCtx.getParents(Current)};
      assert(!Parents.empty() &&
             "Declaration must be in declaration statement!");
      Current = *Parents.begin();
    }
    return &Current.template getUnchecked<DeclStmt>();
  };
  auto skipUntil = [&ParentCtx](const Stmt &S, const Stmt &Until) {
    auto Current{DynTypedNode::create(S)};
    for (;;) {
      auto Parents{ParentCtx.getParents(Current)};
      if (Parents.empty())
        return false;
      Current = *Parents.begin();
      if (auto *PS{Current.get<Stmt>()}) {
        if (PS == &Until)
          return true;
      } else {
        return false;
      }
    }
    return false;
  };
  if (auto *V{std::get_if<VariableT>(&A.Target)};
      V && V->get<AST>()->getDeclContext()->isFunctionOrMethod()) {
    auto *DS{skipDecls(DynTypedNode::create(*V->get<AST>()))};
    return !skipUntil(*DS, *Loc.get<Stmt>());
  }
  return true;
}

static void
insertPragmaData(ArrayRef<PragmaData *> POTraverse,
                 DeferredPragmaToLocations &DeferredPragmas,
                 LocationToPragmas &PragmasToInsert) {
  for (auto *PD : POTraverse) {
    // Do not skip directive (even if it is marked as skipped) if its children
    // are invalid.
    if (PD->isInvalid() || PD->isSkipped())
      continue;
    auto &&[PIRef, ToInsert] = *DeferredPragmas.find_as(PD);
    for (auto &Loc : ToInsert) {
      assert(PragmasToInsert.count(Loc.get<Stmt>()) &&
             "Pragma position must be cached!");
      auto &Position{PragmasToInsert[Loc.get<Stmt>()]};
      auto &ASTCtx{cast<ClangTransformationContext>(Position.TfmCtx)
                              ->getContext()};
      auto &ParentCtx{ASTCtx.getParentMapContext()};
      if (Loc.get<Begin>()) {
        auto BeforeItr{
            find_if(Position.Before, [PI = PIRef.getPI()->get()](auto &Pragma) {
              return std::get<ParallelItem *>(Pragma) == PI;
            })};
        assert(BeforeItr != Position.Before.end() &&
               "Pragma position must be cached!");
        auto &PragmaStr{std::get<Insertion::PragmaString>(*BeforeItr)};
        auto StashPragmaSize{PragmaStr.size()};
        bool NotEmptyPragma{true};
        if (auto *DS{dyn_cast<DeclStmt>(Loc.get<Stmt>())})
          NotEmptyPragma = pragmaDataStr(
              [DS, &ParentCtx, &Loc](const dvmh::Align &A) {
                if (!isPragmaInDeclScope(ParentCtx, Loc, A))
                  return false;
                if (auto *V{std::get_if<VariableT>(&A.Target)})
                  return !is_contained(DS->getDeclGroup(), V->get<AST>());
                return true;
              },
              PIRef, PragmaStr);
        else
          NotEmptyPragma = pragmaDataStr(
              [&ParentCtx, &Loc](const dvmh::Align &A) {
                return isPragmaInDeclScope(ParentCtx, Loc, A);
              },
              PIRef, PragmaStr);
        PragmaStr += "\n";
        if (!NotEmptyPragma)
          PragmaStr.resize(StashPragmaSize);
      } else {
        auto AfterItr{
            find_if(Position.After, [PI = PIRef.getPI()->get()](auto &Pragma) {
              return std::get<ParallelItem *>(Pragma) == PI;
            })};
        assert(AfterItr != Position.After.end() &&
               "Pragma position must be cached!");
        auto &PragmaStr{std::get<Insertion::PragmaString>(*AfterItr)};
        auto StashPragmaSize{PragmaStr.size()};
        PragmaStr += "\n";
        if (!pragmaDataStr(
                [&ParentCtx, &Loc](const dvmh::Align &A) {
                  return isPragmaInDeclScope(ParentCtx, Loc, A);
                },
                PIRef, PragmaStr))
          PragmaStr.resize(StashPragmaSize);
      }
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
    const DeferredPragmaToLocations &DeferredPragmas,
    LocationToPragmas &PragmasToInsert) {
  traversePragmaDataPO(
      NotOptimizedPragmas,
      [&DeferredPragmas, &PragmasToInsert](ParallelItem *PI) {
        auto PD{cast<PragmaData>(PI)};
        dbgs() << "id: " << PD << " ";
        dbgs() << "skipped: " << PD->isSkipped() << " ";
        dbgs() << "invalid: " << PD->isInvalid() << " ";
        dbgs() << "final: " << PD->isFinal() << " ";
        dbgs() << "required: " << PD->isRequired() << " ";
        if (isa<PragmaActual>(PD))
          dbgs() << "actual: ";
        else if (isa<PragmaGetActual>(PD))
          dbgs() << "get_actual: ";
        else if (isa<PragmaRemoteAccess>(PD))
          dbgs() << "remote_access: ";
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
          dbgs() << "location: ";
          for (auto &Loc: ToInsert) {
            if (auto TfmCtx{dyn_cast<ClangTransformationContext>(
                    PragmasToInsert[Loc.get<Stmt>()].TfmCtx)})
              Loc.get<Stmt>()->getBeginLoc().print(
                  dbgs(), TfmCtx->getContext().getSourceManager());
            else
              llvm_unreachable("Unsupported type of a "
                               "transformation context!");
            dbgs() << (Loc.get<Begin>() ? " (before)" : " (after)") << " ";
          }
        } else {
          dbgs() << "stub ";
        }
        dbgs() << "\n";
      });
}

namespace {
struct DeclStmtSearch : public RecursiveASTVisitor<DeclStmtSearch> {
  bool VisitDeclStmt(DeclStmt *) {
    Found = true;
    return true;
  }
  bool Found{false};
};
}

#ifdef LLVM_DEBUG
static void insertLocationLog(const ParallelItemRef &PIRef,
                              const InsertLocation &ToInsert,
                              const ClangTransformationContext &TfmCtx) {
  if (!ToInsert)
    dbgs() << "[DVMH WRITER]: unable to insert ";
  else
    dbgs() << "[DVMH WRITER]: insertion points for ";
  if ((**PIRef.getPI()).isMarker())
    dbgs() << "marker";
  else
    dbgs() << "directive "
           << getName(
                  static_cast<tsar::DirectiveId>((**PIRef.getPI()).getKind()));
  dbgs() << " " << PIRef.getPI()->get();
  dbgs() << " to ";
  if (auto *MD{PIRef.getPL()->Anchor.dyn_cast<MDNode *>()}) {
    MD->print(dbgs());
  } else if (auto *V{PIRef.getPL()->Anchor.dyn_cast<Value *>()}) {
    if (auto *F{dyn_cast<Function>(V)}) {
      dbgs() << "function " << F->getName();
    } else {
      V->print(dbgs());
      if (auto *I{dyn_cast<Instruction>(V)})
        dbgs() << " (function " << I->getFunction()->getName() << ")";
    }
  }
  auto insertLocationLog = [&TfmCtx](auto &List) {
    for (auto [S, K] : List) {
      auto &SrcMgr{TfmCtx.getContext().getSourceManager()};
      dbgs() << " ";
      S->getBeginLoc().print(dbgs(), SrcMgr);
      switch (K) {
      case Insertion::Bind:
        dbgs() << "(bind)";
        break;
      case Insertion::Merge:
        dbgs() << "(merge)";
        break;
      }
    }
  };
  dbgs() << " before:";
  insertLocationLog(ToInsert.ToInsert.get<Begin>());
  dbgs() << " after:";
  insertLocationLog(ToInsert.ToInsert.get<End>());
  dbgs() << "\n";
}
#endif

static bool canMergeDirectives(const Insertion::PragmaList &Pragmas) {
  for (auto &P : Pragmas)
    if (std::get<Insertion::Kind>(P) == Insertion::Merge)
      if (auto *PD{dyn_cast<PragmaData>(std::get<ParallelItem *>(P))})
        if (!all_of(PD->getMemory(), [PD, &Pragmas](auto &M) {
              auto Itr{find_if(Pragmas, [PD, &M](auto &ToMerge) {
                auto *ToMergePD{
                    dyn_cast<PragmaData>(std::get<ParallelItem *>(ToMerge))};
                if (!ToMergePD || ToMergePD == PD ||
                    ToMergePD->getKind() != PD->getKind())
                  return false;
                return ToMergePD->getMemory().count(M) != 0;
              })};
              return Itr != Pragmas.end();
            })) {
          if (!tryToIgnoreDirective(*PD)) {
            LLVM_DEBUG(dbgs() << "[DVMH WRITER]: uanble to merge directive "
                              << PD << "\n");
            return false;
          }
        }
  return true;
}

static bool closeRemoteAccesses(DeferredPragmaToLocations &DeferredPragmas,
                                LocationToPragmas &PragmasToInsert) {
  for (auto &&[PIRef, ToInsert] : DeferredPragmas)
    if (!ToInsert.empty())
      if (auto *Marker{dyn_cast<ParallelMarker<PragmaRemoteAccess>>(PIRef)})
        if (auto *Remote{Marker->getBase()};
            !Remote->isInvalid() && !Remote->isSkipped()) {
          auto &&[RemoteRef, RemoteToInsert] = *DeferredPragmas.find_as(Remote);
          if (RemoteToInsert.size() != 1 || ToInsert.size() != 1) {
            LLVM_DEBUG(dbgs()
                       << "[DVMH WRITER]: unable to insert remote access "
                          "directive: multiple begin/end positions");
            return false;
          }
          auto *TfmCtx{cast<ClangTransformationContext>(
              PragmasToInsert[RemoteToInsert.front().get<Stmt>()].TfmCtx)};
          auto &ParentCtx{TfmCtx->getContext().getParentMapContext()};
          auto RemoteParent{
              ParentCtx.getParents(*RemoteToInsert.front().get<Stmt>())};
          assert(!RemoteParent.empty() &&
                 "Remote access must be inserted in a scope");
          auto &RPS{RemoteParent.begin()->getUnchecked<Stmt>()};
          auto RemoteMarkerParent{
              ParentCtx.getParents(*ToInsert.front().get<Stmt>())};
          assert(!RemoteMarkerParent.empty() &&
                 "Remote access must be inserted in a scope");
          auto &RMPS{RemoteMarkerParent.begin()->getUnchecked<Stmt>()};
          if (&RPS != &RMPS) {
            LLVM_DEBUG(
                dbgs()
                << "[DVMH WRITER]: unable to insert remote access directive: "
                   "inconsistent begin and end insertion points");
            return false;
          }
          if (auto *CS{dyn_cast<CompoundStmt>(&RPS)}) {
            auto I{find(CS->children(), RemoteToInsert.front().get<Stmt>())};
            auto EI{find(CS->children(), ToInsert.front().get<Stmt>())};
            if (!RemoteToInsert.front().get<Begin>())
              ++I;
            if (!ToInsert.front().get<Begin>())
              ++EI;
            DeclStmtSearch DSS;
            for (I; I != EI; ++I) {
              DSS.TraverseStmt(const_cast<Stmt *>(*I));
              if (DSS.Found) {
                LLVM_DEBUG(dbgs() << "[DVMH WRITER]: unable to insert remote "
                                     "access directive: initialization inside "
                                     "the remote access scope");
                return false;
              }
            }
          } else {
            DeclStmtSearch DSS;
            DSS.TraverseStmt(
                const_cast<Stmt *>(RemoteToInsert.front().get<Stmt>()));
            if (DSS.Found) {
              LLVM_DEBUG(dbgs() << "[DVMH WRITER]: unable to insert remote "
                                   "access directive: initialization inside "
                                   "the remote access scope");
              return false;
            }
          }
          for (auto &Loc : ToInsert)
            if (Loc.get<Begin>())
              PragmasToInsert[Loc.get<Stmt>()].Before.emplace_back(
                  Marker, "\n}", Insertion::Bind);
            else
              PragmasToInsert[Loc.get<Stmt>()].After.emplace_back(
                  Marker, "\n}", Insertion::Bind);
        }
  return true;
}

bool ClangDVMHWriter::runOnModule(llvm::Module &M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  ClangDVMHWriterProvider::initialize<TransformationEnginePass>(
      [&TfmInfo](TransformationEnginePass &Wrapper) {
        Wrapper.set(TfmInfo.get());
      });
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: insert data transfer directives\n");
  std::vector<ParallelItem *> NotOptimizedPragmas;
  DeferredPragmaToLocations DeferredPragmas;
  LocationToPragmas PragmasToInsert;
  auto &PIP{getAnalysis<DVMHParallelizationContext>()};
  for (auto F : make_range(PIP.getParallelization().func_begin(),
                           PIP.getParallelization().func_end())) {
    LLVM_DEBUG(dbgs() << "[DVMH WRITER]: process function " << F->getName()
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
        auto processDirective =
            [&PLocListItr, &PLocItr, F, &LI, TfmCtx, EM, &LM, &DeferredPragmas,
             &NotOptimizedPragmas,
             &PragmasToInsert](ParallelBlock::iterator PIItr, auto Tag) {
              ParallelItemRef PIRef{PLocListItr, PLocItr, PIItr,
                                    std::is_same_v<decltype(Tag), Begin>};
              auto ToInsert{findLocationToInsert<decltype(Tag)>(
                  PIRef, *F, LI, *TfmCtx, EM, LM)};
              LLVM_DEBUG(insertLocationLog(PIRef, ToInsert, *TfmCtx));
              if (!ToInsert && !tryToIgnoreDirective(**PIItr))
                return false;
              addPragmaToStmt(ToInsert, PIRef, TfmCtx,
                              std::is_same_v<decltype(Tag), Begin> ? "" : "\n",
                              std::is_same_v<decltype(Tag), Begin> ? "\n" : "",
                              DeferredPragmas, NotOptimizedPragmas,
                              PragmasToInsert);
              return true;
            };
        for (auto PIItr{PLocItr->Entry.begin()}, PIItrE{PLocItr->Entry.end()};
             PIItr != PIItrE; ++PIItr)
          if (!processDirective(PIItr, Begin{}))
            return false;
        for (auto PIItr{PLocItr->Exit.begin()}, PIItrE{PLocItr->Exit.end()};
             PIItr != PIItrE; ++PIItr)
          if (!processDirective(PIItr, End{}))
            return false;
      }
    }
  }
  for (auto &&[S, I] : PragmasToInsert)
    if (!canMergeDirectives(I.Before) || !canMergeDirectives(I.After))
      return false;
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
                                             &PragmasToInsert,
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
    auto &&[PIRef, ToInsert] = *PIItr;
    if (PD->getMemory().empty()) {
      PD->skip();
    } else if (!ToInsert.empty() &&
               all_of(ToInsert, [PD, &PragmasToInsert](auto &Loc) {
                 if (Loc.template get<Begin>()) {
                   auto *S{Loc.template get<Stmt>()};
                   auto *DS{dyn_cast_or_null<DeclStmt>(S)};
                   if (DS &&
                       all_of(PD->getMemory(), [DS](const dvmh::Align &Align) {
                         if (auto *V{std::get_if<VariableT>(&Align.Target)})
                           return is_contained(DS->getDeclGroup(),
                                               V->get<AST>());
                         return false;
                       }))
                     return true;
                 }
                 auto &Position{PragmasToInsert[Loc.template get<Stmt>()]};
                 auto &ASTCtx{cast<ClangTransformationContext>(Position.TfmCtx)
                                  ->getContext()};
                 auto &ParentCtx{ASTCtx.getParentMapContext()};
                 return all_of(PD->getMemory(),
                               [&ParentCtx, &Loc](const dvmh::Align &A) {
                                 return !isPragmaInDeclScope(ParentCtx, Loc, A);
                               });
               })) {
      // Do not mention variables in a directive if it has not been
      // declared yet.
      PD->skip();
    }
  });
  bool IsOk{true}, IsAllOptimized{true};
  // Now we check that there is any way to actualize data for each parallel
  // region.
  for (auto *PI : NotOptimizedPragmas)
    if (!cast<PragmaData>(PI)->isSkipped() &&
        !cast<PragmaData>(PI)->child_empty()) {
      LLVM_DEBUG(
          dbgs() << "[DVMH WRITER]: warning: unable to optimize data directive "
                 << PI << "\n");
      IsAllOptimized = false;
      if (cast<PragmaData>(PI)->isInvalid()) {
        IsOk = false;
        LLVM_DEBUG(
            dbgs() << "[DVMH WRITER]: error: unable to insert data directive "
                   << PI << "\n");
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
  auto merge = [](auto &List) {
    auto CurrItr{List.end()};
    for (auto I{List.begin()}, EI{List.end()}; I != EI; ++I) {
      if (CurrItr != List.end() &&
          std::get<ParallelItem *>(*CurrItr)->getKind() ==
              std::get<ParallelItem *>(*I)->getKind()) {
        // TODO (kaniandr@gmail.com): Do not remove this data
        // directive. It cannot be removed from ParalleizationInfo, otherwise
        // the DefferedPragmas map will be invalidated because it uses
        // iterator as a key. May be we should not store iterators in
        // DefferedPragmas map. Note, that each value from the DefferedPragmas
        // map should be stored in the PragmasToInsert collection, so do not
        // remove this data directive from the List.
        auto *PD{cast<PragmaData>(std::get<ParallelItem *>(*I))};
        if (PD->isSkipped())
          continue;
        auto *Merge{cast<PragmaData>(std::get<ParallelItem *>(*CurrItr))};
        Merge->getMemory().insert(PD->getMemory().begin(),
                                  PD->getMemory().end());
        Merge->child_insert(PD);
        PD->parent_insert(Merge);
        PD->skip();
        continue;
      }
      CurrItr = (isa<PragmaData>(std::get<ParallelItem *>(*I)) &&
                 !cast<PragmaData>(std::get<ParallelItem *>(*I))->isSkipped())
                    ? I
                    : List.end();
    }
  };
  for (auto &&[Position, Insertion] : PragmasToInsert) {
    if (!Insertion.Before.empty()) {
      stable_sort(Insertion.Before, [](auto &LHS, auto &RHS) {
        return (std::get<Insertion::Kind>(LHS) !=
                    std::get<Insertion::Kind>(RHS) &&
                (std::get<Insertion::Kind>(LHS) == Insertion::Bind ||
                 std::get<Insertion::Kind>(RHS) == Insertion::Bind))
                   ? std::get<Insertion::Kind>(LHS) != Insertion::Bind &&
                         std::get<Insertion::Kind>(RHS) == Insertion::Bind
                   : !isa<PragmaRemoteAccess>(std::get<ParallelItem *>(LHS)) &&
                         isa<PragmaRemoteAccess>(std::get<ParallelItem *>(RHS));
      });
      merge(Insertion.Before);
    }
    if (!Insertion.After.empty()) {
      stable_sort(Insertion.After, [](auto &LHS, auto &RHS) {
        return (std::get<Insertion::Kind>(LHS) !=
                    std::get<Insertion::Kind>(RHS) &&
                (std::get<Insertion::Kind>(LHS) == Insertion::Bind ||
                 std::get<Insertion::Kind>(RHS) == Insertion::Bind))
                   ? std::get<Insertion::Kind>(LHS) == Insertion::Bind &&
                         std::get<Insertion::Kind>(RHS) != Insertion::Bind
                   : !isa<PragmaRemoteAccess>(std::get<ParallelItem *>(LHS)) &&
                         isa<PragmaRemoteAccess>(std::get<ParallelItem *>(RHS));
      });
      merge(Insertion.After);
    }
  }
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: optimized replacement tree:\n";
             printReplacementTree(NotOptimizedPragmas, DeferredPragmas,
                                  PragmasToInsert));
  // Build pragmas for necessary data transfer directives.
  insertPragmaData(POTraverse, DeferredPragmas, PragmasToInsert);
  if (!closeRemoteAccesses(DeferredPragmas, PragmasToInsert))
    return false;
  // Update sources.
  for (auto &&[ToInsert, Pragmas] : PragmasToInsert) {
    auto BeginLoc{ToInsert->getBeginLoc()};
    auto TfmCtx{cast<ClangTransformationContext>(Pragmas.TfmCtx)};
    bool IsBeginChanged{false};
    for (auto &&[PI, Str, BindToStmt] : Pragmas.Before)
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
    for (auto &&[PI, Str, BindToStmt] : Pragmas.After)
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
INITIALIZE_PASS_BEGIN(ClangDVMHWriter, "clang-dvmh-writer",
                      "DVMH Writer (Clang)", false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangExprMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DVMHParallelizationContext)
INITIALIZE_PASS_DEPENDENCY(ClangDVMHWriterProvider)
INITIALIZE_PASS_END(ClangDVMHWriter, "clang-dvmh-writer", "DVMH Writer (Clang)",
                    false, false)

ModulePass *llvm::createClangDVMHWriter() { return new ClangDVMHWriter; }

char DVMHParallelizationContext::ID = 0;
INITIALIZE_PASS(DVMHParallelizationContext, "dvmh-context",
  "DVMH Parallelization Context", false, false)

ImmutablePass *llvm::createDVMHParallelizationContext() {
  return new DVMHParallelizationContext;
}
