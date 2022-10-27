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
#include "tsar/Support/Clang/Diagnostic.h"
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

struct InsertLocation {
  enum Kind : uint8_t {
    // Insert in any available position.
    Default,
    // Attach to the nearest statement.
    Bind,
    // Insert only if the same directive should be inserted and it has Default
    // or Bind kind.
    Merge
  };
  class InsertionInfo {
  public:
    InsertionInfo(clang::Stmt *S, bool IsBefore = true, Kind K = Default,
                  bool IsValid = true)
        : mInfo{{S, IsValid}, IsBefore}, mKind(K) {}

    operator clang::Stmt *() { return mInfo.getPointer().getPointer(); }
    operator const clang::Stmt *() const {
      return mInfo.getPointer().getPointer();
    }

    bool operator==(const InsertionInfo &RHS) {
      return mInfo == RHS.mInfo && mKind == RHS.mKind;
    }

    bool operator!=(const InsertionInfo &RHS) { return !operator==(RHS); }

    clang::Stmt *getStmt() { return *this; }
    const clang::Stmt *getStmt() const { return *this; }

    clang::Stmt &operator*() { return *getStmt(); }
    const clang::Stmt &operator*() const { return *getStmt(); }

    clang::Stmt *operator->() { return getStmt(); }
    const clang::Stmt *operator->() const { return getStmt(); }

    bool isValid() const { return mInfo.getPointer().getInt(); }
    void setIsValid(bool IsValid = true) {
      auto Val = mInfo.getPointer();
      Val.setInt(IsValid);
      mInfo.setPointer(Val);
    }

    bool isBefore() const { return mInfo.getInt(); }
    void setIsBefore(bool IsBefore = true) { mInfo.setInt(IsBefore); }

    Kind getKind() const noexcept { return mKind; }

  private:
    PointerIntPair<PointerIntPair<clang::Stmt *, 1, bool>, 1, bool> mInfo;
    Kind mKind;
  };

  explicit InsertLocation(TransformationContextBase *TC) : TfmCtx(TC) {}

  bool isAlwaysInvalid() const noexcept {
    return mIsInvalid || ToInsert.empty();
  }
  bool isInvalid() const {
    return isAlwaysInvalid() ||
           any_of(ToInsert, [](const auto &Info) { return !Info.isValid(); });
  }
  bool isValid() const { return !isInvalid(); }
  operator bool() const { return isValid(); }

  void invalidate() noexcept { mIsInvalid = true; }

  template <typename... T> void emplace_back(T &&...V) {
    ToInsert.emplace_back(std::forward<T>(V)...);
  }

  auto begin() { return ToInsert.begin(); }
  auto end() { return ToInsert.end(); }

  auto begin() const { return ToInsert.begin(); }
  auto end() const { return ToInsert.end(); }

  auto size() const { return ToInsert.size(); }
  bool empty() const { return ToInsert.empty(); }

  void print(raw_ostream &OS) const {
    if (!isa<ClangTransformationContext>(TfmCtx)) {
      OS << "<print is not supported>";
      return;
    }
    auto &SrcMgr{cast<ClangTransformationContext>(TfmCtx)
                     ->getContext()
                     .getSourceManager()};
    if (!isValid())
      OS << "<invalid location>: ";
    OS << "anchor ";
    if (Anchor.isValid())
      Anchor.print(OS, SrcMgr);
    else
      OS << "invalid";
    OS << ", locations";
    for (auto &Info : ToInsert) {
      OS << " ";
      if (Info.isBefore()) {
        OS << "before ";
        Info->getBeginLoc().print(OS, SrcMgr);
      } else {
        OS << "after ";
        Info->getEndLoc().print(OS, SrcMgr);
      }
      OS<< "(";
      switch (Info.getKind()) {
      case InsertLocation::Bind:
        OS << "bind";
        break;
      case InsertLocation::Merge:
        OS << "merge";
        break;
      }
      if (!Info.isValid())
       OS << ", invalid";
      OS << ")";
    }
  }

  SmallVector<InsertionInfo, 1> ToInsert;
  SourceLocation Anchor;
  TransformationContextBase *TfmCtx{nullptr};

private:
  mutable bool mIsInvalid{false};
};

struct Insertion {
  using PragmaString = SmallString<128>;
  using PragmaList =
      SmallVector<std::tuple<ParallelItem *, PragmaString, InsertLocation::Kind,
                             ParallelMarker<PragmaData> *>,
                  2>;
  PragmaList Before, After;
  TransformationContextBase *TfmCtx{nullptr};

  explicit Insertion(TransformationContextBase *Ctx = nullptr) : TfmCtx{Ctx} {}
};

using LocationToPragmas =
    DenseMap<const Stmt *, Insertion, DenseMapInfo<const Stmt *>,
             TaggedDenseMapPair<bcl::tagged<const Stmt *, Stmt>,
                                bcl::tagged<Insertion, Insertion>>>;
using PragmaToLocations =
    DenseMap<ParallelItemRef, InsertLocation, DenseMapInfo<ParallelItemRef>,
             TaggedDenseMapPair<bcl::tagged<ParallelItemRef, ParallelItemRef>,
                                bcl::tagged<InsertLocation, InsertLocation>>>;
}

