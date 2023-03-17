//===-- OpenMPAutoPar.cpp - OpenMP Based Parallelization (Clang) -*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file implements a pass to perform OpenMP-based auto parallelization.
//
//===----------------------------------------------------------------------===//

#include "SharedMemoryAutoPar.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/Utils.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <clang/AST/ParentMapContext.h>
#include <llvm/Frontend/OpenMP/OMPConstants.h>

using namespace llvm;
using namespace tsar;

using clang::ASTContext;
using clang::CompoundStmt;
using clang::ForStmt;
using clang::LangOptions;
using clang::SourceLocation;
using clang::SourceManager;
using clang::Stmt;
using clang::Token;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-openmp-parallel"

namespace {
class OMPParallelDirective : public ParallelLevel {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(llvm::omp::OMPD_parallel);
  }

  OMPParallelDirective(bool HostOnly = false)
      : ParallelLevel(static_cast<unsigned>(llvm::omp::OMPD_parallel), false) {}
};

class OMPForDirective : public ParallelLevel {
public:
  using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
  using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
  using DistanceInfo = ClangDependenceAnalyzer::DistanceInfo;
  using LoopNestT = SmallVector<ObjectID, 4>;

  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<SortedVarListT, trait::LastPrivate>,
                        bcl::tagged<SortedVarListT, trait::FirstPrivate>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>,
                        bcl::tagged<LoopNestT, trait::Induction>>;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(llvm::omp::OMPD_for);
  }

  OMPForDirective(OMPParallelDirective &Parent)
      : ParallelLevel(static_cast<unsigned>(llvm::omp::OMPD_for), false) {
    parent_insert(&Parent);
  }

  ClauseList &getClauses() noexcept { return mClauses; }
  const ClauseList &getClauses() const noexcept { return mClauses; }

  void finalize() override;

private:
  ClauseList mClauses;
};

class OMPOrderedDirective : public ParallelItem {
public:
  using OrderedSinkT = SmallVector<trait::DIDependence::DistanceVector, 3>;
  using const_iterator = OrderedSinkT::const_iterator;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(llvm::omp::OMPD_ordered);
  }

  OMPOrderedDirective(OMPForDirective &Parent)
      : ParallelItem(static_cast<unsigned>(llvm::omp::OMPD_ordered), true) {
    parent_insert(&Parent);
  }

  unsigned depth() const noexcept { return mDepth; }

  void reduce_depth(unsigned Depth) {
    if (Depth < depth())
      for (auto &S : mSink)
        S.resize(Depth);
    mDepth = Depth;
  }

  void push_back(const trait::DIDependence::DistanceVector &DV) {
    if (DV.size() < mDepth)
      mDepth = DV.size();
    mSink.emplace_back(DV);
  }

  void push_back(trait::DIDependence::DistanceVector &&DV) {
    if (DV.size() < mDepth)
      mDepth = DV.size();
    mSink.emplace_back(DV);
  }

  bool empty() const { return mSink.empty(); }
  std::size_t size() const { return mSink.size(); }

  const_iterator begin() const { return mSink.begin(); }
  const_iterator end() const { return mSink.end(); }

private:
  OrderedSinkT mSink;
  unsigned mDepth = 0;
};

void OMPForDirective::finalize() {
  ParallelLevel::finalize();
  for (auto &Child : children())
    if (auto *OmpOrdered = dyn_cast<OMPOrderedDirective>(Child))
      OmpOrdered->reduce_depth(getClauses().get<trait::Induction>().size());
}

