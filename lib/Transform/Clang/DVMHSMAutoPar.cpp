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
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/Utils.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <clang/AST/ParentMapContext.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-sm-parallel"

namespace {
using DistanceInfo = ClangDependenceAnalyzer::DistanceInfo;
using VariableT = ClangDependenceAnalyzer::VariableT;
using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;

class PragmaRegion : public ParallelLevel {
public:
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
  using AcrossVarListT =
      std::map<VariableT, trait::DIDependence::DistanceVector,
               ClangDependenceAnalyzer::VariableLess>;
  using LoopNestT = SmallVector<ObjectID, 4>;
  using VarMappingT =
      DenseMap<ObjectID, SmallVector<std::pair<ObjectID, bool>, 4>>;

  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>,
                        bcl::tagged<AcrossVarListT, trait::Dependence>,
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

  unsigned getPossibleAcrossDepth() const noexcept {
    return mPossibleAcrossDepth;
  }

  void setPossibleAcrossDepth(unsigned Depth) noexcept {
    mPossibleAcrossDepth = Depth;
  }

private:
  ClauseList mClauses;
  unsigned mPossibleAcrossDepth = 0;
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
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    ParallelItem *PI) override;

  bool processRegularDependenceis(const DFLoop &DFL,
    const tsar::ClangDependenceAnalyzer &ASRegionAnalysis,
    const FunctionAnalysis &Provider, PragmaParallel &DVMHParallel);

  void optimizeLevel(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  Parallelization mParallelizationInfo;
};

bool ClangDVMHSMParallelization::processRegularDependenceis(const DFLoop &DFL,
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
  unsigned PossibleAcrossDepth =
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Flow>().empty() ?
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Anti>().size() :
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Flow>().size();
  auto updatePAD = [&PossibleAcrossDepth](auto &Dep, auto T) {
    if (!Dep.second.template get<decltype(T)>().empty()) {
      auto MinDepth = *Dep.second.template get<decltype(T)>().front().first;
      unsigned PAD = 1;
      for (auto InnerItr = Dep.second.template get<decltype(T)>().begin() + 1,
                InnerItrE = Dep.second.template get<decltype(T)>().end();
           InnerItr != InnerItrE; ++InnerItr, ++PAD)
        if (InnerItr->first->isNegative()) {
          auto Revert = -(*InnerItr->first);
          Revert.setIsUnsigned(true);
          if (Revert >= MinDepth)
            break;
        }
      PossibleAcrossDepth = std::min(PossibleAcrossDepth, PAD);
    }
  };
  for (auto &Dep : ASTDepInfo.get<trait::Dependence>()) {
    auto AccessItr =
        find_if(AccessInfo->scope_accesses(LoopID), [&Dep](auto &Access) {
            return Access.getArray() == Dep.first.get<MD>();
        });
    if (AccessItr == AccessInfo->scope_end(LoopID))
      return false;
    Optional<unsigned> DependentDim;
    unsigned NumberOfDims = 0;
    for (auto &Access :
         AccessInfo->array_accesses(AccessItr->getArray(), LoopID)) {
      NumberOfDims = std::max(NumberOfDims, Access.size());
      for (auto *Subscript : Access) {
        if (!Subscript || !isa<DIAffineSubscript>(Subscript))
          return false;
        auto Affine = cast<DIAffineSubscript>(Subscript);
        ObjectID AnotherColumn = nullptr;
        for (unsigned Idx = 0, IdxE = Affine->getNumberOfMonoms(); Idx < IdxE;
             ++Idx) {
          if (Affine->getMonom(Idx).Column == LoopID) {
            if (AnotherColumn ||
                DependentDim && *DependentDim != Affine->getDimension())
              return false;
            DependentDim = Affine->getDimension();
          } else {
            if (DependentDim && *DependentDim == Affine->getDimension())
              return false;
            AnotherColumn = Affine->getMonom(Idx).Column;
          }
        }
      }
    }
    if (!DependentDim)
      return false;
    updatePAD(Dep, trait::Flow{});
    updatePAD(Dep, trait::Anti{});
    auto I =
        DVMHParallel.getClauses()
            .get<trait::Dependence>()
            .emplace(std::piecewise_construct, std::forward_as_tuple(Dep.first),
                     std::forward_as_tuple())
            .first;
    I->second.resize(NumberOfDims);
    auto getDistance = [&Dep](auto T) {
      return Dep.second.get<decltype(T)>().empty()
                 ? None
                 : Dep.second.get<decltype(T)>().front().second;
    };
    if (ConstStep->getAPInt().isNegative())
      I->second[*DependentDim] = {getDistance(trait::Anti{}),
                                  getDistance(trait::Flow{})};

    else
      I->second[*DependentDim] = {getDistance(trait::Flow{}),
                                  getDistance(trait::Anti{})};
  }
  PossibleAcrossDepth += DVMHParallel.getClauses().get<trait::Induction>().size();
  if (DVMHParallel.getPossibleAcrossDepth() == 0)
    DVMHParallel.setPossibleAcrossDepth(PossibleAcrossDepth);
  else
    DVMHParallel.setPossibleAcrossDepth(
        std::min(DVMHParallel.getPossibleAcrossDepth(), PossibleAcrossDepth));
  return true;
}
} // namespace

