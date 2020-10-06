//===-- DVMHSMAutoPar.cpp - OpenMP Based Parallelization (Clang) -*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a pass to perform DVMH-based auto parallelization for
// shared memory.
//
//===----------------------------------------------------------------------===//

#include "SharedMemoryAutoPar.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-sm-parallel"

namespace {
/// Sequence which determines an order of parallel constructs in a source code.
/// This is similar to a basic block in a control-flow graph.
using ParallelBlock = llvm::SmallVector<std::unique_ptr<ParallelItem>, 4>;

/// This determine location in a source code to insert parallel constructs.
struct ParallelLocation {
  /// Source-code item which implies parallel constructs.
  PointerUnion<MDNode *, Instruction *> Anchor;

  /// Parallel constructs before a specified anchor.
  ParallelBlock Entry;

  /// Parallel constructs after a specified anchor.
  ParallelBlock Exit;
};

/// This represents results of program parallelization.
class Parallelization {
  /// Collection of basic blocks with attached parallel blocks to them.
  using ParallelBlocks = llvm::DenseMap<
      llvm::BasicBlock *, llvm::SmallVector<ParallelLocation, 1>,
      llvm::DenseMapInfo<llvm::BasicBlock *>,
      TaggedDenseMapPair<bcl::tagged<llvm::BasicBlock *, llvm::BasicBlock>,
                         bcl::tagged<llvm::SmallVector<ParallelLocation, 1>,
                                     ParallelLocation>>>;

  /// Functions which contains parallel constructs.
  using ParallelFunctions = llvm::SmallPtrSet<llvm::Function *, 32>;

public:
  using iterator = ParallelBlocks::iterator;
  using const_iterator = ParallelBlocks::const_iterator;

  iterator begin() { return mParallelBlocks.begin(); }
  iterator end() { return mParallelBlocks.end(); }

  const_iterator begin() const { return mParallelBlocks.begin(); }
  const_iterator end() const { return mParallelBlocks.end(); }

  /// Return false if program has not been parallelized.
  bool empty() const { return mParallelBlocks.empty(); }

  /// Attach a new parallel block to a specified one and mark corresponding
  /// function as parallel.
  template<class... Ts>
  std::pair<iterator, bool> try_emplace(llvm::BasicBlock *BB, Ts &&... Args) {
    mParallelFuncs.insert(BB->getParent());
    return mParallelBlocks.try_emplace(BB, std::forward<Ts>(Args)...);
  }

  iterator find(const llvm::BasicBlock *BB) { return mParallelBlocks.find(BB); }
  const_iterator find(const llvm::BasicBlock *BB) const {
    return mParallelBlocks.find(BB);
  }

  using function_iterator = ParallelFunctions::iterator;
  using const_function_iterator = ParallelFunctions::const_iterator;

  function_iterator func_begin() { return mParallelFuncs.begin(); }
  function_iterator func_end() { return mParallelFuncs.end(); }

  const_function_iterator func_begin() const { return mParallelFuncs.begin(); }
  const_function_iterator func_end() const { return mParallelFuncs.end(); }

private:
  llvm::SmallPtrSet<llvm::Function *, 32> mParallelFuncs;
  ParallelBlocks mParallelBlocks;
};

class PragmaRegion : public ParallelLevel {
public:
  using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<SortedVarListT, trait::ReadOccurred>,
                        bcl::tagged<SortedVarListT, trait::WriteOccurred>>;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmRegion);
  }

  explicit PragmaRegion(bool HostOnly = false)
      : ParallelLevel(static_cast<unsigned>(DirectiveId::DvmRegion), false,
                      nullptr), mHostOnly(HostOnly) {}

  ClauseList &getClauses() noexcept { return mClauses; }
  const ClauseList &getClauses() const noexcept { return mClauses; }

  void setHostOnly(bool HostOnly = true) { mHostOnly = HostOnly; }
  bool isHostOnly() const noexcept { return mHostOnly; }

private:
  ClauseList mClauses;
  bool mHostOnly;
};