/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangOpenMPParallelization : public ClangSMParallelization {
public:
  static char ID;
  ClangOpenMPParallelization() : ClangSMParallelization(ID) {
    initializeClangOpenMPParallelizationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

private:
  ParallelItem * exploitParallelism(const DFLoop &DFL,
    const clang::ForStmt &For, const FunctionAnalysis &Provider,
    ClangDependenceAnalyzer &ASTRegionAnalysis, ParallelItem *PI) override;

  void optimizeLevel(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  /// Return true if the parallel nest cannot be extended further and
  /// `omp for` directive have to be finalized or false
  /// otherwise, return None if some errors have occurred.
  Optional<bool> addOrUpdateOrderedIfNeed(const DFLoop &DFL,
    const ClangDependenceAnalyzer &ASTRegionAnalysis,
    OMPForDirective &OmpFor);

  Parallelization mParallelizationInfo;
  SmallVector<bcl::tagged_pair<bcl::tagged<Loop *, Loop>,
                               bcl::tagged<std::unique_ptr<OMPOrderedDirective>,
                                           OMPOrderedDirective>>,
              8>
      mOutermostOrderedLoops;
};

struct ClausePrinter {
  /// Add clause for a `Trait` with variable names from a specified list to
  /// the end of `ParallelFor` pragma.
  template <class Trait>
  void operator()(const OMPForDirective::SortedVarListT &VarInfoList) {
    if (VarInfoList.empty())
      return;
    std::string Clause(Trait::tag::toString());
    Clause.erase(
        std::remove_if(Clause.begin(), Clause.end(), bcl::isWhitespace),
        Clause.end());
    ParallelFor += Clause;
    ParallelFor += '(';
    auto I = VarInfoList.begin(), EI = VarInfoList.end();
    ParallelFor += I->get<AST>()->getName();;
    for (++I; I != EI; ++I)
      (ParallelFor += ", ")
          .append(I->get<AST>()->getName().begin(),
                  I->get<AST>()->getName().end());
    ParallelFor += ')';
  }

  /// Add clauses for all reduction variables from a specified list to
  /// the end of `ParallelFor` pragma.
  template <class Trait>
  void operator()(const OMPForDirective::ReductionVarListT &VarInfoList) {
    unsigned I = trait::Reduction::RK_First;
    unsigned EI = trait::Reduction::RK_NumberOf;
    for (; I < EI; ++I) {
      if (VarInfoList[I].empty())
        continue;
      ParallelFor += "reduction";
      ParallelFor += '(';
      switch (static_cast<trait::Reduction::Kind>(I)) {
      case trait::Reduction::RK_Add: ParallelFor += "+:"; break;
      case trait::Reduction::RK_Mult: ParallelFor += "*:"; break;
      case trait::Reduction::RK_Or: ParallelFor += "|:"; break;
      case trait::Reduction::RK_And: ParallelFor += "&:"; break;
      case trait::Reduction::RK_Xor: ParallelFor += "^:"; break;
      case trait::Reduction::RK_Max: ParallelFor += "max:"; break;
      case trait::Reduction::RK_Min: ParallelFor += "min:"; break;
      default: llvm_unreachable("Unknown reduction kind!"); break;
      }
      auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
      ParallelFor += VarItr->get<AST>()->getName();
      for (++VarItr; VarItr != VarItrE; ++VarItr)
        (ParallelFor += ", ")
            .append(VarItr->get<AST>()->getName().begin(),
                    VarItr->get<AST>()->getName().end());
      ParallelFor += ')';
    }
  }


  template <class Trait>
  void operator()(const OMPForDirective::LoopNestT &Nest) {
    if (Nest.size() > 1)
      ("collapse(" + Twine(Nest.size()) + ")").toVector(ParallelFor);
  }

  SmallString<128> &ParallelFor;
};

trait::DIDependence::DistanceVector makeOrderedRange(
    const ClangDependenceAnalyzer::ASTRegionTraitInfo &ASTDepInfo,
    unsigned EnclosingPNestSize) {
  auto &FirstDep = ASTDepInfo.get<trait::Dependence>().begin()->second;
  auto &InitDV = FirstDep.get<trait::Flow>().empty()
                     ? FirstDep.get<trait::Anti>()
                     : FirstDep.get<trait::Flow>();
  trait::DIDependence::DistanceVector DV(EnclosingPNestSize);
  DV.append(InitDV.begin(), InitDV.end());
  auto update = [EnclosingPNestSize,
                 &DV](const trait::DIDependence::DistanceVector &Dist) {
    if (!Dist.empty()) {
      auto Size = EnclosingPNestSize + Dist.size();
      if (Size < DV.size())
        DV.resize(Size);
      for (unsigned I = EnclosingPNestSize, EI = DV.size(); I < EI; ++I) {
        if (*Dist[I - EnclosingPNestSize].first < *DV[I].first)
          DV[I].first = *Dist[I - EnclosingPNestSize].first;
        if (*Dist[I - EnclosingPNestSize].second > DV[I].second)
          DV[I].second = Dist[I - EnclosingPNestSize].second;
      }
    }
  };
  for (auto &Dep : ASTDepInfo.get<trait::Dependence>()) {
    update(Dep.second.get<trait::Flow>());
    update(Dep.second.get<trait::Anti>());
  }
  assert(DV.size() > EnclosingPNestSize &&
         "At lest one regular dependence must exist at the current level!");
  DV[EnclosingPNestSize].first->setIsSigned(true);
  DV[EnclosingPNestSize].first = - (*DV[EnclosingPNestSize].first);
  DV[EnclosingPNestSize].second->setIsSigned(true);
  DV[EnclosingPNestSize].second = - (*DV[EnclosingPNestSize].second);
  return DV;
}

inline Stmt *getScope(Loop *L,
    const LoopMatcherPass::LoopMatcher &LoopMatcher, ASTContext &ASTCtx) {
  auto &ParentCtx = ASTCtx.getParentMapContext();
  auto Itr = LoopMatcher.find<IR>(L);
  if (Itr == LoopMatcher.end())
    return nullptr;
  return const_cast<Stmt *>(
      ParentCtx.getParents(*Itr->get<AST>()).begin()->get<Stmt>());
}

inline OMPForDirective *isParallel(const Loop *L,
                                   Parallelization &ParallelizationInfo) {
  if (auto ID = L->getLoopID()) {
    auto Ref{ ParallelizationInfo.find<OMPForDirective>(L->getHeader(), ID) };
    return cast_or_null<OMPForDirective>(Ref);
  }
  return nullptr;
}

void mergeRegions(const SmallVectorImpl<Loop *> &ToMerge,
    Parallelization &ParallelizationInfo) {
  assert(ToMerge.size() > 1 && "At least two regions must be specified!");
  auto MergedRegion = ParallelizationInfo.find<OMPParallelDirective>(
      ToMerge.front()->getHeader(), ToMerge.front()->getLoopID(), true);
  auto MergedMarker =
      ParallelizationInfo.find<ParallelMarker<OMPParallelDirective>>(
          getValidExitingBlock(*ToMerge.back()), ToMerge.back()->getLoopID(),
          false);
  cast<ParallelMarker<OMPParallelDirective>>(MergedMarker)
      ->parent_insert(MergedRegion.getUnchecked());
  auto remove = [&ParallelizationInfo](BasicBlock *BB, auto RegionItr,
      auto PEdgeItr, ParallelBlock &OppositePB, ParallelBlock &FromPB) {
    if (FromPB.size() == 1) {
      if (OppositePB.empty() &&
          PEdgeItr->template get<ParallelLocation>().size() == 1)
        ParallelizationInfo.erase(BB);
      else
        FromPB.clear();
    } else {
      FromPB.erase(RegionItr);
    }
  };
  auto removeEndOfRegion = [&remove, &ParallelizationInfo](Loop *L) {
    auto ExitingBB = getValidExitingBlock(*L);
    auto ID = L->getLoopID();
    auto Marker =
        ParallelizationInfo.find<ParallelMarker<OMPParallelDirective>>(
            ExitingBB, ID, false);
    auto &ExitPB = Marker.getPL()->Exit;
    remove(ExitingBB, Marker.getPI(), Marker.getPE(), Marker.getPL()->Entry,
           ExitPB);
  };
  auto removeStartOfRegion = [&remove, &ParallelizationInfo](Loop *L) {
    auto HeaderBB = getValidExitingBlock(*L);
    auto ID = L->getLoopID();
    auto Region =
        ParallelizationInfo.find<OMPParallelDirective>(L->getHeader(), ID);
    auto &EntryPB = Region.getPL()->Entry;
    remove(HeaderBB, Region.getPI(), Region.getPE(), Region.getPL()->Exit,
           EntryPB);
  };
  removeEndOfRegion(ToMerge.front());
  for (auto I = ToMerge.begin() + 1, EI = ToMerge.end() - 1; I != EI; ++I) {
    // All markers should be removed before the corresponding region.
    removeEndOfRegion(*I);
    removeStartOfRegion(*I);
  }
  removeStartOfRegion(ToMerge.back());
}

template<typename ItrT>
void mergeSiblingRegions(ItrT I, ItrT EI,
    const FunctionAnalysis &Provider, ASTContext &ASTCtx,
    Parallelization &ParallelizationInfo) {
  if (I == EI)
    return;
  auto &LoopMatcher = Provider.value<LoopMatcherPass *>()->getMatcher();
  auto *ParentScope = getScope(*I, LoopMatcher, ASTCtx);
  assert(ParentScope && "Unable to find AST scope for a parallel loop!");
  SmallVector<Loop *, 4> ToMerge;
  for (auto *Child : ParentScope->children()) {
    if (!Child)
      continue;
    if (auto *For = dyn_cast<ForStmt>(Child)) {
      auto MatchItr = LoopMatcher.find<AST>(For);
      if (MatchItr != LoopMatcher.end())
        if (isParallel(MatchItr->template get<IR>(), ParallelizationInfo)) {
          ToMerge.push_back(MatchItr->template get<IR>());
          continue;
        }
    }
    if (ToMerge.size() > 1)
      mergeRegions(ToMerge, ParallelizationInfo);
    ToMerge.clear();
  }
  if (ToMerge.size() > 1)
    mergeRegions(ToMerge, ParallelizationInfo);
}
} // namespace

void ClangOpenMPParallelization::optimizeLevel(
    PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  // Insert ordered directives.
  for (auto &Ordered : mOutermostOrderedLoops) {
    auto *InnermostLoop = Ordered.get<Loop>();
    for (unsigned I = 1, EI = Ordered.get<OMPOrderedDirective>()->depth();
         I < EI; ++I)
      InnermostLoop = *InnermostLoop->begin();
    SmallVector<BasicBlock *, 1> Latches;
    InnermostLoop->getLoopLatches(Latches);
    for (auto *LatchBB : Latches) {
      auto SourceItr = mParallelizationInfo.try_emplace(LatchBB).first;
      SourceItr->get<ParallelLocation>().emplace_back();
      SourceItr->get<ParallelLocation>().back().Anchor =
          LatchBB->getTerminator();
    SourceItr->get<ParallelLocation>().back().Exit.push_back(
        std::make_unique<ParallelMarker<OMPOrderedDirective>>(
            0, (Ordered.get<OMPOrderedDirective>().get())));
    }
    BasicBlock *BodyEntryBB = nullptr;
    for (auto *Succ : successors(InnermostLoop->getHeader()))
      if (InnermostLoop->contains(Succ)) {
        BodyEntryBB = Succ;
        break;
      }
    assert(BodyEntryBB &&
           "Unable to place ordered directive inside an empty loop!");
    auto SinkInfo = mParallelizationInfo.try_emplace(BodyEntryBB);
    if (SinkInfo.second)
      SinkInfo.first->get<ParallelLocation>().emplace_back();
    SinkInfo.first->get<ParallelLocation>().back().Anchor =
      BodyEntryBB->getFirstNonPHIOrDbgOrLifetime();
    SinkInfo.first->get<ParallelLocation>().back().Entry.push_back(
      std::move(Ordered.get<OMPOrderedDirective>()));
  }
  mOutermostOrderedLoops.clear();
  // Merge neighboring parallel regions.
  auto *F{Level.is<Function *>()
              ? Level.get<Function *>()
              : Level.get<Loop *>()->getHeader()->getParent()};
  if (auto *DISub{findMetadata(F)})
    if (auto *CU{DISub->getUnit()}; CU && (isC(CU->getSourceLanguage()) ||
                                           isCXX(CU->getSourceLanguage()))) {
      auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
      auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                                 TfmInfo->getContext(*CU))
                           : nullptr};
      if (TfmCtx && TfmCtx->hasInstance()) {
        auto &ASTCtx{TfmCtx->getContext()};
        if (Level.is<Function *>()) {
          auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
          mergeSiblingRegions(LI.begin(), LI.end(), Provider, ASTCtx,
                              mParallelizationInfo);
        } else {
          mergeSiblingRegions(Level.get<Loop *>()->begin(),
                              Level.get<Loop *>()->end(), Provider, ASTCtx,
                              mParallelizationInfo);
        }
      }
    }
}

