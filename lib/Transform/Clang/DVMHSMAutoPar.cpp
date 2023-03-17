//===-- DVMHSMAutoPar.cpp - DVMH Based Parallelization (Clang) ---*- C++ -*===//
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
#include "tsar/ADT/PersistentMap.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/ExpressionMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/Utils.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include "tsar/Transform/Clang/Passes.h"
#include <clang/AST/ParentMapContext.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>

using namespace clang;
using namespace llvm;
using namespace tsar;
using namespace tsar::dvmh;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-sm-parallel"

namespace {
class ClangDVMHSMParallelizationInfo final : public ClangSMParallelizationInfo {
  void addBeforePass(legacy::PassManager &Passes) const override {
    Passes.add(createDVMHParallelizationContext());
    ClangSMParallelizationInfo::addBeforePass(Passes);
  }
  void addAfterPass(legacy::PassManager &Passes) const override {
    Passes.add(createDVMHDataTransferIPOPass());
    Passes.add(createClangDVMHWriter());
    ClangSMParallelizationInfo::addAfterPass(Passes);
  }
};

/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangDVMHSMParallelization : public ClangSMParallelization {
public:
  static char ID;
  ClangDVMHSMParallelization() : ClangSMParallelization(ID) {
    initializeClangDVMHSMParallelizationPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    ClangSMParallelization::getAnalysisUsage(AU);
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<DVMHParallelizationContext>();
  }

private:
  ParallelItem * exploitParallelism(const DFLoop &IR, const clang::ForStmt &AST,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    ParallelItem *PI) override;

  bool processRegularDependencies(const DFLoop &DFL,
    const tsar::ClangDependenceAnalyzer &ASRegionAnalysis,
    const FunctionAnalysis &Provider, PragmaParallel &DVMHParallel);

  void optimizeLevel(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  void finalize(llvm::Module &M, bcl::marray<bool, 2> &Reachability) override;
};
} // namespace

bool ClangDVMHSMParallelization::processRegularDependencies(const DFLoop &DFL,
    const tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    const FunctionAnalysis &Provider, PragmaParallel &DVMHParallel) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (ASTDepInfo.get<trait::Dependence>().empty())
    return true;
  assert(DVMHParallel.getPossibleAcrossDepth() == 0 ||
         DVMHParallel.getClauses().get<trait::Induction>().size() <
             DVMHParallel.getPossibleAcrossDepth() &&
         "Maximum depth of a parallel nest has been exceeded!");
  auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
  auto CanonicalItr = CL.find_as(&DFL);
  auto ConstStep = dyn_cast_or_null<SCEVConstant>((*CanonicalItr)->getStep());
  if (!ConstStep) {
    toDiag(ASTRegionAnalysis.getDiagnostics(),
           ASTRegionAnalysis.getRegion()->getBeginLoc(),
           tsar::diag::warn_parallel_loop);
    toDiag(ASTRegionAnalysis.getDiagnostics(),
           ASTRegionAnalysis.getRegion()->getBeginLoc(),
           tsar::diag::note_parallel_across_direction_unknown);
    return false;
  }
  auto LoopID = DFL.getLoop()->getLoopID();
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (!AccessInfo)
    return false;
  unsigned PossibleAcrossDepth{dvmh::processRegularDependencies(
      LoopID, ConstStep, ASTDepInfo, *AccessInfo,
      DVMHParallel.getClauses().get<trait::Dependence>())};
  if (PossibleAcrossDepth == 0)
    return false;
  PossibleAcrossDepth +=
      DVMHParallel.getClauses().get<trait::Induction>().size();
  if (DVMHParallel.getPossibleAcrossDepth() == 0)
    DVMHParallel.setPossibleAcrossDepth(PossibleAcrossDepth);
  else
    DVMHParallel.setPossibleAcrossDepth(
        std::min(DVMHParallel.getPossibleAcrossDepth(), PossibleAcrossDepth));
  return true;
}