ParallelItem *ClangDVMHSMParallelization::exploitParallelism(
    const DFLoop &IR, const clang::ForStmt &For,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis, ParallelItem *PI) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
      !ASTDepInfo.get<trait::LastPrivate>().empty() ||
      !ASTDepInfo.get<trait::Induction>().get<AST>()) {
    if (PI)
      PI->finalize();
    return PI;
  }
  if (PI) {
    auto DVMHParallel = cast<PragmaParallel>(PI);
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    DVMHParallel->getClauses().get<trait::Private>().erase(
        ASTDepInfo.get<trait::Induction>());
    if (PL[IR.getLoop()].isHostOnly() ||
        ASTDepInfo.get<trait::Private>() !=
            DVMHParallel->getClauses().get<trait::Private>() ||
        ASTDepInfo.get<trait::Reduction>() !=
            DVMHParallel->getClauses().get<trait::Reduction>()) {
      DVMHParallel->getClauses().get<trait::Private>().insert(
          ASTDepInfo.get<trait::Induction>());
      PI->finalize();
      return PI;
    }
    if (!processRegularDependenceis(IR, ASTRegionAnalysis, Provider,
                                    *DVMHParallel)) {
      PI->finalize();
      return PI;
    }
  } else {
    std::unique_ptr<PragmaActual> DVMHActual;
    std::unique_ptr<PragmaGetActual> DVMHGetActual;
    std::unique_ptr<PragmaRegion> DVMHRegion;
    auto Localized = ASTRegionAnalysis.evaluateDefUse();
    if (Localized) {
      DVMHRegion = std::make_unique<PragmaRegion>();
      DVMHRegion->finalize();
    }
    auto DVMHParallel = std::make_unique<PragmaParallel>(DVMHRegion.get());
    if (DVMHRegion)
      DVMHRegion->child_insert(DVMHParallel.get());
    PI = DVMHParallel.get();
    DVMHParallel->getClauses().get<trait::Private>().insert(
        ASTDepInfo.get<trait::Private>().begin(),
        ASTDepInfo.get<trait::Private>().end());
    for (unsigned I = 0, EI = ASTDepInfo.get<trait::Reduction>().size(); I < EI;
         ++I)
      DVMHParallel->getClauses().get<trait::Reduction>()[I].insert(
          ASTDepInfo.get<trait::Reduction>()[I].begin(),
          ASTDepInfo.get<trait::Reduction>()[I].end());
    if (!processRegularDependenceis(IR, ASTRegionAnalysis, Provider,
                                    *DVMHParallel))
      return nullptr;
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    if (!PL[IR.getLoop()].isHostOnly() && Localized) {
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
      IR.getLoop()->getLoopID());
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

static inline PragmaParallel *isParallel(const Loop *L,
                                         Parallelization &ParallelizationInfo) {
  if (auto ID = L->getLoopID())
    return ParallelizationInfo.find<PragmaParallel>(L->getHeader(), ID)
        .dyn_cast();
  return nullptr;
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
      ToMerge.back()->getExitingBlock(), ToMerge.back()->getLoopID(), false);
  auto &MergedActual = *[&PB = MergedRegion.getPL()->Entry]() {
    auto I = find_if(PB, [](auto &PI) { return isa<PragmaActual>(*PI); });
    if (I != PB.end())
      return cast<PragmaActual>(I->get());
    PB.push_back(std::make_unique<PragmaActual>());
    return cast<PragmaActual>(PB.back().get());
  }();
  auto &MergedGetActual = *[&PB = MergedMarker.getPL()->Exit]() {
    auto I = find_if(PB, [](auto &PI) { return isa<PragmaGetActual>(*PI); });
    if (I != PB.end())
      return cast<PragmaGetActual>(I->get());
    PB.push_back(std::make_unique<PragmaGetActual>());
    return cast<PragmaGetActual>(PB.back().get());
  }();
  MergedMarker.get()->setParent(MergedRegion.get());
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
  auto copyInOut = [&To = MergedRegion.get()->getClauses()](auto &FromRegion) {
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
    auto ExitingBB = L->getExitingBlock();
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
    auto HeaderBB = L->getExitingBlock();
    auto ID = L->getLoopID();
    auto Region = ParallelizationInfo.find<PragmaRegion>(L->getHeader(), ID);
    copyInOut(*Region.get());
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
  auto *ParentScope = getScope(*I, LoopMatcher, ASTCtx);
  assert(ParentScope && "Unable to find AST scope for a parallel loop!");
  SmallVector<Loop *, 4> ToMerge;
  bool IsHostOnly = false;
  for (auto *Child : ParentScope->children()) {
    if (!Child)
      continue;
    if (auto *For = dyn_cast<ForStmt>(Child)) {
      auto MatchItr = LoopMatcher.find<AST>(For);
      if (MatchItr != LoopMatcher.end())
        if (auto *DVMHParallel =
                isParallel(MatchItr->template get<IR>(), ParallelizationInfo))
          if (auto *DVMHRegion =
                  cast_or_null<PragmaRegion>(DVMHParallel->getParent())) {
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
      auto MappingItr =
          Clauses.template get<trait::DirectAccess>()
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
            auto Itr = find(Clauses.template get<trait::Induction>(),
                            Affine->getMonom(I).Column);
            if (Itr != Clauses.template get<trait::Induction>().end())
              MappingItr->second[Affine->getDimension()] = {
                  *Itr, !Affine->getMonom(I).Value.isNegative()};
          }
        }
      }
    }
  }
}