Optional<bool> ClangOpenMPParallelization::addOrUpdateOrderedIfNeed(
    const DFLoop &DFL, const ClangDependenceAnalyzer &ASTRegionAnalysis,
    OMPForDirective &OmpFor) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (ASTDepInfo.get<trait::Dependence>().empty())
    return false;
  auto OmpOrderedItr = find_if(OmpFor.children(), [](auto *Child) {
    return isa<OMPOrderedDirective>(Child);
    });
  BasicBlock *BodyEntryBB = nullptr;
  for (auto *Succ : successors(DFL.getLoop()->getHeader())) {
    if (!DFL.getLoop()->contains(Succ))
      continue;
    if (BodyEntryBB) {
      toDiag(ASTRegionAnalysis.getDiagnostics(),
             ASTRegionAnalysis.getRegion()->getBeginLoc(),
             tsar::diag::warn_parallel_loop);
      toDiag(ASTRegionAnalysis.getDiagnostics(),
             ASTRegionAnalysis.getRegion()->getBeginLoc(),
             tsar::diag::note_parallel_ordered_entry_unknown);
      return None;
    }
    BodyEntryBB = Succ;
  }
  if (!BodyEntryBB) {
    toDiag(ASTRegionAnalysis.getDiagnostics(),
           ASTRegionAnalysis.getRegion()->getBeginLoc(),
           tsar::diag::warn_parallel_loop);
    toDiag(ASTRegionAnalysis.getDiagnostics(),
           ASTRegionAnalysis.getRegion()->getBeginLoc(),
           tsar::diag::note_parallel_ordered_entry_unknown);
    return None;
  }
  if (OmpOrderedItr == OmpFor.child_end()) {
    auto OmpOrdered = std::make_unique<OMPOrderedDirective>(OmpFor);
    OmpOrderedItr = OmpFor.child_insert(OmpOrdered.get());
    mOutermostOrderedLoops.emplace_back(DFL.getLoop(), std::move(OmpOrdered));
  }
  cast<OMPOrderedDirective>(*OmpOrderedItr)
      ->push_back(makeOrderedRange(
          ASTDepInfo, OmpFor.getClauses().get<trait::Induction>().size()));
  return cast<OMPOrderedDirective>(*OmpOrderedItr)->depth() ==
         OmpFor.getClauses().get<trait::Induction>().size() + 1;
}