ParallelItem *ClangDVMHSMParallelization::exploitParallelism(
    const DFLoop &IR, const clang::ForStmt &For,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis, ParallelItem *PI) {
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
      !ASTDepInfo.get<trait::LastPrivate>().empty() ||
      ASTDepInfo.get<trait::Induction>().size() != 1 ||
      !ASTDepInfo.get<trait::Induction>().begin()->first.get<AST>()) {
    if (PI)
      PI->finalize();
    return PI;
  }
  auto BaseInduct{ASTDepInfo.get<trait::Induction>().begin()->first};
  if (PI) {
    auto DVMHParallel = cast<PragmaParallel>(PI);
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    DVMHParallel->getClauses().get<trait::Private>().erase(BaseInduct);
    if (PL[IR.getLoop()].isHostOnly() ||
        ASTDepInfo.get<trait::Private>() !=
            DVMHParallel->getClauses().get<trait::Private>() ||
        ASTDepInfo.get<trait::Reduction>() !=
            DVMHParallel->getClauses().get<trait::Reduction>()) {
      DVMHParallel->getClauses().get<trait::Private>().insert(BaseInduct);
      PI->finalize();
      return PI;
    }
    if (!processRegularDependencies(IR, ASTRegionAnalysis, Provider,
                                    *DVMHParallel)) {
      PI->finalize();
      return PI;
    }
  } else {
    std::unique_ptr<PragmaActual> DVMHActual;
    std::unique_ptr<PragmaGetActual> DVMHGetActual;
    std::unique_ptr<PragmaRegion> DVMHRegion;
    auto DVMHParallel{std::make_unique<PragmaParallel>()};
    auto Localized = ASTRegionAnalysis.evaluateDefUse();
    if (Localized) {
      DVMHRegion = std::make_unique<PragmaRegion>();
      DVMHRegion->finalize();
      DVMHRegion->child_insert(DVMHParallel.get());
      DVMHParallel->parent_insert(DVMHRegion.get());
      for (const auto &V : ASTDepInfo.get<trait::WriteOccurred>())
        DVMHParallel->getClauses().template get<trait::DirectAccess>().emplace(
            std::piecewise_construct, std::forward_as_tuple(V),
            std::forward_as_tuple());
      for (const auto &V : ASTDepInfo.get<trait::ReadOccurred>())
        DVMHParallel->getClauses().template get<trait::DirectAccess>().emplace(
            std::piecewise_construct, std::forward_as_tuple(V),
            std::forward_as_tuple());
    } else {
      auto *NotLocalized{ASTRegionAnalysis.hasDiagnostic(
          tsar::diag::note_parallel_localize_inout_unable)};
      assert(NotLocalized && "Source of a diagnostic must be available!");
      for (const auto &V : *NotLocalized)
        DVMHParallel->getClauses().template get<trait::DirectAccess>().emplace(
            std::piecewise_construct, std::forward_as_tuple(V),
            std::forward_as_tuple());
    }
    PI = DVMHParallel.get();
    DVMHParallel->getClauses().get<trait::Private>().insert(
        ASTDepInfo.get<trait::Private>().begin(),
        ASTDepInfo.get<trait::Private>().end());
    for (unsigned I = 0, EI = ASTDepInfo.get<trait::Reduction>().size(); I < EI;
         ++I)
      DVMHParallel->getClauses().get<trait::Reduction>()[I].insert(
          ASTDepInfo.get<trait::Reduction>()[I].begin(),
          ASTDepInfo.get<trait::Reduction>()[I].end());
    if (!processRegularDependencies(IR, ASTRegionAnalysis, Provider,
                                    *DVMHParallel))
      return nullptr;
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    if (!PL[IR.getLoop()].isHostOnly() && Localized) {
      if (!ASTDepInfo.get<trait::ReadOccurred>().empty()) {
        DVMHActual = std::make_unique<PragmaActual>(false);
        DVMHActual->getMemory()
            .insert(ASTDepInfo.get<trait::ReadOccurred>().begin(),
                    ASTDepInfo.get<trait::ReadOccurred>().end());
        DVMHRegion->getClauses().get<trait::ReadOccurred>().insert(
            ASTDepInfo.get<trait::ReadOccurred>().begin(),
            ASTDepInfo.get<trait::ReadOccurred>().end());
      }
      if (!ASTDepInfo.get<trait::WriteOccurred>().empty()) {
        DVMHGetActual = std::make_unique<PragmaGetActual>(false);
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
    } else if (Localized) {
      DVMHRegion->setHostOnly(true);
      // TODO (kaniandr@gmail.com): try to predict influence of OpenMP collapse
      // directives. Sometimes they may degrade performance, so we do not use
      // them now if there are no regular dependencies.
      if (ASTDepInfo.get<trait::Dependence>().empty())
        DVMHParallel->finalize();
    } else if (ASTDepInfo.get<trait::Dependence>().empty()) {
      // TODO (kaniandr@gmail.com): try to predict influence of OpenMP collapse
      // directives. Sometimes they may degrade performance, so we do not use
      // them now if there are no regular dependencies.
      DVMHParallel->finalize();
    }
    auto EntryInfo =
        ParallelCtx.getParallelization().try_emplace(IR.getLoop()->getHeader());
    assert(EntryInfo.second && "Unable to create a parallel block!");
    EntryInfo.first->get<ParallelLocation>().emplace_back();
    EntryInfo.first->get<ParallelLocation>().back().Anchor =
        IR.getLoop()->getLoopID();
    auto ExitingBB{getValidExitingBlock(*IR.getLoop())};
    assert(ExitingBB && "Parallel loop must have a single exit!");
    ParallelLocation *ExitLoc = nullptr;
    if (ExitingBB == IR.getLoop()->getHeader()) {
      ExitLoc = &EntryInfo.first->get<ParallelLocation>().back();
    } else {
      auto ExitInfo{ParallelCtx.getParallelization().try_emplace(ExitingBB)};
      assert(ExitInfo.second && "Unable to create a parallel block!");
      ExitInfo.first->get<ParallelLocation>().emplace_back();
      ExitLoc = &ExitInfo.first->get<ParallelLocation>().back();
      ExitLoc->Anchor = IR.getLoop()->getLoopID();
    }
    if (DVMHRegion)
      ExitLoc->Exit.push_back(
          std::make_unique<ParallelMarker<PragmaRegion>>(0, DVMHRegion.get()));
    if (DVMHActual)
      EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
          std::move(DVMHActual));
    if (DVMHRegion)
      EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
          std::move(DVMHRegion));
    EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
        std::move(DVMHParallel));
    if (DVMHGetActual)
      ExitLoc->Exit.push_back(std::move(DVMHGetActual));
  }
  cast<PragmaParallel>(PI)->getClauses().get<trait::Induction>().emplace_back(
      IR.getLoop()->getLoopID(), BaseInduct);
   auto &PerfectInfo =
      Provider.value<ClangPerfectLoopPass *>()->getPerfectLoopInfo();
  if (!PI->isFinal() &&
      (!PerfectInfo.count(&IR) || IR.getNumRegions() == 0 ||
       (cast<PragmaParallel>(PI)->getPossibleAcrossDepth() != 0 &&
        cast<PragmaParallel>(PI)->getClauses().get<trait::Induction>().size() ==
            cast<PragmaParallel>(PI)->getPossibleAcrossDepth())))
    PI->finalize();
  return PI;
}