void ClangDVMHSMParallelization::optimizeLevel(
    PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (AccessInfo) {
    if (Level.is<Function *>()) {
      auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
      optimizeLevelImpl(LI.begin(), LI.end(), Provider, *AccessInfo,
                        mParallelizationInfo);
    } else {
      optimizeLevelImpl(Level.get<Loop *>()->begin(),
                        Level.get<Loop *>()->end(), Provider, *AccessInfo,
                        mParallelizationInfo);
    }
  }
  auto *M = Level.is<Function *>() ? Level.get<Function *>()->getParent() :
    Level.get<Loop *>()->getHeader()->getModule();
  auto &ASTCtx =
      getAnalysis<TransformationEnginePass>()->getContext(*M)->getContext();
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

static inline void addVarList(
    const std::set<std::string> &VarInfoList,
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

static inline void addVarList(const SortedVarListT &VarInfoList,
    SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto name = [](auto &V) { return V.get<AST>()->getName(); };
  auto I{map_iterator(VarInfoList.begin(), name)},
      EI{map_iterator(VarInfoList.end(), name)};
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
  getBaseInductionsForNest(L,
                           Parallel.getClauses().get<trait::Induction>().size(),
                           CL, RI, MemoryMatcher, Inductions);
  assert(Inductions.size() ==
             Parallel.getClauses().get<trait::Induction>().size() &&
    "Unable to find induction variable for some of the loops in a parallel nest!");
  PragmaStr.push_back('(');
  for (auto &LToI : Inductions) {
    PragmaStr.push_back('[');
    PragmaStr.append(LToI.second.begin(), LToI.second.end());
    PragmaStr.push_back(']');
  }
  PragmaStr.push_back(')');
  // We sort arrays to ensure the same order of variables after
  // different launches of parallelization.
  std::set<std::string, std::less<std::string>> MappingStr;
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

static inline void addClauseIfNeed(StringRef Name, SortedVarListT &Vars,
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
    auto VarName{ VarItr->get<AST>()->getName() };
    ParallelFor.append(VarName.begin(), VarName.end());
    ParallelFor.push_back(')');
    for (++VarItr; VarItr != VarItrE; ++VarItr) {
      ParallelFor.push_back(',');
      ParallelFor.append(RedKind.begin(), RedKind.end());
      ParallelFor.push_back('(');
      ParallelFor.append(VarName.begin(), VarName.end());
      ParallelFor.push_back(')');
    }
    ParallelFor.push_back(')');
  }
}

bool ClangDVMHSMParallelization::runOnModule(llvm::Module &M) {
  ClangSMParallelization::runOnModule(M);
  auto *TfmCtx = getAnalysis<TransformationEnginePass>()->getContext(M);
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
            if (!Parallel->getClauses().get<trait::Dependence>().empty()) {
              PragmaStr += "across(";
              for (auto &Across :
                Parallel->getClauses().get<trait::Dependence>()) {
                PragmaStr += Across.first.get<AST>()->getName();
                for (auto &Range : Across.second) {
                  PragmaStr += "[";
                  if (Range.first)
                    Range.first->toString(PragmaStr);
                  else
                    PragmaStr += "0";
                  PragmaStr += ":";
                  if (Range.second)
                    Range.second->toString(PragmaStr);
                  else
                    PragmaStr += "0";
                  PragmaStr += "]";
                }
              }
              PragmaStr += ")";
            }
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