ParallelItem * ClangOpenMPParallelization::exploitParallelism(
    const DFLoop &DFL, const clang::ForStmt &For,
    const FunctionAnalysis &Provider,
    ClangDependenceAnalyzer &ASTRegionAnalysis, ParallelItem *PI) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (ASTDepInfo.get<trait::Induction>().size() != 1 ||
      !ASTDepInfo.get<trait::Induction>().begin()->first.get<AST>()) {
    if (PI)
      PI->finalize();
    return PI;
  }
  auto BaseInduct{ASTDepInfo.get<trait::Induction>().begin()->first};
  auto *M = DFL.getLoop()->getHeader()->getModule();
  auto LoopID = DFL.getLoop()->getLoopID();
  Optional<bool> Finalize;
  if (!PI) {
    auto OmpParallel = std::make_unique<OMPParallelDirective>();
    auto OmpFor = std::make_unique<OMPForDirective>(*OmpParallel.get());
    OmpParallel->child_insert(OmpFor.get());
    PI = OmpFor.get();
    OmpFor->getClauses().get<trait::Private>().insert(
        ASTDepInfo.get<trait::Private>().begin(),
        ASTDepInfo.get<trait::Private>().end());
    OmpFor->getClauses().get<trait::LastPrivate>().insert(
        ASTDepInfo.get<trait::LastPrivate>().begin(),
        ASTDepInfo.get<trait::LastPrivate>().end());
    OmpFor->getClauses().get<trait::FirstPrivate>().insert(
        ASTDepInfo.get<trait::FirstPrivate>().begin(),
        ASTDepInfo.get<trait::FirstPrivate>().end());
    for (unsigned I = 0, EI = ASTDepInfo.get<trait::Reduction>().size(); I < EI;
         ++I)
      OmpFor->getClauses().get<trait::Reduction>()[I].insert(
          ASTDepInfo.get<trait::Reduction>()[I].begin(),
          ASTDepInfo.get<trait::Reduction>()[I].end());
    if (!(Finalize = addOrUpdateOrderedIfNeed(DFL, ASTRegionAnalysis, *OmpFor)))
      return nullptr;
    auto EntryInfo =
        mParallelizationInfo.try_emplace(DFL.getLoop()->getHeader());
    assert(EntryInfo.second && "Unable to create a parallel block!");
    EntryInfo.first->get<ParallelLocation>().emplace_back();
    EntryInfo.first->get<ParallelLocation>().back().Anchor = LoopID;
    auto ExitingBB = getValidExitingBlock(*DFL.getLoop());
    assert(ExitingBB && "Parallel loop must have a single exit!");
    ParallelLocation *ExitLoc = nullptr;
    if (ExitingBB == DFL.getLoop()->getHeader()) {
      ExitLoc = &EntryInfo.first->get<ParallelLocation>().back();
    } else {
      auto ExitInfo = mParallelizationInfo.try_emplace(ExitingBB);
      assert(ExitInfo.second && "Unable to create a parallel block!");
      ExitInfo.first->get<ParallelLocation>().emplace_back();
      ExitLoc = &ExitInfo.first->get<ParallelLocation>().back();
      ExitLoc->Anchor = LoopID;
    }
    ExitLoc->Exit.push_back(
        std::make_unique<ParallelMarker<OMPParallelDirective>>(
            0, OmpParallel.get()));
    EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
        std::move(OmpParallel));
    EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
        std::move(OmpFor));
  } else {
    auto *OmpFor = cast<OMPForDirective>(PI);
    OmpFor->getClauses().get<trait::Private>().erase(BaseInduct);
    if (ASTDepInfo.get<trait::Private>() !=
            OmpFor->getClauses().get<trait::Private>() ||
        ASTDepInfo.get<trait::Reduction>() !=
            OmpFor->getClauses().get<trait::Reduction>() ||
        !(Finalize =
              addOrUpdateOrderedIfNeed(DFL, ASTRegionAnalysis, *OmpFor))) {
      OmpFor->getClauses().get<trait::Private>().insert(BaseInduct);
      PI->finalize();
      return PI;
    }
  }
  cast<OMPForDirective>(PI)->getClauses().get<trait::Induction>().emplace_back(
      LoopID);
  // TODO (kaniandr@gmail.com): fix me, induction variable may be last private.
  // TODO (kaniandr@gmail.com): If there is no regular data dependencies,
  // only outermost loop will be parallelized. Without prediction of
  // performance impact loop nest should not be collapsed.
  if (ASTDepInfo.get<trait::Dependence>().empty()) {
    PI->finalize();
  } else {
    auto &PerfectInfo =
        Provider.value<ClangPerfectLoopPass *>()->getPerfectLoopInfo();
    if (*Finalize || !PerfectInfo.count(&DFL) || DFL.getNumRegions() == 0)
      PI->finalize();
  }
  return PI;
}