class PragmaActual : public ParallelItem {
public:
  using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmActual);
  }

  PragmaActual()
      : ParallelItem(static_cast<unsigned>(DirectiveId::DvmActual), true,
                     nullptr) {}

  SortedVarListT &getMemory() noexcept { return mMemory; }
  const SortedVarListT &getMemory() const noexcept { return mMemory; }

private:
  SortedVarListT mMemory;
};

class PragmaGetActual : public ParallelItem {
public:
  using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmGetActual);
  }

  PragmaGetActual()
      : ParallelItem(static_cast<unsigned>(DirectiveId::DvmGetActual), true,
                     nullptr) {}

  SortedVarListT &getMemory() noexcept { return mMemory; }
  const SortedVarListT &getMemory() const noexcept { return mMemory; }

private:
  SortedVarListT mMemory;
};

class PragmaParallel : public ParallelItem {
public:
  using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
  using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
  using LoopNestT = SmallVector<ObjectID, 4>;
  using VarMappingT =
      DenseMap<ObjectID, SmallVector<std::pair<ObjectID, bool>, 4>>;

  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>,
                        bcl::tagged<LoopNestT, trait::Induction>,
                        bcl::tagged<VarMappingT, trait::DirectAccess>>;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmParallel);
  }

  PragmaParallel(PragmaRegion *Parent)
      : ParallelItem(static_cast<unsigned>(DirectiveId::DvmParallel), false,
                     Parent) {}

  ClauseList &getClauses() noexcept { return mClauses; }
  const ClauseList &getClauses() const noexcept { return mClauses; }

private:
  ClauseList mClauses;
};