static InsertLocation
findLocationToInsert(ParallelItemRef &PIRef, const Function &F, LoopInfo &LI,
                     ClangTransformationContext &TfmCtx,
                     const ClangExprMatcherPass::ExprMatcher &EM,
                     const LoopMatcherPass::LoopMatcher &LM) {
  assert(PIRef && "Invalid parallel item!");
  InsertLocation Loc{&TfmCtx};
  if (PIRef.getPL()->Anchor.is<MDNode *>()) {
    auto *L{LI.getLoopFor(PIRef.getPE()->get<BasicBlock>())};
    auto ID{PIRef.getPL()->Anchor.get<MDNode *>()};
    while (L->getLoopID() && L->getLoopID() != ID)
      L = L->getParentLoop();
    assert(L && "A parallel directive has been attached to an unknown loop!");
    auto LMatchItr{LM.find<IR>(L)};
    assert(LMatchItr != LM.end() &&
           "Unable to find AST representation for a loop!");
    Loc.Anchor = LMatchItr->get<AST>()->getBeginLoc();
    Loc.emplace_back(LMatchItr->get<AST>(), PIRef.isOnEntry(),
                     InsertLocation::Bind);
    return Loc;
  }
  assert(PIRef.getPL()->Anchor.is<Value *>() &&
         "Directives must be attached to llvm::Value!");
  if (isa<Function>(PIRef.getPL()->Anchor.get<Value *>())) {
    auto *FD{TfmCtx.getDeclForMangledName(F.getName())};
    assert(FD && "AST representation of a function must be available!");
    Loc.Anchor = FD->getBody()->child_begin()->getBeginLoc();
    Loc.emplace_back(*FD->getBody()->child_begin(), PIRef.isOnEntry(),
                     InsertLocation::Default);
    return Loc;
  }
  auto MatchItr{EM.find<IR>(PIRef.getPL()->Anchor.get<Value *>())};
  if (MatchItr == EM.end()) {
    auto *FD{TfmCtx.getDeclForMangledName(F.getName())};
    assert(FD && "AST representation of a function must be available!");
    Loc.Anchor = FD->getLocation();
    Loc.invalidate();
    return Loc;
  }
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
  auto &Current{MatchItr->get<AST>()};
  Loc.Anchor = Current.getSourceRange().getBegin();
  if (auto *D{Current.get<Decl>()};
      D &&
      (isa<ParmVarDecl>(D) || !D->getDeclContext()->isFunctionOrMethod())) {
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
      Loc.emplace_back(ToInsert, PIRef.isOnEntry(), InsertLocation::Bind);
      return Loc;
    }
    if (auto If{dyn_cast<IfStmt>(ParentStmt)}) {
      if (If->getCond() == ToInsert ||
          If->getConditionVariableDeclStmt() == ToInsert) {
        if (PIRef.isOnExit()) {
          if (!PIRef->isMarker()) {
            Loc.emplace_back(addBefore(const_cast<Stmt *>(If->getThen())), true,
                             InsertLocation::Default);
            if (auto *Else{If->getElse()})
              Loc.emplace_back(addBefore(const_cast<Stmt *>(Else)), true,
                               InsertLocation::Default);
            else
              Loc.emplace_back(const_cast<Stmt *>(ParentStmt), false,
                               InsertLocation::Default);
          } else {
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), PIRef.isOnEntry(),
                             InsertLocation::Bind);
          }
        } else if (!PIRef->isMarker()) {
          Loc.emplace_back(const_cast<Stmt *>(ParentStmt), PIRef.isOnEntry(),
                           InsertLocation::Default);
        } else {
          Loc.invalidate();
        }
      } else {
        Loc.emplace_back(ToInsert, PIRef.isOnEntry(), InsertLocation::Bind);
      }
      return Loc;
    }
    if (auto For{dyn_cast<ForStmt>(ParentStmt)}) {
      if (For->getBody() != ToInsert) {
        if (PIRef.isOnExit()) {
          if (!PIRef->isMarker()) {
            Loc.emplace_back(
                addBefore(const_cast<Stmt *>(For->getBody())), true,
                ToInsert != For->getInit() ? InsertLocation::Default
                                           : InsertLocation::Merge);
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), false,
                             ToInsert == For->getInc()
                                 ? InsertLocation::Merge
                                 : InsertLocation::Default);
          } else {
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), PIRef.isOnEntry(),
                             InsertLocation::Bind);
          }
        } else if (!PIRef->isMarker()) {
          if (ToInsert != For->getInc() || isa<PragmaRemoteAccess>(PIRef))
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), true,
                             InsertLocation::Default);
          if (ToInsert != For->getInit() && !isa<PragmaRemoteAccess>(PIRef))
            Loc.emplace_back(addAfter(const_cast<Stmt *>(For->getBody())),
                             false, InsertLocation::Default);
        } else {
          Loc.invalidate();
        }
      } else {
        Loc.emplace_back(ToInsert, PIRef.isOnEntry(), InsertLocation::Bind);
      }
      return Loc;
    }
    if (auto While{dyn_cast<WhileStmt>(ParentStmt)}) {
      if (While->getBody() != ToInsert) {
        if (PIRef.isOnExit()) {
          if (!PIRef->isMarker()) {
            Loc.emplace_back(addBefore(const_cast<Stmt *>(While->getBody())),
                             true, InsertLocation::Default);
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), false,
                             InsertLocation::Default);
          } else {
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), false,
                             InsertLocation::Bind);
          }
        } else if (!PIRef->isMarker()) {
          if (!isa<PragmaRemoteAccess>(PIRef))
            Loc.emplace_back(addAfter(const_cast<Stmt *>(While->getBody())),
                             false, InsertLocation::Default);
          Loc.emplace_back(const_cast<Stmt *>(ParentStmt), PIRef.isOnEntry(),
                           InsertLocation::Default);
        } else {
          Loc.invalidate();
        }
      } else {
        Loc.emplace_back(ToInsert, PIRef.isOnEntry(), InsertLocation::Bind);
      }
      return Loc;
    }
    if (auto Do{dyn_cast<DoStmt>(ParentStmt)}) {
      ToInsert = const_cast<Stmt *>(ParentStmt);
      if (Do->getBody() != ToInsert) {
        if (!PIRef->isMarker()) {
          if (PIRef.isOnEntry()) {
            if (isa<PragmaRemoteAccess>(PIRef))
              Loc.emplace_back(const_cast<Stmt *>(ParentStmt), true,
                               InsertLocation::Default);
            else
              Loc.emplace_back(addAfter(const_cast<Stmt *>(Do->getBody())),
                               false, InsertLocation::Default);
          } else {
            Loc.emplace_back(addBefore(const_cast<Stmt *>(Do->getBody())), true,
                             InsertLocation::Merge);
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), false,
                             InsertLocation::Default);
          }
        } else {
          if (PIRef.isOnExit())
            Loc.emplace_back(const_cast<Stmt *>(ParentStmt), PIRef.isOnEntry(),
                             InsertLocation::Bind);
          else
            Loc.invalidate();
        }
      } else {
        Loc.emplace_back(ToInsert, PIRef.isOnEntry(), InsertLocation::Bind);
      }
      return Loc;
    }
  }
  return Loc;
}