static SourceLocation getLoopEnd(Stmt *S, const SourceManager &SrcMgr,
                                 const LangOptions &LangOpts) {
  Token Tok;
  return (!getRawTokenAfter(S->getEndLoc(), SrcMgr, LangOpts, Tok) &&
          Tok.is(clang::tok::semi))
             ? Tok.getLocation()
             : S->getEndLoc();
}

void buildOrederedSink(const trait::DIDependence::DistanceVector &SinkRange,
    unsigned Depth, SmallVectorImpl<std::pair<MDNode *, StringRef>> &Inductions,
    unsigned CurrDepth,
    SmallVectorImpl<APSInt> &SinkTemplate,  SmallVectorImpl<char> &PragmaStr) {
  assert(SinkTemplate.size() == Depth &&
         "Size of template and depth of ordered must be equal!");
  assert(Inductions.size() >= Depth &&
    "Induction variables are not known for some of the loops in the nest!");
  if (CurrDepth >= Depth) {
    StringRef Name{ " depend(sink" };
    PragmaStr.append(Name.begin(), Name.end());
    char Delimiter = ':';
    for (unsigned I = 0; I < Depth; ++I) {
      PragmaStr.append({ Delimiter });
      Delimiter = ',';
      PragmaStr.append(Inductions[I].second.begin(),
                       Inductions[I].second.end());
      if (SinkTemplate[I].isNullValue())
        continue;
      if (SinkTemplate[I].isNonNegative())
        PragmaStr.append({ '+' });
      SinkTemplate[I].toString(PragmaStr);
    }
    PragmaStr.append({ ')' });
    return;
  }
  if (!SinkRange[CurrDepth].first) {
    SinkTemplate[CurrDepth] = 0;
    buildOrederedSink(SinkRange, Depth, Inductions, CurrDepth + 1, SinkTemplate,
                      PragmaStr);
  } else {
    auto Dist = *SinkRange[CurrDepth].first;
    auto MaxDist = *SinkRange[CurrDepth].second;
    if (Dist > MaxDist)
      std::swap(Dist, MaxDist);
    for (; Dist <= MaxDist; ++Dist) {
      SinkTemplate[CurrDepth] = Dist;
      buildOrederedSink(SinkRange, Depth, Inductions, CurrDepth + 1,
                        SinkTemplate, PragmaStr);
    }
  }
}