/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangDVMHSMParallelization : public ClangSMParallelization {
public:
  static char ID;
  ClangDVMHSMParallelization() : ClangSMParallelization(ID) {
    initializeClangDVMHSMParallelizationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

private:
  ParallelItem * exploitParallelism(const DFLoop &IR, const clang::ForStmt &AST,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTDepInfo, ParallelItem *PI) override;

  void optimizeLevel(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  Parallelization mParallelizationInfo;
};

} // namespace

ParallelItem *ClangDVMHSMParallelization::exploitParallelism(
    const DFLoop &IR, const clang::ForStmt &AST,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis, ParallelItem *PI) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
      !ASTDepInfo.get<trait::LastPrivate>().empty()) {
    if (PI)
      PI->finalize();
    return PI;
  }
  if (PI) {
    auto DVMHParallel = cast<PragmaParallel>(PI);
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    if (PL[IR.getLoop()].isHostOnly() ||
        ASTDepInfo.get<trait::Private>() !=
            DVMHParallel->getClauses().get<trait::Private>() ||
        ASTDepInfo.get<trait::Reduction>() !=
            DVMHParallel->getClauses().get<trait::Reduction>()) {
      PI->finalize();
      return PI;
    }
  } else {
    std::unique_ptr<PragmaActual> DVMHActual;
    std::unique_ptr<PragmaGetActual> DVMHGetActual;
    auto DVMHRegion = std::make_unique<PragmaRegion>();
    DVMHRegion->finalize();
    auto DVMHParallel = std::make_unique<PragmaParallel>(DVMHRegion.get());
    PI = DVMHParallel.get();
    DVMHParallel->getClauses().get<trait::Private>().insert(
        ASTDepInfo.get<trait::Private>().begin(),
        ASTDepInfo.get<trait::Private>().end());
    for (unsigned I = 0, EI = ASTDepInfo.get<trait::Reduction>().size(); I < EI;
         ++I)
      DVMHParallel->getClauses().get<trait::Reduction>()[I].insert(
          ASTDepInfo.get<trait::Reduction>()[I].begin(),
          ASTDepInfo.get<trait::Reduction>()[I].end());
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    if (!PL[IR.getLoop()].isHostOnly() && ASTRegionAnalysis.evaluateDefUse()) {
      if (!ASTDepInfo.get<trait::ReadOccurred>().empty()) {
        DVMHActual = std::make_unique<PragmaActual>();
            DVMHActual->getMemory()
            .insert(ASTDepInfo.get<trait::ReadOccurred>().begin(),
                    ASTDepInfo.get<trait::ReadOccurred>().end());
        DVMHRegion->getClauses().get<trait::ReadOccurred>().insert(
            ASTDepInfo.get<trait::ReadOccurred>().begin(),
            ASTDepInfo.get<trait::ReadOccurred>().end());
      }
      if (!ASTDepInfo.get<trait::WriteOccurred>().empty()) {
        DVMHGetActual = std::make_unique<PragmaGetActual>();
        DVMHGetActual->getMemory().insert(
            ASTDepInfo.get<trait::WriteOccurred>().begin(),
            ASTDepInfo.get<trait::WriteOccurred>().end());
        DVMHRegion->getClauses().get<trait::WriteOccurred>().insert(
            ASTDepInfo.get<trait::WriteOccurred>().begin(),
            ASTDepInfo.get<trait::WriteOccurred>().end());
      }
      DVMHRegion->getClauses().get<trait::Private>().insert(
          ASTDepInfo.get<trait::Private>().begin(),
          ASTDepInfo.get<trait::Private>().end());
    } else {
      DVMHRegion->setHostOnly(true);
      DVMHParallel->finalize();
    }
    auto EntryInfo =
        mParallelizationInfo.try_emplace(IR.getLoop()->getHeader());
    assert(EntryInfo.second && "Unable to create a parallel block!");
    EntryInfo.first->get<ParallelLocation>().emplace_back();
    EntryInfo.first->get<ParallelLocation>().back().Anchor =
        IR.getLoop()->getLoopID();
    auto ExitingBB = IR.getLoop()->getExitingBlock();
    assert(ExitingBB && "Parallel loop must have a single exit!");
    ParallelLocation *ExitLoc = nullptr;
    if (ExitingBB == IR.getLoop()->getHeader()) {
      ExitLoc = &EntryInfo.first->get<ParallelLocation>().back();
    } else {
      auto ExitInfo = mParallelizationInfo.try_emplace(ExitingBB);
      assert(ExitInfo.second && "Unable to create a parallel block!");
      ExitInfo.first->get<ParallelLocation>().emplace_back();
      ExitLoc = &ExitInfo.first->get<ParallelLocation>().back();
      ExitLoc->Anchor = IR.getLoop()->getLoopID();
    }
    ExitLoc->Exit.push_back(
        std::make_unique<ParallelMarker<PragmaRegion>>(0, DVMHRegion.get()));
    if (DVMHActual)
      EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
          std::move(DVMHActual));
    EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
        std::move(DVMHRegion));
    EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
        std::move(DVMHParallel));
    if (DVMHGetActual)
      ExitLoc->Exit.push_back(std::move(DVMHGetActual));
  }
  cast<PragmaParallel>(PI)->getClauses().get<trait::Induction>().emplace_back(
      IR.getLoop()->getLoopID());
   auto &PerfectInfo =
      Provider.value<ClangPerfectLoopPass *>()->getPerfectLoopInfo();
  if (!PI->isFinal() && !PerfectInfo.count(&IR) || IR.getNumRegions() == 0)
    PI->finalize();
  return PI;
}