static inline Stmt *getScope(Loop *L,
    const LoopMatcherPass::LoopMatcher &LoopMatcher, ASTContext &ASTCtx) {
  auto &ParentCtx = ASTCtx.getParentMapContext();
  auto Itr = LoopMatcher.find<IR>(L);
  if (Itr == LoopMatcher.end())
    return nullptr;
  return const_cast<Stmt *>(
      ParentCtx.getParents(*Itr->get<AST>()).begin()->get<Stmt>());
}

static void mergeRegions(const SmallVectorImpl<Loop *> &ToMerge,
    Parallelization &ParallelizationInfo) {
  assert(ToMerge.size() > 1 && "At least two regions must be specified!");
  auto MergedRegion = ParallelizationInfo.find<PragmaRegion>(
      ToMerge.front()->getHeader(), ToMerge.front()->getLoopID(), true);
  auto MergedMarker = ParallelizationInfo.find<ParallelMarker<PragmaRegion>>(
      getValidExitingBlock(*ToMerge.back()), ToMerge.back()->getLoopID(), false);
  auto &MergedActual = *[&PB = MergedRegion.getPL()->Entry]() {
    auto I = find_if(PB, [](auto &PI) { return isa<PragmaActual>(*PI); });
    if (I != PB.end())
      return cast<PragmaActual>(I->get());
    PB.push_back(std::make_unique<PragmaActual>(false));
    return cast<PragmaActual>(PB.back().get());
  }();
  auto &MergedGetActual = *[&PB = MergedMarker.getPL()->Exit]() {
    auto I = find_if(PB, [](auto &PI) { return isa<PragmaGetActual>(*PI); });
    if (I != PB.end())
      return cast<PragmaGetActual>(I->get());
    PB.push_back(std::make_unique<PragmaGetActual>(false));
    return cast<PragmaGetActual>(PB.back().get());
  }();
  cast<ParallelMarker<PragmaRegion>>(MergedMarker)
      ->parent_insert(MergedRegion.getUnchecked());
  auto copyActual = [](auto &PB, auto &To) {
    auto I = find_if(PB, [&To](auto &PI) {
      return isa<std::decay_t<decltype(To)>>(PI.get());
    });
    if (I != PB.end()) {
      auto &M = cast<std::decay_t<decltype(To)>>(**I).getMemory();
      To.getMemory().insert(M.begin(), M.end());
    }
    return I;
  };
  auto copyInOut = [&To = cast<PragmaRegion>(MergedRegion)->getClauses()](
                       auto &FromRegion) {
    auto &From = FromRegion.getClauses();
    To.get<trait::ReadOccurred>().insert(
        From.template get<trait::ReadOccurred>().begin(),
        From.template get<trait::ReadOccurred>().end());
    To.get<trait::WriteOccurred>().insert(
        From.template get<trait::WriteOccurred>().begin(),
        From.template get<trait::WriteOccurred>().end());
    To.get<trait::Private>().insert(From.template get<trait::Private>().begin(),
                                    From.template get<trait::Private>().end());
  };
  // Remove start or and of region and corresponding actualization directive.
  auto remove = [&ParallelizationInfo](BasicBlock *BB, auto RegionItr,
      auto ActualItr,  auto PEdgeItr, ParallelBlock &OppositePB,
      ParallelBlock &FromPB) {
    if (FromPB.size() == 1 || FromPB.size() == 2 && ActualItr != FromPB.end()) {
      if (OppositePB.empty() &&
          PEdgeItr->template get<ParallelLocation>().size() == 1)
        ParallelizationInfo.erase(BB);
      else
        FromPB.clear();
    } else if (ActualItr != FromPB.end()) {
      FromPB.erase(RegionItr);
      FromPB.erase(find_if(FromPB, [&ActualItr](auto &PI) {
        return PI->getKind() == (*ActualItr)->getKind();
      }));
    } else {
      FromPB.erase(RegionItr);
    }
  };
  auto removeEndOfRegion = [&copyActual, &remove, &MergedGetActual,
                            &ParallelizationInfo](Loop *L) {
    auto ExitingBB{getValidExitingBlock(*L)};
    auto ID = L->getLoopID();
    auto Marker = ParallelizationInfo.find<ParallelMarker<PragmaRegion>>(
        ExitingBB, ID, false);
    auto &ExitPB = Marker.getPL()->Exit;
    auto GetActualItr = copyActual(ExitPB, MergedGetActual);
    remove(ExitingBB, Marker.getPI(), GetActualItr, Marker.getPE(),
           Marker.getPL()->Entry, ExitPB);
  };
  auto removeStartOfRegion = [&copyActual, &copyInOut, &remove, &MergedActual,
                              &ParallelizationInfo](Loop *L) {
    auto HeaderBB = L->getHeader();
    auto ID = L->getLoopID();
    auto Region = ParallelizationInfo.find<PragmaRegion>(HeaderBB, ID);
    copyInOut(cast<PragmaRegion>(*Region));
    auto &EntryPB = Region.getPL()->Entry;
    auto ActualItr = copyActual(EntryPB, MergedActual);
    remove(HeaderBB, Region.getPI(), ActualItr, Region.getPE(),
           Region.getPL()->Exit, EntryPB);
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
static void mergeSiblingRegions(ItrT I, ItrT EI,
    const FunctionAnalysis &Provider, ASTContext &ASTCtx,
    Parallelization &ParallelizationInfo) {
  if (I == EI)
    return;
  auto &LoopMatcher = Provider.value<LoopMatcherPass *>()->getMatcher();
  // At least one loop must be parallel, so IR-to-AST match is always known for
  // this loop.
  auto *ParentScope{[&]() -> clang::Stmt * {
    for (auto Itr{I}; Itr != EI; ++Itr)
      if (auto *S{getScope(*Itr, LoopMatcher, ASTCtx)})
        return S;
    return nullptr;
  }()};
  assert(ParentScope && "Unable to find AST scope for a parallel loop!");
  SmallVector<Loop *, 4> ToMerge;
  bool IsHostOnly = false;
  for (auto *Child : ParentScope->children()) {
    if (!Child)
      continue;
    if (auto *For = dyn_cast<ForStmt>(Child)) {
      auto MatchItr = LoopMatcher.find<AST>(For);
      if (MatchItr != LoopMatcher.end())
        if (auto *DVMHParallel{
                isParallel(MatchItr->template get<IR>(), ParallelizationInfo)};
            DVMHParallel && !DVMHParallel->parent_empty()) {
          auto *DVMHRegion{cast<PragmaRegion>(DVMHParallel->parent_front())};
          if (!ToMerge.empty()) {
            if (DVMHRegion->isHostOnly() == IsHostOnly) {
              ToMerge.push_back(MatchItr->template get<IR>());
              continue;
            }
            if (ToMerge.size() > 1)
              mergeRegions(ToMerge, ParallelizationInfo);
          }
          ToMerge.push_back(MatchItr->template get<IR>());
          IsHostOnly = DVMHRegion->isHostOnly();
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

template <typename ItrT>
static void sanitizeAcrossLoops(ItrT I, ItrT EI,
    const FunctionAnalysis &Provider, const ASTContext &ASTCtx,
    Parallelization &ParallelizationInfo) {
  auto &LoopMatcher = Provider.value<LoopMatcherPass *>()->getMatcher();
  for (; I != EI; ++I) {
    auto *Parallel{isParallel(*I, ParallelizationInfo)};
    if (!Parallel)
      continue;
    SmallVector<const Decl *, 4> UntiedVars;
    auto &Clauses{Parallel->getClauses()};
    for (auto &Pair : Clauses.template get<trait::Dependence>()) {
      auto &&V{Pair.first};
      auto &&Distances{Pair.second};
      auto Range{Clauses.template get<trait::DirectAccess>().equal_range(V)};
      auto TieItr{find_if(Range.first, Range.second, [&V](const auto &Tie) {
        return Tie.first.template get<MD>() == V.template get<MD>();
      })};
      if (TieItr == Range.second) {
        UntiedVars.emplace_back(V.template get<AST>());
      } else if (TieItr != Range.second) {
        for (unsigned LoopIdx{0},
             LoopIdxE =
                 Distances.template get<trait::DIDependence::DistanceVector>()
                     .size();
             LoopIdx < LoopIdxE; ++LoopIdx) {
          auto [L, R] =
              Distances
                  .template get<trait::DIDependence::DistanceVector>()[LoopIdx];
          if ((L || R) && (LoopIdx >= TieItr->second.size() ||
                           !TieItr->second[LoopIdx].first)) {
            UntiedVars.emplace_back(V.template get<AST>());
            break;
          }
        }
      }
    }
    if (UntiedVars.empty())
      continue;
    toDiag(ASTCtx.getDiagnostics(),
           LoopMatcher.find<IR>(*I)->template get<AST>()->getBeginLoc(),
           tsar::diag::warn_parallel_loop);
    for (auto *D : UntiedVars)
      toDiag(ASTCtx.getDiagnostics(), D->getLocation(),
             tsar::diag::note_parallel_across_tie_unable);
    // Remote parallel loop, enclosing region and actualization directives.
    auto ID{(**I).getLoopID()};
    auto ExitingBB{getValidExitingBlock(**I)};
    auto Marker{ParallelizationInfo.find<ParallelMarker<PragmaRegion>>(
        ExitingBB, ID, false)};
    auto &ExitPB{Marker.getPL()->Exit};
    if (ExitPB.size() == 1 ||
        ExitPB.size() == 2 && all_of(ExitPB, [&Marker](auto &PI) {
          return *Marker.getPI() == PI ||
                 PI->getKind() ==
                     static_cast<unsigned>(DirectiveId::DvmGetActual);
        })) {
      if (Marker.getPL()->Entry.empty() &&
          Marker.getPE()->template get<ParallelLocation>().size() == 1)
        ParallelizationInfo.erase(ExitingBB);
      else
        ExitPB.clear();
    } else {
      ExitPB.erase(Marker.getPI());
      if (auto GetActualItr{find_if(ExitPB,
                                    [](auto &PI) {
                                      return PI->getKind() ==
                                             static_cast<unsigned>(
                                                 DirectiveId::DvmGetActual);
                                    })};
          GetActualItr != ExitPB.end())
        ExitPB.erase(GetActualItr);
    }
    auto HeaderBB{(**I).getHeader()};
    auto Region{ParallelizationInfo.find<PragmaRegion>(HeaderBB, ID)};
    auto &EntryPB{Region.getPL()->Entry};
    if (EntryPB.size() == 2 ||
        EntryPB.size() == 3 && all_of(EntryPB, [&Region, Parallel](auto &PI) {
          return *Region.getPI() == PI || Parallel == PI.get() ||
                 PI->getKind() == static_cast<unsigned>(DirectiveId::DvmActual);
        })) {
      if (Region.getPL()->Exit.empty() &&
          Region.getPE()->template get<ParallelLocation>().size() == 1)
        ParallelizationInfo.erase(HeaderBB);
      else
        EntryPB.clear();
    } else {
      EntryPB.erase(Region.getPI());
      EntryPB.erase(find_if(
          EntryPB, [Parallel](auto &PI) { return PI.get() == Parallel; }));
      if (auto ActualItr{find_if(EntryPB,
                                 [](auto &PI) {
                                   return PI->getKind() ==
                                          static_cast<unsigned>(
                                              DirectiveId::DvmActual);
                                 })};
          ActualItr != EntryPB.end())
        EntryPB.erase(ActualItr);
    }
  }
}

template <typename ItrT>
static void optimizeLevelImpl(ItrT I, ItrT EI, const FunctionAnalysis &Provider,
    const DIArrayAccessInfo &AccessInfo, Parallelization &ParallelizationInfo) {
  for (; I != EI; ++I) {
    auto *DVMHParallel = isParallel(*I, ParallelizationInfo);
    if (!DVMHParallel)
      continue;
    auto ID = (*I)->getLoopID();
    assert(ID && "Identifier must be known for a parallel loop!");
    auto &Clauses = DVMHParallel->getClauses();
    for (auto &Access : AccessInfo.scope_accesses(ID)) {
      if (!isa<DIEstimateMemory>(Access.getArray()))
        continue;
      auto MappingItr{find_if(
          Clauses.template get<trait::DirectAccess>(), [&Access](auto V) {
            return V.first.template get<MD>() == Access.getArray();
          })};
      if (MappingItr == Clauses.template get<trait::DirectAccess>().end())
        continue;
      if (is_contained(Clauses.template get<trait::Private>(),
                       MappingItr->first))
        continue;
      MappingItr->second.assign(Access.size(),
                                std::pair<ObjectID, bool>(nullptr, true));
      for (auto *Subscript : Access) {
        if (!Subscript || MappingItr->second[Subscript->getDimension()].first)
          continue;
        if (auto *Affine = dyn_cast<DIAffineSubscript>(Subscript)) {
          for (unsigned I = 0, EI = Affine->getNumberOfMonoms(); I < EI; ++I) {
            auto &Monom{Affine->getMonom(I)};
            if (Monom.Value.Kind == DIAffineSubscript::Symbol::SK_Constant &&
                Monom.Value.Constant.isNullValue())
              continue;
            auto Itr = find_if(
                Clauses.template get<trait::Induction>(),
                [Column = Monom.Column](auto &Level) {
                  return Level.template get<Loop>() == Column;
                });
            if (Itr != Clauses.template get<trait::Induction>().end())
              MappingItr->second[Affine->getDimension()] = {
                  Itr->template get<Loop>(),
                  Monom.Value.Kind == DIAffineSubscript::Symbol::SK_Constant &&
                      !Monom.Value.Constant.isNegative()};
          }
        }
      }
    }
  }
}

void ClangDVMHSMParallelization::optimizeLevel(
    PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (AccessInfo) {
    if (Level.is<Function *>()) {
      auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
      optimizeLevelImpl(LI.begin(), LI.end(), Provider, *AccessInfo,
                        ParallelCtx.getParallelization());
    } else {
      optimizeLevelImpl(Level.get<Loop *>()->begin(),
                        Level.get<Loop *>()->end(), Provider, *AccessInfo,
                        ParallelCtx.getParallelization());
    }
  }
  auto *F{Level.is<Function *>()
              ? Level.get<Function *>()
              : Level.get<Loop *>()->getHeader()->getParent()};
  auto *DISub{findMetadata(F)};
  if (!DISub)
    return;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance())
    return;
  auto &ASTCtx{TfmCtx->getContext()};
  if (Level.is<Function *>()) {
    auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
    sanitizeAcrossLoops(LI.begin(), LI.end(), Provider, ASTCtx,
                        ParallelCtx.getParallelization());
    mergeSiblingRegions(LI.begin(), LI.end(), Provider, ASTCtx,
                        ParallelCtx.getParallelization());
  } else {
    sanitizeAcrossLoops(Level.get<Loop *>()->begin(),
                        Level.get<Loop *>()->end(), Provider, ASTCtx,
                        ParallelCtx.getParallelization());
    mergeSiblingRegions(Level.get<Loop *>()->begin(),
                        Level.get<Loop *>()->end(), Provider, ASTCtx,
                        ParallelCtx.getParallelization());
  }
}

void ClangDVMHSMParallelization::finalize(llvm::Module &M,
                                          bcl::marray<bool, 2> &Reachability) {
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  for (auto &&[F, Node] : functions()) {
    if (isParallelCallee(*F, Node.get<Id>(), Reachability))
      ParallelCtx.markAsParallelCallee(*F);
  }
}

ModulePass *llvm::createClangDVMHSMParallelization() {
  return new ClangDVMHSMParallelization;
}

char ClangDVMHSMParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(
    ClangDVMHSMParallelization, "clang-dvmh-sm-parallel",
    "Shared Memory DVMH-based Parallelization (Clang)",
    ClangDVMHSMParallelizationInfo)