static inline unsigned addVarListEntry(const std::set<std::string> &VarInfoList,
                                       SmallVectorImpl<char> &Clause) {
  auto I{ VarInfoList.begin() }, EI{ VarInfoList.end() };
  Clause.append(I->begin(), I->end());
  unsigned Count{1};
  for (++I; I != EI; ++I, ++Count) {
    Clause.append({ ',', ' ' });
    Clause.append(I->begin(), I->end());
  }
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
                if (V.Offset.isNegative())
                  Str.push_back('(');
                V.Offset.toString(Str);
                if (V.Offset.isNegative())
                  Str.push_back(')');
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

static inline unsigned addVarListEntry(const SortedVarListT &VarInfoList,
                                       SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
  auto I{VarInfoList.begin()}, EI{VarInfoList.end()};
  addVar(*I, Clause);
  for (++I, ++Count; I != EI; ++I, ++Count) {
    Clause.append({',', ' '});
    addVar(*I, Clause);
  }
  return Count;
}

static inline unsigned addVarListEntry(const AlignVarListT &VarInfoList,
                                       SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
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
  return Count;
}

template <typename FilterT>
static inline unsigned addVarListEntry(const AlignVarListT &VarInfoList,
                                       FilterT &&F,
                                       SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
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
  return Count;
}

template <typename ListT>
static inline unsigned addVarList(const ListT &VarInfoList,
                                  SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto Count{addVarListEntry(VarInfoList, Clause)};
  Clause.push_back(')');
  return Count;
}

template <typename FilterT>
static inline unsigned addVarList(const AlignVarListT &VarInfoList, FilterT &&F,
                                  SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto Count{addVarListEntry(VarInfoList, std::forward<FilterT>(F), Clause)};
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

static void addParallelMapping(const PragmaParallel &Parallel,
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
  for (auto &[Var, Mapping] :
       Parallel.getClauses().get<trait::DirectAccess>()) {
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

static void pragmaRealignStr(const PragmaRealign &Realign,
                             SmallVectorImpl<char> &Str) {
  getPragmaText(DirectiveId::DvmRealign, Str);
  Str.resize(Str.size() - 1);
  Str.push_back('(');
  auto WhatName{Realign.what().get<AST>()->getName()};
  Str.append(WhatName.begin(), WhatName.end());
  StringRef IdxPrefix{"iEX"};
  for (unsigned I = 0, EI = Realign.getWhatDimSize(); I < EI; ++I) {
    Str.push_back('[');
    if (auto Itr{find_if(Realign.with().Relation,
                         [I](const auto &A) {
                           if (!A)
                             return false;
                           if (auto *V{std::get_if<dvmh::Align::Axis>(&*A)})
                             return V->Dimension == I;
                           return false;
                         })};
        Itr != Realign.with().Relation.end()) {
      Str.append(IdxPrefix.begin(), IdxPrefix.end());
      SmallString<2> SuffixData;
      auto Suffix{Twine(I).toStringRef(SuffixData)};
      Str.append(Suffix.begin(), Suffix.end());
    }
    Str.push_back(']');
  }
  Str.append({' ', 'w', 'i', 't', 'h', ' '});
  addVar(
      Realign.with(),
      [IdxPrefix](unsigned Dim, SmallVectorImpl<char> &Str) {
        (IdxPrefix + Twine(Dim)).toVector(Str);
      },
      Str);
  Str.push_back(')');
}

static void pragmaParallelStr(const PragmaParallel &Parallel,
                              SmallVectorImpl<char> &Str) {
  getPragmaText(DirectiveId::DvmParallel, Str);
  Str.resize(Str.size() - 1);
  if (Parallel.getClauses().get<dvmh::Align>()) {
    Str.push_back('(');
    for (auto &LToI : Parallel.getClauses().get<trait::Induction>()) {
      Str.push_back('[');
      auto Name{LToI.get<VariableT>().get<AST>()->getName()};
      Str.append(Name.begin(), Name.end());
      Str.push_back(']');
    }
    Str.append({' ', 'o', 'n', ' '});
    addVar(
        *Parallel.getClauses().get<dvmh::Align>(),
        [&Parallel](unsigned Dim, SmallVectorImpl<char> &Str) {
          auto &Induct{Parallel.getClauses()
                           .get<trait::Induction>()[Dim]
                           .template get<VariableT>()};
          auto Name{Induct.template get<AST>()->getName()};
          Str.append(Name.begin(), Name.end());
        },
        Str);
    Str.push_back(')');
  } else if (Parallel.getClauses().get<trait::DirectAccess>().empty()) {
    Str.push_back('(');
    auto NestSize{
        std::to_string(Parallel.getClauses().get<trait::Induction>().size())};
    Str.append(NestSize.begin(), NestSize.end());
    Str.push_back(')');
  } else {
    addParallelMapping(Parallel, Str);
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
  if (!Parallel.getClauses().get<Shadow>().empty()) {
    Str.append(
        {'s', 'h', 'a', 'd', 'o', 'w', '_', 'r', 'e', 'n', 'e', 'w', '('});
    for (auto &Shadow : Parallel.getClauses().get<Shadow>()) {
      addShadow(Shadow);
      Str.push_back(',');
    }
    Str.pop_back();
    Str.push_back(')');
  }
  if (!Parallel.getClauses().get<trait::Dependence>().empty()) {
    Str.append({'a', 'c', 'r', 'o', 's', 's', '('});
    for (auto &Across : Parallel.getClauses().get<trait::Dependence>()) {
      addShadow(Across);
      Str.push_back(',');
    }
    Str.pop_back();
    Str.push_back(')');
  }
  if (!Parallel.getClauses().get<Remote>().empty()) {
    Str.append(
        {'r', 'e', 'm', 'o', 't', 'e', '_', 'a', 'c', 'c', 'e', 's', 's', '('});
    for (auto &R : Parallel.getClauses().get<Remote>()) {
      addVar(
          R,
          [&Parallel](unsigned Dim, SmallVectorImpl<char> &Str) {
            auto &Induct{Parallel.getClauses()
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
  addClauseIfNeed(" private", Parallel.getClauses().get<trait::Private>(),
                  Str);
  addReductionIfNeed(Parallel.getClauses().get<trait::Reduction>(), Str);
}

static void pragmaRegionStr(const PragmaRegion &R,
                            SmallVectorImpl<char> &Str) {
  getPragmaText(DirectiveId::DvmRegion, Str);
  Str.resize(Str.size() - 1);
  addClauseIfNeed(" in", R.getClauses().get<trait::ReadOccurred>(), Str);
  addClauseIfNeed(" out", R.getClauses().get<trait::WriteOccurred>(), Str);
  addClauseIfNeed(" local", R.getClauses().get<trait::Private>(), Str);
  if (R.isHostOnly()) {
    StringRef Targets{" targets(HOST)"};
    Str.append(Targets.begin(), Targets.end());
  }
  Str.push_back('\n');
  Str.push_back('{');
}

static void
addPragmaToStmt(const InsertLocation &ToInsert,
                ParallelItemRef &PIRef,
                const Twine &Prefix, const Twine &Suffix,
                PragmaToLocations &PragmaLocations,
                std::vector<ParallelItem *> &NotOptimizedPragmas,
                LocationToPragmas &PragmasToInsert) {
  SmallString<128> PragmaStr;
  Prefix.toVector(PragmaStr);
  PragmaLocations.try_emplace(PIRef, ToInsert);
  if (auto P{dyn_cast<PragmaParallel>(PIRef)}) {
    pragmaParallelStr(*P, PragmaStr);
  } else if (auto P{dyn_cast<PragmaRegion>(PIRef)}) {
    pragmaRegionStr(*P, PragmaStr);
  } else if (auto P{dyn_cast<PragmaRealign>(PIRef)}) {
    pragmaRealignStr(*P, PragmaStr);
  } else if (auto *Marker{dyn_cast<ParallelMarker<PragmaRegion>>(PIRef)}) {
    PragmaStr = "}";
  } else if (isa<PragmaData>(PIRef) || isa<ParallelMarker<PragmaData>>(PIRef)) {
    if (ToInsert) {
      for (auto &Info : ToInsert) {
        auto &PragmaLoc{
            *PragmasToInsert.try_emplace(Info.getStmt(), ToInsert.TfmCtx)
                 .first};
        // Mark the position of the directive in the source code. It will be
        // later created their only if necessary.
        if (Info.isBefore())
          PragmaLoc.get<Insertion>().Before.emplace_back(
              &*PIRef, "", Info.getKind(), nullptr);
        else
          PragmaLoc.get<Insertion>().After.emplace_back(
              &*PIRef, "", Info.getKind(), nullptr);
      }
    }
    if (auto *PD{dyn_cast<PragmaData>(PIRef)}; PD && PD->parent_empty())
      NotOptimizedPragmas.push_back(PD);
    return;
  } else {
    llvm_unreachable("An unknown pragma has been attached to a loop!");
  }
  Suffix.toVector(PragmaStr);
  assert(ToInsert && "A location for a pragma must be known!");
  for (auto &Info : ToInsert) {
    auto &PragmaLoc{
        *PragmasToInsert.try_emplace(Info.getStmt(), ToInsert.TfmCtx).first};
    if (Info.isBefore())
      PragmaLoc.second.Before.emplace_back(&*PIRef, std::move(PragmaStr),
                                           Info.getKind(), nullptr);
    else
      PragmaLoc.second.After.emplace_back(&*PIRef, std::move(PragmaStr),
                                          Info.getKind(), nullptr);
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
static bool pragmaDataAppendVars(FilterT Filter, const PragmaData &PD,
    SmallVectorImpl<char> &Str) {
  if (PD.getMemory().empty())
    return false;
  if constexpr (std::is_same_v<decltype(Filter), std::true_type>)
    return addVarList(PD.getMemory(), Str) != 0;
  else
    return addVarList(PD.getMemory(), std::move(Filter), Str) != 0;
}


template <typename FilterT>
static bool pragmaDataStr(FilterT Filter, const PragmaData &PD,
    SmallVectorImpl<char> &Str) {
  if (PD.getMemory().empty())
    return false;
  getPragmaText(static_cast<DirectiveId>(PD.getKind()), Str);
  // Remove the last '\n'.
  Str.pop_back();
  struct OnReturnT {
    ~OnReturnT() {
      if (isa<PragmaRemoteAccess>(PD))
        Str.append({'\n', '{'});
    }
    const PragmaData &PD;
    SmallVectorImpl<char> &Str;
  } OnReturn{PD, Str};
  if constexpr (std::is_same_v<decltype(Filter), std::true_type>)
    return addVarList(PD.getMemory(), Str) != 0;
  else
    return addVarList(PD.getMemory(), std::move(Filter), Str) != 0;
}

static inline bool pragmaDataStr(const PragmaData &PD,
    SmallVectorImpl<char> &Str) {
  return pragmaDataStr(std::true_type{}, PD, Str);
}

static void pragmaStr(const ParallelItem &PI,
                      SmallVectorImpl<char> &PragmaStr) {
  if (isa<PragmaParallel>(PI))
    pragmaParallelStr(cast<PragmaParallel>(PI), PragmaStr);
  else if (isa<PragmaRegion>(PI))
    pragmaRegionStr(cast<PragmaRegion>(PI), PragmaStr);
  else if (isa<PragmaRealign>(PI))
    pragmaRealignStr(cast<PragmaRealign>(PI), PragmaStr);
  else if (isa<PragmaData>(PI))
    pragmaDataStr(cast<PragmaData>(PI), PragmaStr);
}

template<bool IsError>
static void emitInsertIssue(const InsertLocation &ToInsert,
                            const ParallelItem &PI) {
  auto *TfmCtx{cast<ClangTransformationContext>(ToInsert.TfmCtx)};
  auto &Diags{TfmCtx->getContext().getDiagnostics()};
  SmallString<128> PragmaStr;
  if (PI.isMarker()) {
    PragmaStr += "(marker) ";
    if (auto Marker{dyn_cast<ParallelMarker<PragmaData>>(&PI)})
      pragmaStr(*Marker->getBase(), PragmaStr);
    else if (auto Marker{dyn_cast<ParallelMarker<PragmaRegion>>(&PI)})
      pragmaStr(*Marker->getBase(), PragmaStr);
  } else {
    pragmaStr(PI, PragmaStr);
  }
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: " << (IsError ? "error" : "warning")
                    << " unable to insert "
                    << getName(static_cast<DirectiveId>(PI.getKind())) << " "
                    << &PI << "\n");
  if constexpr (IsError)
    toDiag(Diags, ToInsert.Anchor, tsar::diag::err_directive_insert_unable)
        << StringRef(PragmaStr).trim();
  else
    toDiag(Diags, ToInsert.Anchor, tsar::diag::warn_directive_insert_unable)
        << StringRef(PragmaStr).trim();
  for (auto &Info : ToInsert)
    if (!Info.isValid()) {
      LLVM_DEBUG(dbgs() << "[DVMH WRITER]: unable to instantiate at ";
                 Info.isBefore()
                     ? Info->getBeginLoc().print(
                           dbgs(), TfmCtx->getContext().getSourceManager())
                     : Info->getEndLoc().print(
                           dbgs(), TfmCtx->getContext().getSourceManager()));
      toDiag(Diags, Info.isBefore() ? Info->getBeginLoc() : Info->getEndLoc(),
             tsar::diag::note_directive_instantiate_unable);
    }
}

static bool tryToIgnoreDirective(const InsertLocation &ToInsert,
                                 ParallelItem &PI) {
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
  emitInsertIssue<true>(ToInsert, PI);
  return false;
}

/// Return true if a specified variable `A` is available in a scope which
/// contains a pragma at location `Loc`.
static bool isPragmaInDeclScope(ParentMapContext &ParentCtx, const Stmt &Loc,
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
      V && !isa<ParmVarDecl>(V->get<AST>()) &&
      V->get<AST>()->getDeclContext()->isFunctionOrMethod()) {
    auto *DS{skipDecls(DynTypedNode::create(*V->get<AST>()))};
    return !skipUntil(*DS, Loc);
  }
  return true;
}

static void insertPragmaData(PragmaToLocations &PragmaLocations,
                             LocationToPragmas &PragmasToInsert) {
  auto isMergeableMarker = [&PragmaLocations](const auto &LHSItr, const auto &RHSItr) {
    auto *LHSMarker{std::get<ParallelMarker<PragmaData> *>(*LHSItr)};
    auto *RHSMarker{std::get<ParallelMarker<PragmaData> *>(*RHSItr)};
    if (LHSMarker == RHSMarker)
      return true;
    auto LHSToInsert{
        PragmaLocations.find_as(LHSMarker)->template get<InsertLocation>()};
    auto RHSToInsert{
        PragmaLocations.find_as(RHSMarker)->template get<InsertLocation>()};
    if (LHSToInsert.isInvalid() || RHSToInsert.isInvalid() ||
        LHSToInsert.TfmCtx != RHSToInsert.TfmCtx ||
        LHSToInsert.size() != RHSToInsert.size())
      return false;
    for (auto LHSI{LHSToInsert.begin()}, RHSI{RHSToInsert.begin()},
         LHSEI{LHSToInsert.end()};
         LHSI != LHSEI; ++LHSI, ++RHSI) {
      if (*LHSI != *RHSI)
        return false;
    }
    return true;
  };
  for (auto &Pair : PragmasToInsert) {
    auto &&S{Pair.first};
    auto &&Position{Pair.second};
    auto &ASTCtx{
        cast<ClangTransformationContext>(Position.TfmCtx)->getContext()};
    auto &ParentCtx{ASTCtx.getParentMapContext()};
    auto addPragmaBefore = [&Position, &ParentCtx, S](auto &CurrItr,
                                                      auto &Vars) {
      auto NotEmptyPragma{false};
      auto &PragmaStr{std::get<Insertion::PragmaString>(*CurrItr)};
      auto StashPragmaSize{PragmaStr.size()};
      getPragmaText(static_cast<DirectiveId>(
                        std::get<ParallelItem *>(*CurrItr)->getKind()),
                    PragmaStr);
      PragmaStr.pop_back();
      if (auto *DS{dyn_cast<DeclStmt>(S)})
        NotEmptyPragma |= addVarList(
            Vars,
            [DS, &ParentCtx, S](const dvmh::Align &A) {
              if (!isPragmaInDeclScope(ParentCtx, *S, A))
                return false;
              if (auto *V{std::get_if<VariableT>(&A.Target)})
                return !is_contained(DS->getDeclGroup(), V->get<AST>());
              return true;
            },
            PragmaStr);
      else
        NotEmptyPragma |= addVarList(
            Vars,
            [&ParentCtx, S](const dvmh::Align &A) {
              return isPragmaInDeclScope(ParentCtx, *S, A);
            },
            PragmaStr);
      if (isa<PragmaRemoteAccess>(std::get<ParallelItem *>(*CurrItr)))
        PragmaStr += "\n{";
      if (!NotEmptyPragma)
        PragmaStr.resize(StashPragmaSize);
      else
        PragmaStr += "\n";
    };
    auto CurrItr{Position.Before.end()};
    AlignVarListT Vars;
    for (auto I{Position.Before.begin()}, EI{Position.Before.end()}; I != EI;
         ++I) {
      auto *PD{dyn_cast<PragmaData>(std::get<ParallelItem *>(*I))};
      if (!PD || PD->isInvalid() || PD->isSkipped() || PD->getMemory().empty())
        continue;
      if (CurrItr == Position.Before.end()) {
        // Merge similar pragmas to a single string and attach it to the first
        // merged pragma.
        CurrItr = I;
      } else if (std::get<ParallelItem *>(*CurrItr)->getKind() !=
                     std::get<ParallelItem *>(*I)->getKind() ||
                 !isMergeableMarker(CurrItr, I)) {
        addPragmaBefore(CurrItr, Vars);
        Vars.clear();
        CurrItr = I;
      }
      Vars.insert(PD->getMemory().begin(), PD->getMemory().end());
    }
    if (CurrItr != Position.Before.end()) {
      addPragmaBefore(CurrItr, Vars);
      Vars.clear();
      CurrItr = Position.Before.end();
    }
    auto addPragmaAfter = [&Position, &ParentCtx, S](auto &CurrItr,
                                                     auto &Vars) {
      auto NotEmptyPragma{false};
      auto &PragmaStr{std::get<Insertion::PragmaString>(*CurrItr)};
      auto StashPragmaSize{PragmaStr.size()};
      PragmaStr += "\n";
      getPragmaText(static_cast<DirectiveId>(
                        std::get<ParallelItem *>(*CurrItr)->getKind()),
                    PragmaStr);
      PragmaStr.pop_back();
      NotEmptyPragma |= addVarList(
          Vars,
          [&ParentCtx, S](const dvmh::Align &A) {
            return isPragmaInDeclScope(ParentCtx, *S, A);
          },
          PragmaStr);
      if (isa<PragmaRemoteAccess>(std::get<ParallelItem *>(*CurrItr)))
        PragmaStr += "\n{";
      if (!NotEmptyPragma)
        PragmaStr.resize(StashPragmaSize);
    };
    CurrItr = Position.After.end();
    for (auto I{Position.After.begin()}, EI{Position.After.end()}; I != EI;
         ++I) {
      auto *PD{dyn_cast<PragmaData>(std::get<ParallelItem *>(*I))};
      if (!PD || PD->isInvalid() || PD->isSkipped() || PD->getMemory().empty())
        continue;
      if (CurrItr == Position.After.end()) {
        // Merge similar pragmas to a single string and attach it to the first
        // merged pragma.
        CurrItr = I;
      } else if (std::get<ParallelItem *>(*CurrItr)->getKind() !=
                     std::get<ParallelItem *>(*I)->getKind() ||
                 !isMergeableMarker(CurrItr, I)) {
        addPragmaAfter(CurrItr, Vars);
        Vars.clear();
        CurrItr = I;
      }
      Vars.insert(PD->getMemory().begin(), PD->getMemory().end());
    }
    if (CurrItr != Position.After.end()) {
      addPragmaAfter(CurrItr, Vars);
      Vars.clear();
      CurrItr = Position.After.end();
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

namespace {
struct DeclStmtSearch : public RecursiveASTVisitor<DeclStmtSearch> {
  bool VisitDeclStmt(DeclStmt *S) {
    Found = S;
    return true;
  }
  DeclStmt *Found{nullptr};
};
}

#ifdef LLVM_DEBUG
static void
printReplacementTree(ArrayRef<ParallelItem *> NotOptimizedPragmas,
                     const PragmaToLocations &PragmaLocations,
                     LocationToPragmas &PragmasToInsert) {
  traversePragmaDataPO(NotOptimizedPragmas,
                       [&PragmaLocations, &PragmasToInsert](ParallelItem *PI) {
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
                         if (auto PIItr{PragmaLocations.find_as(PI)};
                             PIItr != PragmaLocations.end()) {
                           auto &&[PIRef, ToInsert] = *PIItr;
                           dbgs() << "location: ";
                           ToInsert.print(dbgs());
                         } else {
                           dbgs() << "stub ";
                         }
                         dbgs() << "\n";
                       });
}

static void insertLocationLog(const ParallelItemRef &PIRef,
                              const InsertLocation &ToInsert) {
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
  dbgs() << " ";
  ToInsert.print(dbgs());
  dbgs() << "\n";
}
#endif

static bool canMergeDirectives(const Stmt *Loc,
                               const Insertion::PragmaList &Pragmas,
                               PragmaToLocations &PragmaLocations) {
  for (auto &P : Pragmas) {
    if (std::get<InsertLocation::Kind>(P) == InsertLocation::Merge)
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
          auto ToInsertItr{
              PragmaLocations.find_as(std::get<ParallelItem *>(P))};
          assert(ToInsertItr != PragmaLocations.end() &&
                 "Directive locations must be cached!");
          auto InfoItr{find(ToInsertItr->get<InsertLocation>(), Loc)};
          assert(InfoItr != ToInsertItr->get<InsertLocation>().end() &&
                 "Invalid location for the directive!");
          InfoItr->setIsValid(false);
          if (!tryToIgnoreDirective(ToInsertItr->get<InsertLocation>(), *PD))
            return false;
        }
  }
  return true;
}

static bool closeRemoteAccesses(PragmaToLocations &PragmaLocations,
                                LocationToPragmas &PragmasToInsert) {
  for (auto &&[PIRef, ToInsert] : PragmaLocations)
    if (ToInsert)
      if (auto *Marker{dyn_cast<ParallelMarker<PragmaRemoteAccess>>(PIRef)})
        if (auto *Remote{Marker->getBase()};
            !Remote->isInvalid() && !Remote->isSkipped()) {
          auto &&[RemoteRef, RemoteToInsert] = *PragmaLocations.find_as(Remote);
          bool IsEmpty{true};
          for (auto &I : RemoteToInsert) {
            if (!I.isValid())
              continue;
            auto Itr{find_if(I.isBefore() ? PragmasToInsert[I.getStmt()].Before
                                          : PragmasToInsert[I.getStmt()].After,
                             [Remote](auto &V) {
                               return std::get<ParallelItem *>(V) == Remote;
                             })};
            assert(Itr != (I.isBefore()
                               ? PragmasToInsert[I.getStmt()].Before.end()
                               : PragmasToInsert[I.getStmt()].After.end()) &&
                   "Insertion point must be known!");
            IsEmpty &= std::get<Insertion::PragmaString>(*Itr).empty();
          }
          if (IsEmpty)
            continue;
          auto *TfmCtx{cast<ClangTransformationContext>(ToInsert.TfmCtx)};
          if (RemoteToInsert.size() != 1 || ToInsert.size() != 1) {
            LLVM_DEBUG(dbgs()
                       << "[DVMH WRITER]: unable to insert remote access "
                          "directive: multiple begin/end positions\n");
            emitInsertIssue<true>(RemoteToInsert, *RemoteRef);
            if (RemoteToInsert.size() > 1) {
              for (auto &Info : RemoteToInsert)
                toDiag(TfmCtx->getContext().getDiagnostics(),
                       Info.isBefore() ? Info->getBeginLoc()
                                       : Info->getEndLoc(),
                       tsar::diag::note_directive_multiple_begin);
            }
            if (ToInsert.size() > 1) {
              for (auto &Info : ToInsert)
                toDiag(TfmCtx->getContext().getDiagnostics(),
                       Info.isBefore() ? Info->getBeginLoc()
                                       : Info->getEndLoc(),
                       tsar::diag::note_directive_multiple_end);
            }
            return false;
          }
          auto &ParentCtx{TfmCtx->getContext().getParentMapContext()};
          auto RemoteParent{
              ParentCtx.getParents(*RemoteToInsert.begin()->getStmt())};
          assert(!RemoteParent.empty() &&
                 "Remote access must be inserted in a scope");
          auto &RPS{RemoteParent.begin()->getUnchecked<Stmt>()};
          auto RemoteMarkerParent{
              ParentCtx.getParents(*ToInsert.begin()->getStmt())};
          assert(!RemoteMarkerParent.empty() &&
                 "Remote access must be inserted in a scope");
          auto &RMPS{RemoteMarkerParent.begin()->getUnchecked<Stmt>()};
          if (&RPS != &RMPS) {
            LLVM_DEBUG(
                dbgs()
                << "[DVMH WRITER]: unable to insert remote access directive: "
                   "inconsistent begin and end insertion points\n");
            emitInsertIssue<true>(RemoteToInsert, *RemoteRef);
            toDiag(TfmCtx->getContext().getDiagnostics(),
                   RemoteToInsert.begin()->isBefore()
                       ? (**RemoteToInsert.begin()).getBeginLoc()
                       : (**RemoteToInsert.begin()).getEndLoc(),
                   tsar::diag::note_directive_inconsistent_bounds);
            toDiag(TfmCtx->getContext().getDiagnostics(),
                   ToInsert.begin()->isBefore()
                       ? (**ToInsert.begin()).getBeginLoc()
                       : (**ToInsert.begin()).getEndLoc(),
                   tsar::diag::note_directive_inconsistent_bounds);
            return false;
          }
          if (auto *CS{dyn_cast<CompoundStmt>(&RPS)}) {
            auto I{find(CS->children(), RemoteToInsert.begin()->getStmt())};
            auto EI{find(CS->children(), ToInsert.begin()->getStmt())};
            if (!RemoteToInsert.begin()->isBefore())
              ++I;
            if (!ToInsert.begin()->isBefore())
              ++EI;
            DeclStmtSearch DSS;
            for (I; I != EI; ++I) {
              DSS.TraverseStmt(const_cast<Stmt *>(*I));
              if (DSS.Found) {
                LLVM_DEBUG(dbgs() << "[DVMH WRITER]: unable to insert remote "
                                     "access directive: declaration inside "
                                     "the remote access scope\n");
                emitInsertIssue<true>(RemoteToInsert, *RemoteRef);
                toDiag(TfmCtx->getContext().getDiagnostics(),
                       DSS.Found->getBeginLoc(),
                       tsar::diag::note_directive_declaration_in_scope);
                return false;
              }
            }
          } else {
            DeclStmtSearch DSS;
            DSS.TraverseStmt(
                const_cast<Stmt *>(RemoteToInsert.begin()->getStmt()));
            if (DSS.Found) {
              LLVM_DEBUG(dbgs() << "[DVMH WRITER]: unable to insert remote "
                                   "access directive: declaration inside "
                                   "the remote access scope\n");
              emitInsertIssue<true>(RemoteToInsert, *RemoteRef);
              toDiag(TfmCtx->getContext().getDiagnostics(),
                     DSS.Found->getBeginLoc(),
                     tsar::diag::note_directive_declaration_in_scope);
              return false;
            }
          }
          for (auto &Info : ToInsert)
            if (Info.isBefore())
              PragmasToInsert[Info.getStmt()].Before.emplace_back(
                  Marker, "\n}", InsertLocation::Bind, nullptr);
            else
              PragmasToInsert[Info.getStmt()].After.emplace_back(
                  Marker, "\n}", InsertLocation::Bind, nullptr);
        }
  return true;
}

bool ClangDVMHWriter::runOnModule(llvm::Module &M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  ClangDVMHWriterProvider::initialize<TransformationEnginePass>(
      [&TfmInfo](TransformationEnginePass &Wrapper) {
        Wrapper.set(TfmInfo.get());
      });
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: insert parallelization directives\n");
  std::vector<ParallelItem *> NotOptimizedPragmas;
  PragmaToLocations PragmaLocations;
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
    if (!CU)
      return false;
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
            [&PLocListItr, &PLocItr, F, &LI, TfmCtx, EM, &LM, &PragmaLocations,
             &NotOptimizedPragmas,
             &PragmasToInsert](ParallelBlock::iterator PIItr, auto Tag) {
              ParallelItemRef PIRef{PLocListItr, PLocItr, PIItr,
                                    std::is_same_v<decltype(Tag), Begin>};
              auto ToInsert{
                  findLocationToInsert(PIRef, *F, LI, *TfmCtx, EM, LM)};
              LLVM_DEBUG(insertLocationLog(PIRef, ToInsert));
              if (!ToInsert && !tryToIgnoreDirective(ToInsert, **PIItr))
                return false;
              addPragmaToStmt(ToInsert, PIRef,
                              std::is_same_v<decltype(Tag), Begin> ? "" : "\n",
                              std::is_same_v<decltype(Tag), Begin> ? "\n" : "",
                              PragmaLocations, NotOptimizedPragmas,
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
    if (!canMergeDirectives(S, I.Before, PragmaLocations) ||
        !canMergeDirectives(S, I.After, PragmaLocations))
      return false;
  for (auto &&[PIRef, ToInsert] : PragmaLocations)
    if (auto *Marker{dyn_cast<ParallelMarker<PragmaData>>(PIRef)})
      if (auto *Base{Marker->getBase()}) {
        auto &&[BaseRef, BaseToInsert] = *PragmaLocations.find_as(Base);
        if (!BaseToInsert)
          continue;
        for (auto &I : BaseToInsert) {
          auto &Insertion{PragmasToInsert[I.getStmt()]};
          auto BaseItr{find_if(
              I.isBefore() ? Insertion.Before : Insertion.After,
              [Base](auto &P) { return std::get<ParallelItem *>(P) == Base; })};
          assert(BaseItr != (I.isBefore() ? Insertion.Before.end()
                                          : Insertion.After.end()) &&
                 "Directive representation must be available!");
          std::get<ParallelMarker<PragmaData> *>(*BaseItr) = Marker;
        }
      }
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: IPO root ID: " << &PIP.getIPORoot()
                    << "\n";
             dbgs() << "[DVMH WRITER]: initial replacement tree:\n";
             printReplacementTree(NotOptimizedPragmas, PragmaLocations,
                                  PragmasToInsert));
  std::vector<PragmaData *> POTraverse;
  // Optimize CPU-to-GPU data transfer. Try to skip unnecessary directives.
  // We use post-ordered traversal to propagate the `skip` property in upward
  // direction.
  traversePragmaDataPO(NotOptimizedPragmas, [this, &PIP, &PragmaLocations,
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
    auto PIItr{PragmaLocations.find_as(PI)};
    assert(PIItr != PragmaLocations.end() &&
           "Internal pragmas must be cached!");
    auto &&[PIRef, ToInsert] = *PIItr;
    if (PD->getMemory().empty()) {
      PD->skip();
    } else if (!ToInsert.isAlwaysInvalid() &&
               all_of(ToInsert, [PD, &PragmasToInsert,
                                 TfmCtx = ToInsert.TfmCtx](auto &Loc) {
                 if (Loc.isBefore()) {
                   auto *S{Loc.getStmt()};
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
                 auto &Position{PragmasToInsert[Loc.getStmt()]};
                 auto &ASTCtx{
                     cast<ClangTransformationContext>(TfmCtx)->getContext()};
                 auto &ParentCtx{ASTCtx.getParentMapContext()};
                 return all_of(
                     PD->getMemory(), [&ParentCtx, &Loc](const dvmh::Align &A) {
                       return !isPragmaInDeclScope(ParentCtx, *Loc, A);
                     });
               })) {
      // Do not mention variables in a directive if it has not been
      // declared yet.
      LLVM_DEBUG(
          dbgs()
          << "[DVMH WRITER]: skip directive outside the declaration scope "
          << PD << "\n");
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
      auto ToInsertItr{PragmaLocations.find_as(PI)};
      assert(ToInsertItr != PragmaLocations.end() &&
             "Insert location must be known!");
      auto &Diags{cast<ClangTransformationContext>(
                      ToInsertItr->get<InsertLocation>().TfmCtx)
                      ->getContext()
                      .getDiagnostics()};
      SmallString<128> PragmaStr;
      pragmaDataStr(*cast<PragmaData>(PI), PragmaStr);
      toDiag(Diags, ToInsertItr->get<InsertLocation>().Anchor,
             tsar::diag::warn_directive_dt_optimize_unable)
          << StringRef(PragmaStr).trim();
      if (cast<PragmaData>(PI)->isInvalid()) {
        IsOk = false;
        LLVM_DEBUG(
            dbgs() << "[DVMH WRITER]: error: unable to insert data directive "
                   << PI << "\n");
        emitInsertIssue<true>(ToInsertItr->get<InsertLocation>(), *PI);
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
      if (PD->parent_empty()) {
        PD->actualize();
      } else if (!PD->isSkipped()) {
        PD->skip();
        if (PD->isInvalid())
          emitInsertIssue<false>(
              PragmaLocations.find_as(PD)->get<InsertLocation>(), *PD);
      }
    }
  }
  for (auto &&[Position, Insertion] : PragmasToInsert) {
    if (!Insertion.Before.empty())
      stable_sort(Insertion.Before, [](auto &LHS, auto &RHS) {
        return (std::get<InsertLocation::Kind>(LHS) !=
                    std::get<InsertLocation::Kind>(RHS) &&
                (std::get<InsertLocation::Kind>(LHS) == InsertLocation::Bind ||
                 std::get<InsertLocation::Kind>(RHS) == InsertLocation::Bind))
                   ? std::get<InsertLocation::Kind>(LHS) !=
                             InsertLocation::Bind &&
                         std::get<InsertLocation::Kind>(RHS) ==
                             InsertLocation::Bind
                   : !isa<PragmaRemoteAccess>(std::get<ParallelItem *>(LHS)) &&
                         isa<PragmaRemoteAccess>(std::get<ParallelItem *>(RHS));
      });
    if (!Insertion.After.empty())
      stable_sort(Insertion.After, [](auto &LHS, auto &RHS) {
        return (std::get<InsertLocation::Kind>(LHS) !=
                    std::get<InsertLocation::Kind>(RHS) &&
                (std::get<InsertLocation::Kind>(LHS) == InsertLocation::Bind ||
                 std::get<InsertLocation::Kind>(RHS) == InsertLocation::Bind))
                   ? std::get<InsertLocation::Kind>(LHS) ==
                             InsertLocation::Bind &&
                         std::get<InsertLocation::Kind>(RHS) !=
                             InsertLocation::Bind
                   : !isa<PragmaRemoteAccess>(std::get<ParallelItem *>(LHS)) &&
                         isa<PragmaRemoteAccess>(std::get<ParallelItem *>(RHS));
      });
  }
  LLVM_DEBUG(dbgs() << "[DVMH WRITER]: optimized replacement tree:\n";
             printReplacementTree(NotOptimizedPragmas, PragmaLocations,
                                  PragmasToInsert));
  // Build pragmas for necessary data transfer directives.
  insertPragmaData(PragmaLocations, PragmasToInsert);
  if (!closeRemoteAccesses(PragmaLocations, PragmasToInsert))
    return false;
  // Update sources.
  for (auto &&[ToInsert, Pragmas] : PragmasToInsert) {
    auto BeginLoc{ToInsert->getBeginLoc()};
    auto TfmCtx{cast<ClangTransformationContext>(Pragmas.TfmCtx)};
    bool IsBeginChanged{false};
    for (auto &&[PI, Str, BindToStmt, Marker] : Pragmas.Before)
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
    for (auto &&[PI, Str, BindToStmt, Marker] : Pragmas.After)
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