template <typename ItrT>
static void optimizeLevelImpl(ItrT I, ItrT EI, const FunctionAnalysis &Provider,
    const DIArrayAccessInfo &AccessInfo,
    Parallelization &ParallelizationInfo) {
  for (; I != EI; ++I) {
    auto ID = (*I)->getLoopID();
    if (!ID)
      continue;
    auto PLocs = ParallelizationInfo.find((*I)->getHeader());
    if (PLocs == ParallelizationInfo.end())
      continue;
    auto PL = find_if(
        PLocs->template get<ParallelLocation>(), [ID](ParallelLocation &PL) {
          return PL.Anchor.is<MDNode *>() && PL.Anchor.get<MDNode *>() == ID;
        });
    if (PL == PLocs->template get<ParallelLocation>().end())
      continue;
    auto PI = find_if(PL->Entry,
                      [](auto &PI) { return isa<PragmaParallel>(PI.get()); });
    if (PI == PL->Entry.end())
      continue;
    auto &DVMHParallel = cast<PragmaParallel>(**PI);
    for (auto &Access : AccessInfo.scope_accesses(ID)) {
      if (!isa<DIEstimateMemory>(Access.getArray()))
        continue;
      auto MappingItr =
          DVMHParallel.getClauses()
              .template get<trait::DirectAccess>()
              .try_emplace(Access.getArray()->getAsMDNode(), Access.size(),
                           std::pair<ObjectID, bool>(nullptr, true))
              .first;
      for (auto *Subscript : Access) {
        if (!Subscript || MappingItr->second[Subscript->getDimension()].first)
          continue;
        if (auto *Affine = dyn_cast<DIAffineSubscript>(Subscript)) {
          for (unsigned I = 0, EI = Affine->getNumberOfMonoms(); I < EI; ++I) {
            if (Affine->getMonom(I).Value.isNullValue())
              continue;
            auto Itr =
                find(DVMHParallel.getClauses().template get<trait::Induction>(),
                     Affine->getMonom(I).Column);
            if (Itr != DVMHParallel.getClauses()
                           .template get<trait::Induction>()
                           .end())
              MappingItr->second[Affine->getDimension()] = {
                  *Itr, !Affine->getMonom(I).Value.isNegative() };
          }
        }
      }
    }
  }
}

void ClangDVMHSMParallelization::optimizeLevel(
  PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (!AccessInfo)
    return;
  if (Level.is<Function *>()) {
    auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
    optimizeLevelImpl(LI.begin(), LI.end(), Provider, *AccessInfo,
                      mParallelizationInfo);
  } else {
    optimizeLevelImpl(Level.get<Loop *>()->begin(), Level.get<Loop *>()->end(),
                      Provider, *AccessInfo, mParallelizationInfo);
  }
}

/// Compute inductions for loops in a parallel nest with a specified outermost
/// loop 'L'.
static void getBaseInductionsForNest(
    Loop &L, PragmaParallel &Parallel, const FunctionAnalysis &Provider,
    SmallVectorImpl<std::pair<ObjectID, StringRef>> &Inductions) {
  auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
  auto &RI = Provider.value<DFRegionInfoPass *>()->getRegionInfo();
  auto &MemoryMatcher =
      Provider.value<MemoryMatcherImmutableWrapper *>()->get();
  auto addToInductions = [&Inductions, &CL, &MemoryMatcher, &RI](Loop &L) {
    auto *DFL = RI.getRegionFor(&L);
    assert(DFL && "A parallel directive has been attached to an "
                  "unknown loop!");
    auto CanonicalItr = CL.find_as(DFL);
    auto *Induction = (**CanonicalItr).getInduction();
    assert(Induction &&
           "Induction variable must not be null in canonical loop!");
    auto MatchItr = MemoryMatcher.Matcher.find<IR>(Induction);
    assert(MatchItr != MemoryMatcher.Matcher.end() &&
           "AST-level variable representation must be available!");
    Inductions.emplace_back(L.getLoopID(), MatchItr->get<AST>()->getName());
  };
  addToInductions(L);
  auto *CurrLoop = &L;
  for (unsigned I = 1,
                EI = Parallel.getClauses().get<trait::Induction>().size();
       I < EI; ++I) {
    CurrLoop = *CurrLoop->begin();
    addToInductions(*CurrLoop);
  }
}

static inline void addVarList(
    const ClangDependenceAnalyzer::SortedVarListT &VarInfoList,
    SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I = VarInfoList.begin(), EI = VarInfoList.end();
  Clause.append(I->begin(), I->end());
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    Clause.append(I->begin(), I->end());
  }
  Clause.push_back(')');
}