bool ClangOpenMPParallelization::runOnModule(llvm::Module &M) {
  ClangSMParallelization::runOnModule(M);
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  for (auto F : make_range(mParallelizationInfo.func_begin(),
                           mParallelizationInfo.func_end())) {
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
    auto Provider = analyzeFunction(*F);
    auto &LI = Provider.value<LoopInfoWrapperPass*>()->getLoopInfo();
    auto &LM = Provider.value<LoopMatcherPass *>()->getMatcher();
    auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
    auto &RI = Provider.value<DFRegionInfoPass *>()->getRegionInfo();
    auto &MM = Provider.value<MemoryMatcherImmutableWrapper *>()->get();
    auto &ASTCtx = TfmCtx->getContext();
    struct InsertData {
      std::string Before;
      bool BeforeAfterToken = false;
      StringRef Delimiter = "";
      bool DelimiterAfterToken = false;
      std::string After;
      bool AfterAfterToken = false;
    };
    DenseMap<unsigned, InsertData> LoopToUpdate;
    for (auto &BB : *F) {
      auto ParallelItr = mParallelizationInfo.find(&BB);
      if (ParallelItr == mParallelizationInfo.end())
        continue;
      for (auto &PL : ParallelItr->get<ParallelLocation>()) {
        if (PL.Anchor.is<Value*>()) {
          auto Anchor = cast<Instruction>(PL.Anchor.get<Value *>());
          for (auto &PI : PL.Entry) {
            SmallString<128> PragmaStr{ "#pragma omp " };
            if (auto *OmpOrdered = dyn_cast<OMPOrderedDirective>(PI.get())) {
              auto *L = LI.getLoopFor(Anchor->getParent());
              assert(L && "Ordered directive must be placed inside a loop body!");
              auto LMatchItr = LM.find<IR>(L);
              assert(LMatchItr != LM.end() &&
                     "Unable to find AST representation for a loop!");
              auto For = cast<ForStmt>(LMatchItr->get<AST>());
              auto &ToBodyBegin =
                  *LoopToUpdate
                       .try_emplace(
                           For->getBody()->getBeginLoc().getRawEncoding())
                       .first;
              PragmaStr += omp::getOpenMPDirectiveName(
                  static_cast<omp::Directive>(OmpOrdered->getKind()));
              auto CurrDepth = OmpOrdered->depth();
              auto *OuterLoop = L;
              for (--CurrDepth; CurrDepth != 0; --CurrDepth)
                OuterLoop = OuterLoop->getParentLoop();
              SmallVector<std::pair<MDNode *, StringRef>, 3> Inductions;
              getBaseInductionsForNest(*OuterLoop, OmpOrdered->depth(), CL, RI,
                                       MM, Inductions);
              SmallVector<APSInt, 3> SinkTemplate(OmpOrdered->depth());
              for (auto Sink : *OmpOrdered)
                buildOrederedSink(Sink, OmpOrdered->depth(), Inductions, 0,
                                  SinkTemplate, PragmaStr);
              PragmaStr += "\n";
              if (!isa<CompoundStmt>(For->getBody())) {
                ToBodyBegin.second.Delimiter = "{\n";
                ToBodyBegin.second.DelimiterAfterToken = false;
              } else {
                ToBodyBegin.second.After += "\n";
                ToBodyBegin.second.AfterAfterToken = true;
              }
              ToBodyBegin.second.After += PragmaStr;
            } else {
              llvm_unreachable(
                  "An unknown pragma has been attached to an instruction!");
            }
          }
          for (auto &PI : PL.Exit) {
            if (auto *Marker =
                    dyn_cast<ParallelMarker<OMPOrderedDirective>>(PI.get())) {
              auto *OmpOrdered =
                  cast<OMPOrderedDirective>(Marker->parent_front());
              SmallString<128> PragmaStr{"#pragma omp "};
              PragmaStr += omp::getOpenMPDirectiveName(
                  static_cast<omp::Directive>(OmpOrdered->getKind()));
              PragmaStr += " depend(source)\n";
              auto *L = LI.getLoopFor(Anchor->getParent());
              assert(L && "Ordered directive must be placed inside a loop body!");
              auto LMatchItr = LM.find<IR>(L);
              assert(LMatchItr != LM.end() &&
                     "Unable to find AST representation for a loop!");
              auto &ToBodyEnd =
                  *LoopToUpdate
                       .try_emplace(getLoopEnd(LMatchItr->get<AST>(),
                                               ASTCtx.getSourceManager(),
                                               ASTCtx.getLangOpts())
                                        .getRawEncoding())
                       .first;
              ToBodyEnd.second.Before += PragmaStr;
              auto For = cast<ForStmt>(LMatchItr->get<AST>());
              if (!isa<CompoundStmt>(For->getBody())) {
                ToBodyEnd.second.BeforeAfterToken = true;
                ToBodyEnd.second.DelimiterAfterToken = true;
                ToBodyEnd.second.Delimiter = "}\n";
                auto &ToBodyBegin =
                    *LoopToUpdate
                         .try_emplace(
                             For->getBody()->getBeginLoc().getRawEncoding())
                         .first;
                ToBodyBegin.second.Delimiter = "{\n";
                ToBodyBegin.second.DelimiterAfterToken = false;
              }
            } else {
              llvm_unreachable(
                  "An unknown pragma has been attached to an instruction!");
            }
          }
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
        auto &ToInsertBefore =
            *LoopToUpdate
                 .try_emplace(
                     LMatchItr->get<AST>()->getBeginLoc().getRawEncoding())
                 .first;
        ToInsertBefore.second.AfterAfterToken = false;
        ToInsertBefore.second.DelimiterAfterToken = false;
        for (auto &PI : PL.Entry) {
          SmallString<128> PragmaStr{"#pragma omp "};
          if (auto *OmpParallel = dyn_cast<OMPParallelDirective>(PI.get())) {
            PragmaStr += omp::getOpenMPDirectiveName(
                static_cast<omp::Directive>(PI->getKind()));
            PragmaStr += " default(shared)";
            PragmaStr += "\n";
            ToInsertBefore.second.Before += PragmaStr;
            ToInsertBefore.second.Delimiter = "{\n";
          } else if (auto *OmpFor = dyn_cast<OMPForDirective>(PI.get())) {
            PragmaStr += omp::getOpenMPDirectiveName(
                static_cast<omp::Directive>(PI->getKind()));
            PragmaStr += " ";
            bcl::for_each(OmpFor->getClauses(), ClausePrinter{PragmaStr});
            auto OmpOrderedItr =
                llvm::find_if(OmpFor->children(), [](auto *Child) {
                  return isa<OMPOrderedDirective>(Child);
                });
            if (OmpOrderedItr != OmpFor->child_end()) {
              (" ordered(" +
               Twine(cast<OMPOrderedDirective>(**OmpOrderedItr).depth()) +
               ") schedule(static, 1)")
                  .toVector(PragmaStr);
            }
            PragmaStr += "\n";
            ToInsertBefore.second.After += PragmaStr;
          } else {
            llvm_unreachable("An unknown pragma has been attached to a loop!");
          }
        }
        if (PL.Exit.empty())
          continue;
        auto &ToInsertAfter =
            *LoopToUpdate
                 .try_emplace(getLoopEnd(LMatchItr->get<AST>(),
                                         ASTCtx.getSourceManager(),
                                         ASTCtx.getLangOpts())
                                  .getRawEncoding())
                 .first;
        ToInsertAfter.second.AfterAfterToken = true;
        for (auto &PI : PL.Exit) {
          SmallString<128> PragmaStr;
          if (auto *Marker =
                  dyn_cast<ParallelMarker<OMPParallelDirective>>(PI.get())) {
            PragmaStr = "}\n";
          } else {
            llvm_unreachable("An unknown pragma has been attached to a loop!");
          }
          PragmaStr += "\n";
          ToInsertAfter.second.After += PragmaStr;
        }
      }
    }
    for (auto &ToInsert : LoopToUpdate) {
      auto &Rewriter = TfmCtx->getRewriter();
      auto Loc = SourceLocation::getFromRawEncoding(ToInsert.first);
      if (!ToInsert.second.Before.empty())
        if (ToInsert.second.BeforeAfterToken)
          Rewriter.InsertTextAfterToken(Loc, ToInsert.second.Before);
        else
          Rewriter.InsertTextAfter(Loc, ToInsert.second.Before);
      if (!ToInsert.second.Delimiter.empty())
        if (ToInsert.second.DelimiterAfterToken)
          Rewriter.InsertTextAfterToken(Loc, ToInsert.second.Delimiter);
        else
          Rewriter.InsertTextAfter(Loc, ToInsert.second.Delimiter);
      if (!ToInsert.second.After.empty())
        if (ToInsert.second.AfterAfterToken)
          Rewriter.InsertTextAfterToken(Loc, ToInsert.second.After);
        else
          Rewriter.InsertTextAfter(Loc, ToInsert.second.After);
    }
  }
  return false;
}


ModulePass *llvm::createClangOpenMPParallelization() {
  return new ClangOpenMPParallelization;
}

char ClangOpenMPParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(ClangOpenMPParallelization,
                                  "clang-openmp-parallel",
                                  "OpenMP Based Parallelization (Clang)",
                                  ClangSMParallelizationInfo)