static void addParallelMapping(Loop &L, PragmaParallel &Parallel,
    const FunctionAnalysis &Provider, SmallVectorImpl<char> &PragmaStr) {
  auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
  auto &RI = Provider.value<DFRegionInfoPass *>()->getRegionInfo();
  auto &DIAT = Provider.value<DIEstimateMemoryPass *>()->getAliasTree();
  auto &MemoryMatcher =
      Provider.value<MemoryMatcherImmutableWrapper *>()->get();
  SmallVector<std::pair<ObjectID, StringRef>, 4> Inductions;
  getBaseInductionsForNest(L, Parallel, Provider, Inductions);
  PragmaStr.push_back('(');
  for (auto &LToI : Inductions) {
    PragmaStr.push_back('[');
    PragmaStr.append(LToI.second.begin(), LToI.second.end());
    PragmaStr.push_back(']');
  }
  PragmaStr.push_back(')');
  // We sort arrays to ensure the same order of variables after
  // different launches of parallelization.
  ClangDependenceAnalyzer::SortedVarListT MappingStr;
  for (auto &Mapping : Parallel.getClauses().get<trait::DirectAccess>()) {
    auto &DIEM = cast<DIEstimateMemory>(*DIAT.find(*Mapping.first));
    SmallString<32> Tie{DIEM.getVariable()->getName()};
    for (auto &Map : Mapping.second) {
      Tie += "[";
      if (Map.first) {
        if (!Map.second)
          Tie += "-";
        Tie += find_if(Inductions, [&Map](auto &LToI) {
                 return LToI.first == Map.first;
               })->second;
      }
      Tie += "]";
    }
    MappingStr.insert(std::string(Tie));
  }
  PragmaStr.append({ ' ', 't', 'i', 'e' });
  addVarList(MappingStr, PragmaStr);
}

static inline void addClauseIfNeed(StringRef Name,
    ClangDependenceAnalyzer::SortedVarListT &Vars,
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
    case trait::Reduction::RK_Xor: RedKind + "xor "; break;
    case trait::Reduction::RK_Max: RedKind += "max"; break;
    case trait::Reduction::RK_Min: RedKind += "min"; break;
    default: llvm_unreachable("Unknown reduction kind!"); break;
    }
    ParallelFor.append({ 'r', 'e', 'd', 'u', 'c', 't', 'i', 'o', 'n' });
    ParallelFor.push_back('(');
    auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
    ParallelFor.append(RedKind.begin(), RedKind.end());
    ParallelFor.push_back('(');
    ParallelFor.append(VarItr->begin(), VarItr->end());
    ParallelFor.push_back(')');
    for (++VarItr; VarItr != VarItrE; ++VarItr) {
      ParallelFor.push_back(',');
      ParallelFor.append(RedKind.begin(), RedKind.end());
      ParallelFor.push_back('(');
      ParallelFor.append(VarItr->begin(), VarItr->end());
      ParallelFor.push_back(')');
    }
    ParallelFor.push_back(')');
  }
}

bool ClangDVMHSMParallelization::runOnModule(llvm::Module &M) {
  ClangSMParallelization::runOnModule(M);
  auto *TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  for (auto F : make_range(mParallelizationInfo.func_begin(),
                           mParallelizationInfo.func_end())) {
    auto Provider = analyzeFunction(*F);
    auto &LI = Provider.value<LoopInfoWrapperPass*>()->getLoopInfo();
    auto &LM = Provider.value<LoopMatcherPass *>()->getMatcher();
    for (auto &BB : *F) {
      auto ParallelItr = mParallelizationInfo.find(&BB);
      if (ParallelItr == mParallelizationInfo.end())
        continue;
      for (auto &PL : ParallelItr->get<ParallelLocation>()) {
        if (PL.Anchor.is<Instruction *>()) {
          llvm_unreachable(
              "Directives cannot be attached to instructions yet!");
          continue;
        }
        auto ID = PL.Anchor.get<MDNode *>();
        auto *L = LI.getLoopFor(&BB);
        while (L->getLoopID() && L->getLoopID() != ID)
          L = L->getParentLoop();
        assert(L &&
               "A parallel directive has been attached to an unknown loop!");
        auto LMatchItr = LM.find<IR>(L);
        assert(LMatchItr != LM.end() &&
               "Unable to find AST representation for a loop!");
        for (auto &PI : PL.Entry) {
          SmallString<128> PragmaStr;
          if (auto *Parallel = dyn_cast<PragmaParallel>(PI.get())) {
            getPragmaText(DirectiveId::DvmParallel, PragmaStr);
            PragmaStr.resize(PragmaStr.size() - 1);
            if (Parallel->getClauses().get<trait::DirectAccess>().empty())
              PragmaStr +=
                  "(" +
                  std::to_string(
                      Parallel->getClauses().get<trait::Induction>().size()) +
                  ")";
            else
              addParallelMapping(*L, *Parallel, Provider, PragmaStr);
            addClauseIfNeed(" private",
                            Parallel->getClauses().get<trait::Private>(),
                            PragmaStr);
            addReductionIfNeed(Parallel->getClauses().get<trait::Reduction>(),
                       PragmaStr);
          } else if (auto *Region = dyn_cast<PragmaRegion>(PI.get())) {
            getPragmaText(DirectiveId::DvmRegion, PragmaStr);
            PragmaStr.resize(PragmaStr.size() - 1);
            addClauseIfNeed(" in",
                            Region->getClauses().get<trait::ReadOccurred>(),
                            PragmaStr);
            addClauseIfNeed(" out",
                            Region->getClauses().get<trait::WriteOccurred>(),
                            PragmaStr);
            addClauseIfNeed(" local",
                            Region->getClauses().get<trait::Private>(),
                            PragmaStr);
            if (Region->isHostOnly())
              PragmaStr += " targets(HOST)";
            PragmaStr += "\n{";
          } else if (auto *Actual = dyn_cast<PragmaActual>(PI.get())) {
            if (Actual->getMemory().empty())
              continue;
            getPragmaText(DirectiveId::DvmActual, PragmaStr);
            PragmaStr.resize(PragmaStr.size() - 1);
            addVarList(Actual->getMemory(), PragmaStr);
          } else {
            llvm_unreachable("An unknown pragma has been attached to a loop!");
          }
          PragmaStr += "\n";
          TfmCtx->getRewriter().InsertTextAfter(
              LMatchItr->get<AST>()->getBeginLoc(), PragmaStr);
        }
        if (PL.Exit.empty())
          continue;
        auto &ASTCtx = TfmCtx->getContext();
        Token SemiTok;
        auto InsertLoc = (!getRawTokenAfter(LMatchItr->get<AST>()->getEndLoc(),
                                            ASTCtx.getSourceManager(),
                                            ASTCtx.getLangOpts(), SemiTok) &&
                          SemiTok.is(tok::semi))
                             ? SemiTok.getLocation()
                             : LMatchItr->get<AST>()->getEndLoc();
        for (auto &PI : PL.Exit) {
          SmallString<128> PragmaStr;
          if (auto *GetActual = dyn_cast<PragmaGetActual>(PI.get())) {
            if (GetActual->getMemory().empty())
              continue;
            getPragmaText(DirectiveId::DvmGetActual, PragmaStr);
            PragmaStr.resize(PragmaStr.size() - 1);
            addVarList(GetActual->getMemory(), PragmaStr);

          } else if (auto *Marker =
                         dyn_cast<ParallelMarker<PragmaRegion>>(PI.get())) {
            PragmaStr = "}";
          } else {
            llvm_unreachable("An unknown pragma has been attached to a loop!");
          }
          PragmaStr += "\n";
          TfmCtx->getRewriter().InsertTextAfterToken(InsertLoc, PragmaStr);
        }
      }
    }
  }
  return false;
}

ModulePass *llvm::createClangDVMHSMParallelization() {
  return new ClangDVMHSMParallelization;
}

char ClangDVMHSMParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(ClangDVMHSMParallelization,
  "clang-dvmh-sm-parallel", "Shared Memory DVMH-based Parallelization (Clang)")
