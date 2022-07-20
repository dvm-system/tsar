//===--- PrivateAnalysis.cpp - Private Variable Analyzer --------*- C++ -*-===//
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
// This file implements passes to analyze variables which can be privatized.
//
//===----------------------------------------------------------------------===//

#include "BitMemoryTrait.h"
#include "tsar/Analysis/Memory/PrivateAnalysis.h"
#include "tsar/ADT/GraphUtils.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DependenceAnalysis.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/MemoryCoverage.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <bcl/utility.h>

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private"

MEMORY_TRAIT_STATISTIC(NumTraits)

char PrivateRecognitionPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(PrivateRecognitionPass, "private",
  "Private Variable Analysis", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(LiveMemoryPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(PrivateRecognitionPass, "private",
  "Private Variable Analysis", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

namespace tsar {
namespace detail {
/// Internal representation of cache which stores dependence analysis results.
struct DependenceCache {
  using SrcDstPair = std::pair<Instruction *, Instruction *>;
  using DependenceConfusedPair =
    std::pair<std::unique_ptr<Dependence>, unsigned short>;
  using CacheT = DenseMap<SrcDstPair, DependenceConfusedPair>;
  CacheT Impl;
};
}
}

bool PrivateRecognitionPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  if (!GlobalOpts.AnalyzeLibFunc && hasFnAttr(F, AttrKind::LibFunc))
    return false;
#ifdef LLVM_DEBUG
  for (const BasicBlock &BB : F)
    assert((&F.getEntryBlock() == &BB || BB.getNumUses() > 0 )&&
      "Data-flow graph must not contain unreachable nodes!");
#endif
  LoopInfo &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DFRegionInfo &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  mDefInfo = &getAnalysis<DefinedMemoryPass>().getDefInfo();
  mLiveInfo = &getAnalysis<LiveMemoryPass>().getLiveInfo();
  mAliasTree = &getAnalysis<EstimateMemoryPass>().getAliasTree();
  mDepInfo = &getAnalysis<DependenceAnalysisWrapperPass>().getDI();
  mDL = &F.getParent()->getDataLayout();
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  GraphNumbering<const AliasNode *> Numbers;
  numberGraph(mAliasTree, &Numbers);
  AliasTreeRelation AliasSTR(mAliasTree);
  DependenceCache Cache;
  resolveCandidats(Numbers, AliasSTR, DFF, Cache);
  return false;
}

namespace {
struct DistanceInfo {
  enum Apply : uint8_t {
    NotChange = 0u,
    // This is useful for anti and flow dependencies because direction is
    // known and sign of distance for a current loop (level) can be
    // always converted to a positive value.
    // [-2, 3, -1] => [2, -3, 1]
    Revert = 1u << 0,
    // This is useful if direction is not known. This implies conservative
    // assumption which allows to separate distances for different dimensions
    // and aggregate min/max distances for the accessed memory (instead of
    // each separate access):
    // [X, Y] => [abs(X), abs(Y)] [abs(X), -abs(Y)]
    //
    // (1) A(f1, j) = ...
    // (2) A(f2, j - 2) = ...
    // If f1 - f2 < 0 then aggregated distances for variable A
    // (for all accesses) are [abs(f1 - f2], -2]. This means that if we access
    // variable A on iteration (i, j) the next access will be on
    // (i + abs(f1 - f2), j - 2).
    // Otherwise (f1 - f2 > 0) then aggregated distances are [f1 - f2, 2].
    // This means that the next access will be on
    // (i + f1 - f2 = i + abs(f1 - f2), j + 2).
    // If we do not known sign of f1 - f2, then we assume that access in
    // j-loop occurs in the range between -2 and 2. And it allows us to
    // compute min/max distances for all loops in the nest.
    Extend = 1u << 1,
    LLVM_MARK_AS_BITMASK_ENUM(Extend)
  };

  DistanceInfo() = default;
  DistanceInfo(const Dependence &D, unsigned L, Apply T, ScalarEvolution &SE)
      : Dep(&D), Level(L), Transform(T), SE(&SE) {
    assert(Level <= Dep->getLevels() && "Level out of range!");
  }

  const Dependence *Dep = nullptr;
  unsigned Level = 0;
  Apply Transform = NotChange;
  ScalarEvolution *SE = nullptr;
};
}

namespace tsar {
namespace detail {
/// Internal representation of loop-carried dependencies.
class DependenceImp {
  friend struct UpdateFunctor;
  friend struct DumpFunctor;
  friend struct SummarizeFunctor;

public:
  using Distances = SmallPtrSet<const SCEV *, 4>;
  using DistanceNest = SmallVector<Distances, 3>;
  using Descriptor =
    bcl::TraitDescriptor<trait::Flow, trait::Anti, trait::Output>;

  /// \brief This functor summarize information about dependencies and stores
  /// summary in set of traits mSet.
  ///
  /// The one of the actions to be performed in calculation of maximum and
  /// minimum distances.
  template<class TraitSet>
  struct SummarizeFunctor {
    template<class Trait> void operator()() {
      SmallVector<Value *, 4> Causes{mDep->mCauses.begin(),
                                     mDep->mCauses.end()};
      if (!(mDep->mFlags.get<Trait>() & trait::Dependence::UnknownDistance)) {
        trait::IRDependence::DistanceVector Distances (
            mDep->mDists.get<Trait>().size());
        auto LevelItr = mDep->mDists.get<Trait>().begin();
        auto LevelItrE = mDep->mDists.get<Trait>().end();
        unsigned Idx = 0;
        SmallVector<const SCEV *, 4> MaxOps(LevelItr->begin(), LevelItr->end());
        auto MinOps(MaxOps); //Computation of max/min may update its parameter.
        Distances[Idx].first = mSE->getUMinExpr(MinOps);
        Distances[Idx].second = mSE->getUMaxExpr(MaxOps);
        for (++Idx, ++LevelItr; LevelItr != LevelItrE; ++LevelItr, ++Idx) {
          SmallVector<const SCEV *, 4> MaxOps(LevelItr->begin(),
                                              LevelItr->end());
          auto MinOps(MaxOps);
          Distances[Idx].first = mSE->getSMinExpr(MinOps);
          Distances[Idx].second = mSE->getSMaxExpr(MaxOps);
        }
        mSet->template set<Trait>(new trait::IRDependence(
            mDep->mFlags.get<Trait>(), Distances, Causes));
      } else {
        mSet->template set<Trait>(
            new trait::IRDependence(mDep->mFlags.get<Trait>(), Causes));
      }
    }
    DependenceImp *mDep;
    TraitSet *mSet;
    ScalarEvolution *mSE;
  };

  /// Returns descriptor.
  const Descriptor & get() const noexcept { return mDptr; }

  /// Uses specified descriptor, flags, and distance to update
  /// information about dependencies (see UpdateFunctor for details).
  void update(Descriptor Dptr, trait::Dependence::Flag F, DistanceInfo &&Dist,
              ArrayRef<Value *> Causes) {
    Dptr.for_each(UpdateFunctor{ this, F, std::move(Dist), Causes});
  }

  /// Uses specified dependence description to update underlying
  /// information about dependencies (see UpdateDefFunctor for details).
  void update(DependenceImp &Dep) {
    Dep.mDptr.for_each(UpdateDepFunctor{ this, &Dep });
  }

  /// Print information about dependencies.
  void print(raw_ostream &OS) { mDptr.for_each(DumpFunctor{ this, OS }); }

  /// Print information about dependencies.
  LLVM_DUMP_METHOD void dump() { print(dbgs()); dbgs() << "\n"; }

private:
  /// This functor updates specified dependence description mDep.
  struct UpdateFunctor {
    template<class Trait> void operator()() {
      mDep->mCauses.insert(mCauses.begin(), mCauses.end());
      mDep->mDptr.set<Trait>();
      mDep->mFlags.get<Trait>() |= mFlag;
      if (!mDist.Dep ||
          mDep->mFlags.get<Trait>() & trait::Dependence::UnknownDistance) {
        mDep->mFlags.get<Trait>() |= trait::Dependence::UnknownDistance;
        mDep->mKnownDistanceLevel.get<Trait>() = 0;
        mDep->mDists.get<Trait>().clear();
        return;
      }
      unsigned DistLevel = mDist.Dep->getLevels() + 1;
      if (mDep->mKnownDistanceLevel.get<Trait>() &&
          DistLevel > mDist.Level + *mDep->mKnownDistanceLevel.get<Trait>())
        DistLevel = mDist.Level + *mDep->mKnownDistanceLevel.get<Trait>();
      for (unsigned I = mDist.Level; I < DistLevel; ++I) {
        auto *Dist = mDist.Dep->getDistance(I);
        if (!Dist) {
          mDep->mKnownDistanceLevel.get<Trait>() = I - mDist.Level;
          mDep->mDists.get<Trait>().resize(
              *mDep->mKnownDistanceLevel.get<Trait>());
          if (*mDep->mKnownDistanceLevel.get<Trait>() == 0)
            mDep->mFlags.get<Trait>() |= trait::Dependence::UnknownDistance;
          break;
        }
        if (I - mDist.Level == mDep->mDists.get<Trait>().size())
          mDep->mDists.get<Trait>().emplace_back();
        if (mDist.Transform == DistanceInfo::NotChange)
          mDep->mDists.get<Trait>()[I - mDist.Level].insert(Dist);
        else if (mDist.Transform & DistanceInfo::Revert)
          mDep->mDists.get<Trait>()[I - mDist.Level].insert(
              mDist.SE->getNegativeSCEV(Dist));
        else if (mDist.Transform & DistanceInfo::Extend)
          if (I == mDist.Level) {
            mDep->mDists.get<Trait>()[I - mDist.Level].insert(
              mDist.SE->getSMaxExpr(mDist.SE->getNegativeSCEV(Dist), Dist));
          } else {
            mDep->mDists.get<Trait>()[I - mDist.Level].insert(Dist);
            mDep->mDists.get<Trait>()[I - mDist.Level].insert(
                mDist.SE->getNegativeSCEV(Dist));
          }
      }
    }
    DependenceImp *mDep;
    trait::Dependence::Flag mFlag;
    DistanceInfo mDist;
    ArrayRef<Value *> mCauses;
  };

  /// This functor updates specified dependence description mDep.
  struct UpdateDepFunctor {
    template<class Trait> void operator()() {
      mDep->mCauses.insert(mSrc->mCauses.begin(), mSrc->mCauses.end());
      mDep->mDptr.set<Trait>();
      mDep->mFlags.get<Trait>() |= mSrc->mFlags.get<Trait>();
      if (!(mDep->mFlags.get<Trait>() & trait::Dependence::UnknownDistance)) {
        if (auto SrcLevel = mSrc->mKnownDistanceLevel.get<Trait>())
          if (auto DepLevel = mDep->mKnownDistanceLevel.get<Trait>())
            mDep->mKnownDistanceLevel.get<Trait>() =
                std::min(*SrcLevel, *DepLevel);
          else
            mDep->mKnownDistanceLevel.get<Trait>() = SrcLevel;
        unsigned MergedLevel =
            mDep->mKnownDistanceLevel.get<Trait>()
                ? std::min<unsigned>(*mDep->mKnownDistanceLevel.get<Trait>(),
                                     mSrc->mDists.get<Trait>().size())
                : mSrc->mDists.get<Trait>().size();
        if (mDep->mDists.get<Trait>().size() < MergedLevel)
          mDep->mDists.get<Trait>().resize(MergedLevel);
        for (unsigned I = 0; I < MergedLevel; ++I)
          mDep->mDists.get<Trait>()[I].insert(
              mSrc->mDists.get<Trait>()[I].begin(),
              mSrc->mDists.get<Trait>()[I].end());
      } else {
        mDep->mKnownDistanceLevel.get<Trait>() = 0;
        mDep->mDists.get<Trait>().clear();
      }
    }
    DependenceImp *mDep;
    DependenceImp *mSrc;
  };

  /// Print information about dependencies.
  struct DumpFunctor {
    template<class Trait> void operator()() {
      mOS << "{" << Trait::toString();
      mOS << ", flags=";
      bcl::bitPrint(mDep->mFlags.get<Trait>(), mOS);
      mOS << ", distance={";
      unsigned LevelIdx = 0;
      for (const auto &Level : mDep->mDists.get<Trait>()) {
        for (const auto *D : Level) {
          mOS << " ";
          D->print(mOS);
        }
        ++LevelIdx;
        if (LevelIdx < mDep->mDists.get<Trait>().size())
          mOS << " |";
      }
      mOS << "}";
      if (mDep->mKnownDistanceLevel.get<Trait>())
        mOS << ", has dropped distances";
      if (!mDep->mCauses.empty())
        mOS << ", has " << mDep->mCauses.size() << " known causes";
      mOS << "}";
    }
    DependenceImp *mDep;
    raw_ostream &mOS;
  };

  Descriptor mDptr;
  bcl::tagged_tuple<
    bcl::tagged<DistanceNest, trait::Flow>,
    bcl::tagged<DistanceNest, trait::Anti>,
    bcl::tagged<DistanceNest, trait::Output>> mDists;
  bcl::tagged_tuple<
    bcl::tagged<trait::Dependence::Flag, trait::Flow>,
    bcl::tagged<trait::Dependence::Flag, trait::Anti>,
    bcl::tagged<trait::Dependence::Flag, trait::Output>> mFlags;
  bcl::tagged_tuple<
    bcl::tagged<Optional<unsigned>, trait::Flow>,
    bcl::tagged<Optional<unsigned>, trait::Anti>,
    bcl::tagged<Optional<unsigned>, trait::Output>> mKnownDistanceLevel;
  SmallPtrSet<Value *, 8> mCauses;
};
}
}

#ifdef LLVM_DEBUG
static void updateTraitsLog(const EstimateMemory *EM, BitMemoryTrait T) {
  llvm::dbgs() << "[PRIVATE]: update traits of ";
  printLocationSource(llvm::dbgs(),
    llvm::MemoryLocation(EM->front(), EM->getSize(), EM->getAAInfo()));
  llvm::dbgs() << " to ";
  bcl::bitPrint(T, llvm::dbgs());
  llvm::dbgs() << "\n";
}

static void updateDependenceLog(const EstimateMemory &EM, DependenceImp &Dep) {
  dbgs() << "[PRIVATE]: update dependence kind of ";
  printLocationSource(
    dbgs(), MemoryLocation(EM.front(), EM.getSize(), EM.getAAInfo()));
  dbgs() << " to ";
  Dep.print(dbgs());
  dbgs() << "\n";
}

template<class TraitList>
static void removeRedundantLog(TraitList &TL, StringRef Prefix) {
  dbgs() << "[PRIVATE]: " << Prefix << " remove redundant: ";
  for (auto CurrItr = TL.begin(); CurrItr != TL.end(); ++CurrItr) {
    printLocationSource(dbgs(),
      MemoryLocation(
        CurrItr->template get<EstimateMemory>()->front(),
        CurrItr->template get<EstimateMemory>()->getSize(),
        CurrItr->template get<EstimateMemory>()->getAAInfo()));
    dbgs() << " ";
  }
  dbgs() << "\n";
}
#endif


/// Inserts or updates information about dependencies in a specified map.
template<class MapTy>
static inline void updateDependence(const EstimateMemory *EM,
    DependenceImp::Descriptor &Dptr, trait::Dependence::Flag F,
    DistanceInfo &&Dist, MapTy &Deps,
    ArrayRef<Value *> Causes = ArrayRef<Value *>()) {
  assert(EM && "Estimate memory location must not be null!");
  auto Itr = Deps.try_emplace(EM, nullptr).first;
  if (!Itr->template get<DependenceImp>())
    Itr->template get<DependenceImp>().reset(new DependenceImp);
  Itr->template get<DependenceImp>()->update(Dptr, F, std::move(Dist), Causes);
  LLVM_DEBUG(updateDependenceLog(*EM, *Itr->template get<DependenceImp>()));
}

/// Merges descriptions of loop-carried dependencies and stores result in
/// a specified map.
///
/// Description of dependence carried by `To` location will be updated. If it
/// does not exist than it will be created. Privitizable variables are also
/// treated as loop-carried dependencies.
/// If `ToTrait` is `Dependency` or `From` is located in `Deps` than record for
/// `EM` will be inserted into `Deps` even if it did not exist before.
template<class MapTy>
static inline void mergeDependence(const EstimateMemory *To,
    BitMemoryTrait::Id ToTrait,
    const EstimateMemory *From, MapTy &Deps) {
  assert(To && "Estimate memory must not be null!");
  assert(From && "Estimate memory must not be null!");
  auto FromItr = Deps.find(From);
  DependenceImp *FromDep = nullptr;
  if (FromItr != Deps.end()) {
    FromDep = FromItr->template get<DependenceImp>().get();
    assert(FromDep &&
      "Location is stored in dependence map without dependence description!");
  } else if (dropUnitFlag(ToTrait) != BitMemoryTrait::Dependency) {
    return;
  }
  auto ToItr = Deps.try_emplace(To, nullptr).first;
  if (!ToItr->template get<DependenceImp>())
    ToItr->template get<DependenceImp>().reset(new DependenceImp);
  if (FromDep)
    ToItr->template get<DependenceImp>()->update(*FromDep);
  LLVM_DEBUG(updateDependenceLog(*To, *ToItr->template get<DependenceImp>()));
}

static inline MemoryLocation getLoadOrStoreLocation(Instruction *I) {
  if (const LoadInst *LI = dyn_cast<LoadInst>(I)) {
    if (LI->isUnordered())
      return MemoryLocation::get(LI);
  } else if (const StoreInst *SI = dyn_cast<StoreInst>(I)) {
    if (SI->isUnordered())
      return MemoryLocation::get(SI);
  }
  return MemoryLocation();
}

void PrivateRecognitionPass::collectHeaderAccesses(
     Loop *L, const DefUseSet &DefUse,
     TraitMap &ExplicitAccesses, UnknownMap &ExplicitUnknowns) {
  assert(L && "Loop must not be null!");
  for (auto &I : *L->getHeader()) {
    if (!I.mayReadOrWriteMemory())
      continue;
    for_each_memory(I, *mTLI,
      [this, &ExplicitAccesses, &DefUse](Instruction &I, MemoryLocation &&Loc,
          unsigned, AccessInfo R, AccessInfo W) {
        if (R == AccessInfo::No &&  W == AccessInfo::No)
          return;
        auto *EM = mAliasTree->find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        auto EMTraitItr = ExplicitAccesses.find(EM);
        while (EMTraitItr == ExplicitAccesses.end()) {
          using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
          EM = CT::getNext(EM);
          assert(EM && "It seems that traits does not exist!");
          EMTraitItr = ExplicitAccesses.find(EM);
        }
        *EMTraitItr->get<BitMemoryTrait>() &= BitMemoryTrait::HeaderAccess;
      },
      [this, &ExplicitUnknowns](Instruction &I, AccessInfo, AccessInfo) {
        auto Itr = ExplicitUnknowns.find(&I);
        assert(Itr != ExplicitUnknowns.end() &&
          "Explicitly accessed memory must be stored in a list of explicit accesses!");
        *Itr->get<BitMemoryTrait>() &= BitMemoryTrait::HeaderAccess;
      });
  }
}

void PrivateRecognitionPass::resolveCandidats(
    const GraphNumbering<const AliasNode *> &Numbers,
    const AliasTreeRelation &AliasSTR, DFRegion *R, DependenceCache &Cache) {
  assert(R && "Region must not be null!");
  if (auto *L = dyn_cast<DFLoop>(R)) {
    LLVM_DEBUG(dbgs() << "[PRIVATE]: analyze loop ";
      L->getLoop()->print(dbgs());
      if (DebugLoc DbgLoc = L->getLoop()->getStartLoc()) {
        dbgs() << " at ";
        DbgLoc.print(dbgs());
      }
      dbgs() << "\n";
    );
    auto PrivInfo = mPrivates.try_emplace(L);
    auto DefItr = mDefInfo->find(L);
    assert(DefItr != mDefInfo->end() &&
      DefItr->get<DefUseSet>() && DefItr->get<ReachSet>() &&
      "Def-use and reach definition set must be specified!");
    auto LiveItr = mLiveInfo->find(L);
    assert(LiveItr != mLiveInfo->end() && LiveItr->get<LiveSet>() &&
      "List of live locations must be specified!");
    TraitMap ExplicitAccesses;
    UnknownMap ExplicitUnknowns;
    AliasMap NodeTraits;
    for (auto &N : *mAliasTree)
      NodeTraits.insert(
        std::make_pair(&N, std::make_tuple(TraitList(), UnknownList())));
    DependenceMap Deps;
    collectDependencies(L->getLoop(), Deps, Cache);
    resolveAccesses(L->getLoop(), R->getLatchNode(), R->getExitNode(),
      *DefItr->get<DefUseSet>(), *LiveItr->get<LiveSet>(), Deps, AliasSTR,
      ExplicitAccesses, ExplicitUnknowns, NodeTraits);
    resolvePointers(*DefItr->get<DefUseSet>(), ExplicitAccesses);
    resolveAddresses(L, *DefItr->get<DefUseSet>(), ExplicitAccesses,
      ExplicitUnknowns, NodeTraits);
    collectHeaderAccesses(L->getLoop(), *DefItr->get<DefUseSet>(),
      ExplicitAccesses, ExplicitUnknowns);
    propagateTraits(Numbers, *R, ExplicitAccesses, ExplicitUnknowns, NodeTraits,
      Deps, PrivInfo.first->get<DependenceSet>());
  }
  for (auto I = R->region_begin(), E = R->region_end(); I != E; ++I)
    resolveCandidats(Numbers, AliasSTR, *I, Cache);
}

void PrivateRecognitionPass::insertDependence(const Dependence &Dep,
  const MemoryLocation &Src, const MemoryLocation Dst,
  trait::Dependence::Flag Flag, Loop &L, DependenceMap &Deps) {
  auto LoopDepth = L.getLoopDepth();
  if (Dep.isConfused() || Dep.getConfusedLevels() >= LoopDepth) {
    LLVM_DEBUG(dbgs() << "[PRIVATE]: assume confused dependence"
      " (confused levels " << Dep.getConfusedLevels() << ")\n");
    DependenceImp::Descriptor Dptr;
    Dptr.set<trait::Flow, trait::Anti, trait::Output>();
    trait::Dependence::Flag Flag = trait::Dependence::ConfusedCause |
      trait::Dependence::LoadStoreCause | trait::Dependence::May;
    updateDependence(mAliasTree->find(Src), Dptr, Flag, DistanceInfo{}, Deps);
    updateDependence(mAliasTree->find(Dst), Dptr, Flag, DistanceInfo{}, Deps);
    return;
  }
  for (unsigned OuterDepth =
    Dep.getConfusedLevels() > 0 ? Dep.getConfusedLevels() : 1;
    OuterDepth < LoopDepth; ++OuterDepth) {
    auto Dir = Dep.getDirection(OuterDepth);
    if (Dir != Dependence::DVEntry::EQ && Dir != Dependence::DVEntry::ALL &&
      Dir != Dependence::DVEntry::LE && Dir != Dependence::DVEntry::GE) {
      LLVM_DEBUG(dbgs() << "[PRIVATE]: ignore loop independent dependence (due "
        "to outer loop dependence direction)\n");
      return;
    }
  }
  auto Dir = Dep.getDirection(LoopDepth);
  if (Dir == Dependence::DVEntry::EQ) {
    LLVM_DEBUG(dbgs() << "[PRIVATE]: ignore loop independent dependence\n");
    return;
  }
  assert((Dep.isOutput() || Dep.isAnti() || Dep.isFlow()) &&
    "Unknown kind of dependency!");
  DistanceInfo Dist{Dep, L.getLoopDepth(), DistanceInfo::NotChange, *mSE};
  DependenceImp::Descriptor Dptr;
  if (Dep.isOutput()) {
    if (Dir == Dependence::DVEntry::GT || Dir == Dependence::DVEntry::GE)
      Dist.Transform |= DistanceInfo::Revert;
    Dptr.set<trait::Output>();
  } else if (Dir == Dependence::DVEntry::ALL) {
    Dptr.set<trait::Flow, trait::Anti>();
    Dist.Transform |= DistanceInfo::Extend;
  } else if (Dep.isFlow())
    if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE) {
      Dptr.set<trait::Flow>();
    } else {
      Dptr.set<trait::Anti>();
      Dist.Transform |= DistanceInfo::Revert;
    }
  else if (Dep.isAnti())
    if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE) {
      Dptr.set<trait::Anti>();
    } else {
      Dptr.set<trait::Flow>();
      Dist.Transform |= DistanceInfo::Revert;
    }
  else {
    Dptr.set<trait::Flow, trait::Anti>();
    Dist.Transform |= DistanceInfo::Extend;
  }
  updateDependence(mAliasTree->find(Src), Dptr,
                   trait::Dependence::LoadStoreCause | Flag, std::move(Dist),
                   Deps);
  updateDependence(mAliasTree->find(Dst), Dptr,
                   trait::Dependence::LoadStoreCause | Flag, std::move(Dist),
                   Deps);
}

void PrivateRecognitionPass::collectDependencies(Loop *L, DependenceMap &Deps,
    DependenceCache &Cache) {
  auto &AA = mAliasTree->getAliasAnalysis();
  std::vector<Instruction *> LoopInsts;
  for (auto *BB : L->getBlocks())
    for (auto &I : *BB)
      LoopInsts.push_back(&I);
  for (auto SrcItr = LoopInsts.begin(), EndItr = LoopInsts.end();
       SrcItr != EndItr; ++SrcItr) {
    if (!(**SrcItr).mayReadOrWriteMemory())
      continue;
    auto Src = getLoadOrStoreLocation(*SrcItr);
    if (!Src.Ptr) {
      if (auto II = dyn_cast<IntrinsicInst>(*SrcItr))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()))
          continue;
      for (auto DstItr = SrcItr; DstItr != EndItr; ++DstItr) {
        if (!(**DstItr).mayReadOrWriteMemory())
          continue;
        if (auto II = dyn_cast<IntrinsicInst>(*DstItr))
          if (isMemoryMarkerIntrinsic(II->getIntrinsicID()))
            continue;
        trait::Dependence::Flag Flag = trait::Dependence::May |
          trait::Dependence::UnknownDistance |
          (!isa<CallBase>(*SrcItr) && !isa<CallBase>(*DstItr)
             ? trait::Dependence::UnknownCause
             : trait::Dependence::CallCause);
        DependenceImp::Descriptor Dptr;
        Dptr.set<trait::Flow, trait::Anti, trait::Output>();
        SmallVector<Value *, 2> Causes;
        if (isa<CallBase>(*SrcItr))
          Causes.push_back(*SrcItr);
        if (isa<CallBase>(*DstItr))
          Causes.push_back(*DstItr);
        auto insertUnknownDep =
          [this, &AA, &SrcItr, &DstItr, &Dptr, Flag, &Causes, &Deps](
            Instruction &, MemoryLocation &&Loc, unsigned,
            AccessInfo R, AccessInfo W) {
          if (R == AccessInfo::No && W == AccessInfo::No)
            return;
          if (AA.getModRefInfo(*SrcItr, Loc) == ModRefInfo::NoModRef)
            return;
          if (AA.getModRefInfo(*DstItr, Loc) == ModRefInfo::NoModRef)
            return;
          updateDependence(mAliasTree->find(Loc), Dptr, Flag, DistanceInfo{},
                           Deps, Causes);
        };
        auto stab = [](Instruction &, AccessInfo, AccessInfo) {};
        LLVM_DEBUG(dbgs() << "[PRIVATE]: conservatively assume dependence: ";
                   (**SrcItr).print(dbgs()); dbgs() << "\n";
                   (**DstItr).print(dbgs()); dbgs() << "\n");
        for_each_memory(**SrcItr, *mTLI, insertUnknownDep, stab);
        for_each_memory(**DstItr, *mTLI, insertUnknownDep, stab);
      }
    } else {
      for (auto DstItr = SrcItr; DstItr != EndItr; ++DstItr) {
        auto Dst = getLoadOrStoreLocation(*DstItr);
        if (!Dst.Ptr) {
          if (!(**DstItr).mayReadOrWriteMemory())
            continue;
          if (auto II = dyn_cast<IntrinsicInst>(*DstItr))
            if (isMemoryMarkerIntrinsic(II->getIntrinsicID()))
              continue;
          if (AA.getModRefInfo(*DstItr, Src) == ModRefInfo::NoModRef)
            continue;
          trait::Dependence::Flag Flag = trait::Dependence::May |
            trait::Dependence::UnknownDistance |
            (!isa<CallBase>(*DstItr) ? trait::Dependence::UnknownCause :
              trait::Dependence::CallCause);
          DependenceImp::Descriptor Dptr;
          Dptr.set<trait::Flow, trait::Anti, trait::Output>();
          LLVM_DEBUG(dbgs() << "[PRIVATE]: conservatively assume dependence: ";
                     (**SrcItr).print(dbgs()); dbgs() << "\n";
                     (**DstItr).print(dbgs()); dbgs() << "\n");
          updateDependence(mAliasTree->find(Src), Dptr, Flag, DistanceInfo{},
                           Deps, isa<CallBase>(*DstItr) ? *DstItr : nullptr);
        } else {
          if (!(*SrcItr)->mayWriteToMemory() &&
              !(*DstItr)->mayWriteToMemory()) {
            LLVM_DEBUG(dbgs() << "[PRIVATE]: ignore input dependence\n");
            continue;
          }
          auto CacheItr = Cache.Impl.find(std::make_pair(*SrcItr, *DstItr));
          unsigned short ConfusedLevels;
          Dependence *Dep = nullptr;
          if (CacheItr != Cache.Impl.end()) {
            Dep = CacheItr->second.first.get();
            ConfusedLevels = CacheItr->second.second;
          } else {
            auto D = mDepInfo->depends(*SrcItr, *DstItr, true, &ConfusedLevels);
            Dep = D.get();
            Cache.Impl.try_emplace(std::make_pair(*SrcItr, *DstItr),
              std::move(D), ConfusedLevels);
          }
          if (Dep) {
            LLVM_DEBUG(
              dbgs() << "[PRIVATE]: dependence found: ";
              Dep->dump(dbgs());
              (**SrcItr).print(dbgs()); dbgs() << "\n";
              (**DstItr).print(dbgs()); dbgs() << "\n";
            );
            // Do not use Dependence::isLoopIndependent() to check loop
            // independent dependencies. This method returns `may` instead of
            // `must`. This means that if it returns `true` than dependency
            // may be loop-carried or may arise inside a single iteration.
            insertDependence(*Dep, Src, Dst, trait::Dependence::No, *L, Deps);
          } else if (L->getLoopDepth() <= ConfusedLevels) {
            LLVM_DEBUG(dbgs() << "[PRIVATE]: assume confused dependence"
              " (confused levels " << ConfusedLevels << ")\n");
            DependenceImp::Descriptor Dptr;
            Dptr.set<trait::Flow, trait::Anti, trait::Output>();
            trait::Dependence::Flag Flag = trait::Dependence::ConfusedCause |
              trait::Dependence::LoadStoreCause | trait::Dependence::May;
            updateDependence(mAliasTree->find(Src), Dptr, Flag, DistanceInfo{},
                             Deps);
            updateDependence(mAliasTree->find(Dst), Dptr, Flag, DistanceInfo{},
                             Deps);
          }
        }
      }
    }
  }
}

void PrivateRecognitionPass::resolveAccesses(Loop *L, const DFNode *LatchNode,
    const DFNode *ExitNode, const tsar::DefUseSet &DefUse,
    const tsar::LiveSet &LS, const DependenceMap &Deps,
    const AliasTreeRelation &AliasSTR, TraitMap &ExplicitAccesses,
    UnknownMap &ExplicitUnknowns, AliasMap &NodeTraits) {
  assert(LatchNode && "Latch node must not be null!");
  assert(ExitNode && "Exit node must not be null!");
  auto LatchDefItr = mDefInfo->find(const_cast<DFNode *>(LatchNode));
  assert(LatchDefItr != mDefInfo->end() && LatchDefItr->get<ReachSet>() &&
    "Reach definition set must be specified!");
  auto &LatchDF = LatchDefItr->get<ReachSet>();
  assert(LatchDF && "List of must/may defined locations must not be null!");
  // LatchDefs is a set of must/may define locations before a branch to
  // a next arbitrary iteration.
  const DefinitionInfo &LatchDefs = LatchDF->getOut();
  // ExitingDefs is a set of must and may define locations which obtains
  // definitions in the iteration in which exit from a loop takes place.
  auto ExitDefItr = mDefInfo->find(const_cast<DFNode *>(ExitNode));
  assert(ExitDefItr != mDefInfo->end() && ExitDefItr->get<ReachSet>() &&
    "Reach definition set must be specified!");
  auto &ExitDF = ExitDefItr->get<ReachSet>();
  assert(ExitDF && "List of must/may defined locations must not be null!");
  const DefinitionInfo &ExitingDefs = ExitDF->getOut();
  for (const auto &Loc : DefUse.getExplicitAccesses()) {
    const EstimateMemory *Base = mAliasTree->find(Loc);
    assert(Base && "Estimate memory location must not be null!");
    auto Pair = ExplicitAccesses.insert(std::make_pair(Base, nullptr));
    if (Pair.second) {
      auto I = NodeTraits.find(Base->getAliasNode(*mAliasTree));
      I->get<TraitList>().push_front(
        std::make_pair(Base, BitMemoryTrait::NoRedundant));
      Pair.first->get<BitMemoryTrait>() =
        &I->get<TraitList>().front().get<BitMemoryTrait>();
    }
    auto &CurrTraits = *Pair.first->get<BitMemoryTrait>();
    BitMemoryTrait SharedTrait = ~BitMemoryTrait::NoAccess;
    BitMemoryTrait DefTrait = BitMemoryTrait::Dependency;
    if (!Deps.count(Base)) {
      SharedTrait = BitMemoryTrait::SharedJoin;
      DefTrait = BitMemoryTrait::Shared;
    }
    // TODO (kaniandr@gmail.com): live memory analysis does not expand
    // analysis results from aggregated array representation to explicit
    // accesses, so we conservatively use analysis for the whole array
    // instead of analysis results for an explicitly accessed memory location.
    auto isLiveAggregate = [this, &LS](auto &Loc) {
      auto BasePtr = getUnderlyingObject(const_cast<Value *>(Loc.Ptr), 0);
      if (BasePtr == Loc.Ptr)
        return false;
      return LS.getOut().overlap(
          MemoryLocation::getAfter(BasePtr, Loc.AATags));
    };
    if (!DefUse.hasUse(Loc)) {
      if (!LS.getOut().overlap(Loc) && !isLiveAggregate(Loc))
        CurrTraits &= BitMemoryTrait::Private | SharedTrait;
      else {
        auto *Expr = mSE->getSCEV(const_cast<Value *>(Loc.Ptr));
        bool IsInvariant =
          isLoopInvariant(Expr, L, *mTLI, *mSE, DefUse, *mAliasTree, AliasSTR);
        if (IsInvariant && ExitingDefs.MustReach.contain(Loc))
          CurrTraits &= BitMemoryTrait::LastPrivate | SharedTrait;
        else if (IsInvariant && LatchDefs.MustReach.contain(Loc) &&
                 !ExitingDefs.MayReach.overlap(Loc))
          // These location will be stored as second to last private, i.e.
          // the last definition of these locations is executed on the
          // second to the last loop iteration (on the last iteration the
          // loop condition check is executed only).
          // It is possible that there is only one (last) iteration in
          // the loop. In this case the location has not been assigned and
          // must be declared as a first private.
          CurrTraits &= BitMemoryTrait::SecondToLastPrivate &
          BitMemoryTrait::FirstPrivate | SharedTrait;
        else
          // There is no certainty that the location is always assigned
          // the value in the loop. Therefore, it must be declared as a
          // first private, to preserve the value obtained before the loop
          // if it has not been assigned.
          CurrTraits &= BitMemoryTrait::DynamicPrivate &
          BitMemoryTrait::FirstPrivate | SharedTrait;
        CurrTraits &= BitMemoryTrait::UseAfterLoop;
      }
    } else if ((DefUse.hasMayDef(Loc) || DefUse.hasDef(Loc))) {
      CurrTraits &= DefTrait;
      if (LS.getOut().overlap(Loc) || isLiveAggregate(Loc))
        CurrTraits &= BitMemoryTrait::UseAfterLoop;
    } else {
      CurrTraits &= BitMemoryTrait::Readonly;
      if (LS.getOut().overlap(Loc) || isLiveAggregate(Loc))
        CurrTraits &= BitMemoryTrait::UseAfterLoop;
    }
    LLVM_DEBUG(updateTraitsLog(Base, CurrTraits));
  }
  for (const auto &Unknown : DefUse.getExplicitUnknowns()) {
    const auto N = mAliasTree->findUnknown(Unknown);
    assert(N && "Alias node for unknown memory location must not be null!");
    auto I = NodeTraits.find(N);
    auto &AA = mAliasTree->getAliasAnalysis();
    auto *Call = dyn_cast<CallBase>(Unknown);
    BitMemoryTrait TID = (Call && AA.onlyReadsMemory(Call)) ?
        BitMemoryTrait::Readonly : BitMemoryTrait::Dependency;
    TID &= BitMemoryTrait::NoRedundant;
    I->get<UnknownList>().push_front(
      std::make_pair(Unknown, TID));
    ExplicitUnknowns.insert(std::make_pair(Unknown,
      std::make_tuple(N, &I->get<UnknownList>().front().get<BitMemoryTrait>())));
  }
}

void PrivateRecognitionPass::resolvePointers(
    const tsar::DefUseSet &DefUse, TraitMap &ExplicitAccesses) {
  for (const auto &Loc : DefUse.getExplicitAccesses()) {
    auto BasePtr = getUnderlyingObject(const_cast<Value *>(Loc.Ptr), 0);
    // *p means that address of location should be loaded from p using 'load'.
    if (auto *LI = dyn_cast<LoadInst>(BasePtr)) {
      auto *EM = mAliasTree->find(Loc);
      assert(EM && "Estimate memory location must not be null!");
      auto LocTraits = ExplicitAccesses.find(EM);
      assert(LocTraits != ExplicitAccesses.end() &&
        "Traits of location must be initialized!");
      if (dropSharedFlag(dropUnitFlag(*LocTraits->get<BitMemoryTrait>()))
            == BitMemoryTrait::Private ||
          dropUnitFlag(*LocTraits->get<BitMemoryTrait>())
            == BitMemoryTrait::Readonly ||
          dropUnitFlag(*LocTraits->get<BitMemoryTrait>())
            == BitMemoryTrait::Shared)
        continue;
      const EstimateMemory *Ptr = mAliasTree->find(MemoryLocation::get(LI));
      assert(Ptr && "Estimate memory location must not be null!");
      // DefUse.getExplicitAccesses() contains largest memory location, so
      // if there are two instructions which access <P,8> and <P,?> then
      // <P,?> will be stored in DefUse.getExplicitlyAccesses() and related
      // estimate memory will be stored in ExplisitAccesses. So, we traverse
      // estimate memory chain to determine which memory is analyzed.
      auto PtrTraits = ExplicitAccesses.find(Ptr);
      using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
      while (PtrTraits == ExplicitAccesses.end() && (Ptr = CT::getNext(Ptr)))
        PtrTraits = ExplicitAccesses.find(Ptr);
      // If Ptr is null it means that it has been dereferenced outside the loop.
      // int **P, *V = *P; for(...) { ...V... }
      // In this case after memory promotion V becomes *P, however corresponding
      // 'load' remains outside the loop.
      if (!Ptr || dropUnitFlag(*PtrTraits->get<BitMemoryTrait>())
            == BitMemoryTrait::Readonly)
        continue;
      // Location can not be declared as copy in or copy out without
      // additional analysis because we do not know which memory must
      // be copy. Let see an example:
      // for (...) { P = &X; *P = ...; P = &Y; } after loop P = &Y, not &X.
      // P = &Y; for (...) { *P = ...; P = &X; } before loop P = &Y, not &X.
      // Note that case when location is shared, but pointer is not read-only
      // may be difficulty to implement for distributed memory, for example:
      // for(...) { P = ...; ... = *P; } It is not evident which memory
      // should be copy to each processor.
      *LocTraits->get<BitMemoryTrait>() &= BitMemoryTrait::Dependency;
    }
  }
}

void PrivateRecognitionPass::resolveAddresses(DFLoop *L,
    const DefUseSet &DefUse, TraitMap &ExplicitAccesses,
    UnknownMap &ExplicitUnknowns, AliasMap &NodeTraits) {
  assert(L && "Loop must not be null!");
  Loop *Lp = L->getLoop();
  for (Value *Ptr : DefUse.getAddressAccesses()) {
    const EstimateMemory* Base = mAliasTree->find(MemoryLocation(Ptr, 0));
    assert(Base && "Estimate memory location must not be null!");
    auto Root = Base->getTopLevelParent();
    // Do not remember an address:
    // * if it is stored in some location, for example
    // isa<LoadInst>(Root->front()), locations are analyzed separately;
    // * if it points to a temporary location and should not be analyzed:
    // for example, a result of a call can be a pointer.
    if (!isa<AllocaInst>(Root->front()) && !isa<GlobalValue>(Root->front()))
      continue;
    // If this is an address of a location declared in the loop do not
    // remember it.
    if (auto AI = dyn_cast<AllocaInst>(Root->front()))
      if (Lp->contains(AI->getParent()))
        continue;
    for (auto &U : Ptr->uses()) {
      auto *User = U.getUser();
      if (auto II = dyn_cast<IntrinsicInst>(User))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      SmallDenseMap<Instruction *, Use *, 1> UseInsts;
      if (auto *CE = dyn_cast<ConstantExpr>(User)) {
        SmallVector<ConstantExpr *, 4> WorkList{ CE };
        do {
          auto *Expr = WorkList.pop_back_val();
          for (auto &ExprU : Expr->uses()) {
            auto ExprUse = ExprU.getUser();
            if (auto ExprUseInst = dyn_cast<Instruction>(ExprUse))
              UseInsts.try_emplace(ExprUseInst, &ExprU);
            else if (auto ExprUseExpr = dyn_cast<ConstantExpr>(ExprUse))
              WorkList.push_back(ExprUseExpr);
          }
        } while (!WorkList.empty());
      } else if (auto UI = dyn_cast<Instruction>(User)) {
        UseInsts.try_emplace(UI, &U);
      }
      if (UseInsts.empty())
        continue;
      if (!any_of(UseInsts, [Lp, User](std::pair<Instruction *, Use *> &I) {
            if (!Lp->contains(I.first->getParent()))
              return false;
            // The address is used inside the loop.
            // Remember it if it is used for computation instead of memory
            // access or if we do not know how it will be used.
            if (isa<PtrToIntOperator>(User))
              return true;
            if (auto *SI = dyn_cast<StoreInst>(I.first)) {
              if (I.second->getOperandNo() ==
                  StoreInst::getPointerOperandIndex())
                return false;
              if (pointsToLocalMemory(*SI->getPointerOperand(), *Lp))
                return false;
              return true;
            }
            // Address should be also remembered if it is a function parameter
            // because it is not known how it is used inside a function.
            auto *Call = dyn_cast<CallBase>(I.first);
            if (Call && Call->isArgOperand(I.second)) {
              if (auto Callee = llvm::dyn_cast<Function>(
                      Call->getCalledOperand()->stripPointerCasts())) {
                // Function declaration may have unknown number of arguments.
                // The function is casted to the valid prototype before a call.
                return Callee->arg_size() > Call->getArgOperandNo(I.second) &&
                       !Callee->getArg(Call->getArgOperandNo(I.second))
                            ->hasAttribute(Attribute::NoCapture);
              }
              return true;
            }
            return false;
          }))
        continue;
      auto Pair = ExplicitAccesses.insert(std::make_pair(Base, nullptr));
      if (!Pair.second) {
        *Pair.first->get<BitMemoryTrait>() &= BitMemoryTrait::AddressAccess;
      } else {
        auto I = NodeTraits.find(Base->getAliasNode(*mAliasTree));
        I->get<TraitList>().push_front(
          std::make_pair(Base, BitMemoryTrait(BitMemoryTrait::NoRedundant &
            BitMemoryTrait::NoAccess & BitMemoryTrait::AddressAccess)));
        Pair.first->get<BitMemoryTrait>() =
          &I->get<TraitList>().front().get<BitMemoryTrait>();
      }
      break;
    }
  }
  for (auto &[Ptr, Insts] : DefUse.getAddressTransitives()) {
    // TODO (kaniandr@gmail.com): extend address access analysis and set
    // 'nocapture'-like attribute for global variables.
    const EstimateMemory *Base = mAliasTree->find(MemoryLocation(Ptr, 0));
    assert(Base && "Estimate memory location must not be null!");
    auto Pair = ExplicitAccesses.insert(std::make_pair(Base, nullptr));
    if (!Pair.second) {
      *Pair.first->get<BitMemoryTrait>() &= BitMemoryTrait::AddressAccess;
    } else {
      auto I = NodeTraits.find(Base->getAliasNode(*mAliasTree));
      I->get<TraitList>().push_front(
          std::make_pair(Base, BitMemoryTrait(BitMemoryTrait::NoRedundant &
                                              BitMemoryTrait::NoAccess &
                                              BitMemoryTrait::AddressAccess)));
      Pair.first->get<BitMemoryTrait>() =
          &I->get<TraitList>().front().get<BitMemoryTrait>();
    }
  }
  for (auto *Unknown : DefUse.getAddressUnknowns()) {
    /// Is it safe to ignore intrinsics here? It seems that all intrinsics in
    /// LLVM does not use addresses to perform  computations instead of
    /// memory accesses. We also ignore intrinsics in DefinedMemoryPass.
    if (isa<IntrinsicInst>(Unknown))
      continue;
    const auto *N = mAliasTree->findUnknown(Unknown);
    assert(N && "Alias node for unknown memory location must not be null!");
    auto Pair = ExplicitUnknowns.try_emplace(Unknown);
    if (!Pair.second) {
      *Pair.first->get<BitMemoryTrait>() &= BitMemoryTrait::AddressAccess;
    } else {
      auto I = NodeTraits.find(N);
      I->get<UnknownList>().push_front(std::make_pair(
          Unknown, BitMemoryTrait::NoRedundant & BitMemoryTrait::NoAccess &
                       BitMemoryTrait::AddressAccess));
    Pair.first->get<BitMemoryTrait>() =
      &I->get<UnknownList>().front().get<BitMemoryTrait>();
    Pair.first->get<AliasNode>() = N;
    }
  }
}

void PrivateRecognitionPass::propagateTraits(
    const tsar::GraphNumbering<const AliasNode *> &Numbers,
    const tsar::DFRegion &R,
    TraitMap &ExplicitAccesses, UnknownMap &ExplicitUnknowns,
    AliasMap &NodeTraits, DependenceMap &Deps, DependenceSet &DS) {
  LLVM_DEBUG(dbgs() << "[PRIVATE]: propagate traits\n");
  std::stack<TraitPair> ChildTraits;
  auto *Prev = mAliasTree->getTopLevelNode();
  // Such initialization of Prev is sufficient for the first iteration, then
  // it will be overwritten.
  for (auto *N : post_order(mAliasTree)) {
    auto NTItr = NodeTraits.find(N);
    if (Prev->getParent(*mAliasTree) == N) {
      // All children has been analyzed and now it is possible to combine
      // obtained results and to propagate to a current node N.
      for (auto &Child : make_range(N->child_begin(), N->child_end())) {
        // This for loop is used to extract all necessary information from
        // the ChildTraits stack. Number of pop() calls should be the same
        // as a number of children.
        auto &CT = ChildTraits.top();
        ChildTraits.pop();
        for (auto &EMToT : *CT.get<TraitList>()) {
          auto Parent = EMToT.get<EstimateMemory>()->getParent();
          if (!Parent || Parent->getAliasNode(*mAliasTree) != N) {
            NTItr->get<TraitList>().push_front(std::move(EMToT));
          } else {
            auto EA = ExplicitAccesses.find(Parent);
            if (EA != ExplicitAccesses.end()) {
              *EA->get<BitMemoryTrait>() &= EMToT.get<BitMemoryTrait>();
              mergeDependence(Parent, *EA->get<BitMemoryTrait>(),
                EMToT.get<EstimateMemory>(), Deps);
            } else
              mergeDependence(Parent, EMToT.get<BitMemoryTrait>(),
                EMToT.get<EstimateMemory>(), Deps);
              NTItr->get<TraitList>().push_front(
                std::make_pair(Parent, std::move(EMToT.get<BitMemoryTrait>())));
          }
        }
        for (auto &UToT : *CT.get<UnknownList>())
          NTItr->get<UnknownList>().push_front(std::move(UToT));
      }
    }
    auto &TL = NTItr->get<TraitList>();
    LLVM_DEBUG(removeRedundantLog(TL, "before"));
    for (auto BI = TL.before_begin(), I = TL.begin(), E = TL.end(); I != E;)
      removeRedundant(N, NTItr->get<TraitList>(), BI, I, Deps);
    LLVM_DEBUG(removeRedundantLog(TL, "after"));
    TraitPair NT(&NTItr->get<TraitList>(), &NTItr->get<UnknownList>());
    storeResults(
      Numbers, R, *N, ExplicitAccesses, ExplicitUnknowns, Deps, NT, DS);
    ChildTraits.push(std::move(NT));
    Prev = N;
  }
  sanitizeCombinedTraits(DS);
  std::vector<const AliasNode *> Coverage;
  explicitAccessCoverage(DS, *mAliasTree, Coverage);
  // All descendant nodes for nodes in `Coverage` access some part of
  // explicitly accessed memory. The conservativeness of analysis implies
  // that memory accesses from this nodes arise loop carried dependencies.
  for (auto *N : Coverage)
    for (auto &Child : make_range(N->child_begin(), N->child_end()))
      for (auto *Descendant : make_range(df_begin(&Child), df_end(&Child))) {
        auto I = DS.find_as(Descendant);
        if (I != DS.end() && !I->is<trait::NoAccess>())
          I->set<trait::Flow, trait::Anti, trait::Output>();
      }
}

static EstimateMemoryTrait *mayIgnoreDereference(AliasTrait &Trait,
    const AliasTree &Tree, DependenceSet &DS) {
  EstimateMemoryTrait *MainMemory = nullptr;
  for (auto &T : Trait) {
    auto *TopEM = T.getMemory()->getTopLevelParent();
    if (auto *LI = dyn_cast<LoadInst>(TopEM->front())) {
      const EstimateMemory *Ptr = Tree.find(MemoryLocation::get(LI));
      assert(Ptr && "Estimate memory location must not be null!");
      auto Itr = DS.find_as(Ptr->getAliasNode(Tree));
      if (Itr == DS.end())
        return nullptr;
      if (Itr->is<trait::Private>())
        continue;
      return nullptr;
    }
    if (MainMemory)
      return nullptr;
    MainMemory = &T;
  }
  return MainMemory;
}

void PrivateRecognitionPass::sanitizeCombinedTraits(DependenceSet &DS) {
  for (auto &AT : DS) {
    if (AT.unknown_empty() && AT.count() == 1)
      continue;
    if (AT.is_any<trait::Anti, trait::Flow, trait::Output>()) {
      if (auto *T = mayIgnoreDereference(AT, *mAliasTree, DS)) {
        BitMemoryTrait BitTrait(*T);
        BitTrait = dropUnitFlag(BitTrait);
        bcl::trait::set(BitTrait.toDescriptor(0, NumTraits), AT);
      }
      /// Due to conservativeness of analysis type of dependencies must be the
      /// same for all locations in the node.
      /// Let us consider an example.
      /// for (...) X[...] = Y[...];
      /// Analysis can not be performed accurately if X and Y may alias.
      /// Dependence analysis pass tests the following pairs of accesses:
      /// W(X)-W(X), W(X)-R(Y), R(Y)-R(Y) (W means 'write' and R means 'read').
      /// So, if X produces 'output' dependence there is no way to understand
      /// that Y is also produced 'output' dependence (due to memory
      /// overlapping). Then it is necessary to iterate over all accessed
      /// locations and to update their traits.
      DependenceImp::Descriptor Dptr;
      bcl::trait::set(AT, Dptr);
      for (auto &T : AT) {
        bcl::trait::set(Dptr, T);
        LLVM_DEBUG(dbgs() << "[PRIVATE]: conservatively update trait of ";
        printLocationSource(dbgs(), *T.getMemory()); dbgs() << " to ";
        T.get().print(dbgs()); dbgs() << "\n";);
      }
    }
  }
}

void PrivateRecognitionPass::checkFirstPrivate(
    const GraphNumbering<const AliasNode *> &Numbers,
    const DFRegion &R,
    const TraitList::iterator &TraitItr, MemoryDescriptor &Dptr) {
  if (Dptr.is<trait::FirstPrivate>() ||
      !Dptr.is<trait::LastPrivate>() && !Dptr.is<trait::SecondToLastPrivate>())
    return;
  auto LatchNode = R.getLatchNode();
  assert(LatchNode && "Latch node must not be null!");
  auto ExitNode = R.getExitNode();
  assert(ExitNode && "Exit node must not be null!");
  auto LatchDefItr = mDefInfo->find(const_cast<DFNode *>(LatchNode));
  assert(LatchDefItr != mDefInfo->end() && LatchDefItr->get<ReachSet>() &&
    "Reach definition set must be specified!");
  auto &LatchDF = LatchDefItr->get<ReachSet>();
  assert(LatchDF && "List of must/may defined locations must not be null!");
  // LatchDefs is a set of must/may define locations before a branch to
  // a next arbitrary iteration.
  const DefinitionInfo &LatchDefs = LatchDF->getOut();
  // ExitingDefs is a set of must and may define locations which obtains
  // definitions in the iteration in which exit from a loop takes place.
  auto ExitDefItr = mDefInfo->find(const_cast<DFNode *>(ExitNode));
  assert(ExitDefItr != mDefInfo->end() && ExitDefItr->get<ReachSet>() &&
    "Reach definition set must be specified!");
  auto &ExitDF = ExitDefItr->get<ReachSet>();
  assert(ExitDF && "List of must/may defined locations must not be null!");
  const DefinitionInfo &ExitingDefs = ExitDF->getOut();
  auto isAmbiguousCover = [](
     const LocationDFValue &Reach, const EstimateMemory &EM) {
    for (auto *Ptr : EM)
      if (!Reach.contain(MemoryLocation(Ptr, EM.getSize(), EM.getAAInfo())))
        return false;
    return true;
  };
  auto EM = TraitItr->get<EstimateMemory>();
  SmallVector<const EstimateMemory *, 8> DefLeafs;
  for (auto *Descendant : make_range(df_begin(EM), df_end(EM))) {
    if (!Descendant->isLeaf())
      continue;
    if (Dptr.is<trait::LastPrivate>()) {
      if (!isAmbiguousCover(ExitingDefs.MustReach, *Descendant))
        continue;
    } else if (Dptr.is<trait::SecondToLastPrivate>()) {
      /// TODO (kaniandr@gmail.com): it seams that ExitingDefs should not be
      /// checked because SecondToLastPrivate location must not be written on
      /// the last iteration.
      if (!isAmbiguousCover(LatchDefs.MustReach, *Descendant) &&
          !isAmbiguousCover(ExitingDefs.MustReach, *Descendant))
        continue;
    }
    DefLeafs.push_back(Descendant);
  }
  /// TODO (kaniandr@gmail.com): the same check should be added into reach
  /// definition and live memory analysis paths to increase precision of
  /// analysis of explicitly accessed locations which extend some other
  /// locations.
  if (cover(*mAliasTree, Numbers, *EM, DefLeafs.begin(), DefLeafs.end()))
    return;
  if (hasSharedJoin(TraitItr->get<BitMemoryTrait>()))
    TraitItr->get<BitMemoryTrait>() &=
      BitMemoryTrait::FirstPrivate | BitMemoryTrait::SharedJoin;
  else
    TraitItr->get<BitMemoryTrait>() &= BitMemoryTrait::FirstPrivate;
  Dptr.set<trait::FirstPrivate>();
}

void PrivateRecognitionPass::removeRedundant(
    const AliasNode *N, TraitList &Traits, TraitList::iterator &BeforeCurrItr,
    TraitList::iterator &CurrItr, DependenceMap &Deps) {
  assert(CurrItr != Traits.end() && "Iterator must be valid!");
  auto BeforeI = CurrItr, I = CurrItr, E = Traits.end();
  auto Current = CurrItr->get<EstimateMemory>();
  // It is necessary to find the largest estimate location which covers
  // the current one and is associated with the currently analyzed node `N`.
  // Note, that if current location is not stored in `N` it means that this
  // locations is stored in one of proper descendant of `N`. It also means
  // that proper ancestors of the location in estimate tree is stored in
  // proper ancestors of `N` (see propagateTraits()) and the current locations
  // should not be analyzed.
  // This search is performed before a redundancy test is executed for the
  // current location, because it also may produce redundancy.
  if (Current->getAliasNode(*mAliasTree) == N) {
    while (Current->getParent() &&
      Current->getParent()->getAliasNode(*mAliasTree) == N)
      Current = Current->getParent();
    // It is not necessary to execute a conjunction of traits here. If Current
    // is not explicitly accessed in the loop then there are no traits and
    // conjunction will change nothing. However, if Current is explicitly
    // accessed it is presented in a TraitList as a separate item and will be
    // processed separately.
    mergeDependence(Current, CurrItr->get<BitMemoryTrait>(),
      CurrItr->get<EstimateMemory>(), Deps);
    CurrItr->get<EstimateMemory>() = Current;
  }
  for (++I; I != E;) {
    if (Current == I->get<EstimateMemory>()) {
      I->get<BitMemoryTrait>() &= CurrItr->get<BitMemoryTrait>();
      CurrItr = Traits.erase_after(BeforeCurrItr);
      return;
    }
    auto Ancestor = ancestor(Current, I->get<EstimateMemory>());
    if (Ancestor == I->get<EstimateMemory>()) {
      I->get<BitMemoryTrait>() &= CurrItr->get<BitMemoryTrait>();
      mergeDependence(
        I->get<EstimateMemory>(), I->get<BitMemoryTrait>(), Current, Deps);
      CurrItr = Traits.erase_after(BeforeCurrItr);
      return;
    }
    if (Ancestor == Current) {
      CurrItr->get<BitMemoryTrait>() &= I->get<BitMemoryTrait>();
      mergeDependence(
        Current, CurrItr->get<BitMemoryTrait>(), I->get<EstimateMemory>(), Deps);
      I = Traits.erase_after(BeforeI);
    } else {
      ++BeforeI; ++I;
    }
  }
  ++BeforeCurrItr; ++CurrItr;
}

void PrivateRecognitionPass::storeResults(
    const GraphNumbering<const tsar::AliasNode *> &Numbers,
    const DFRegion &R, const AliasNode &N,
    const TraitMap &ExplicitAccesses, const UnknownMap &ExplicitUnknowns,
    const DependenceMap &Deps, const TraitPair &Traits, DependenceSet &DS) {
  assert(DS.find_as(&N) == DS.end() && "Results must not be already stored!");
  auto storeDepIfNeed = [this, &Deps](TraitList::iterator EMI,
      AliasTrait::iterator EMTraitItr) {
    auto EMToDep = Deps.find(EMI->get<EstimateMemory>());
    assert(EMToDep != Deps.end() &&
      "Dependence must be presented in the map!");
    auto Dep = EMToDep->get<DependenceImp>().get();
    Dep->get().for_each(DependenceImp::SummarizeFunctor<MemoryTraitSet>{
      Dep, &*EMTraitItr, mSE});
    LLVM_DEBUG(
      dbgs() << "[PRIVATE]: summarize dependence for ";
      printLocationSource(dbgs(), MemoryLocation(
        EMI->get<EstimateMemory>()->front(),
        EMI->get<EstimateMemory>()->getSize(),
        EMI->get<EstimateMemory>()->getAAInfo()));
      dbgs() << " ";
      bcl::TraitKey I(1);
      Dep->print(dbgs());
      dbgs() << " to ";
      EMTraitItr->print(dbgs());
      dbgs() << "\n";
    );
  };
  DependenceSet::iterator NodeTraitItr;
  auto EMI = Traits.get<TraitList>()->begin();
  auto EME = Traits.get<TraitList>()->end();
  if (!Traits.get<TraitList>()->empty()) {
    NodeTraitItr = DS.insert(&N, MemoryDescriptor()).first;
    auto SecondEM = Traits.get<TraitList>()->begin(); ++SecondEM;
    if (Traits.get<UnknownList>()->empty() && SecondEM == EME) {
      *NodeTraitItr = EMI->get<BitMemoryTrait>().toDescriptor(1, NumTraits);
      checkFirstPrivate(Numbers, R, EMI, *NodeTraitItr);
      auto ExplicitItr = ExplicitAccesses.find(EMI->get<EstimateMemory>());
      if (ExplicitItr != ExplicitAccesses.end() &&
          dropUnitFlag(*ExplicitItr->second) != BitMemoryTrait::NoAccess &&
          EMI->get<EstimateMemory>()->getAliasNode(*mAliasTree) == &N) {
        NodeTraitItr->set<trait::ExplicitAccess>();
        ++NumTraits.get<trait::ExplicitAccess>();
      }
      bcl::trait::unset<DependenceImp::Descriptor>(*NodeTraitItr);
      auto EMTraitItr = NodeTraitItr->insert(
        EstimateMemoryTrait(EMI->get<EstimateMemory>(), *NodeTraitItr)).first;
      if (dropUnitFlag(EMI->get<BitMemoryTrait>())
            == BitMemoryTrait::Dependency) {
        storeDepIfNeed(EMI, EMTraitItr);
        *NodeTraitItr = *EMTraitItr;
      }
      return;
    }
  } else if (!Traits.get<UnknownList>()->empty()) {
    NodeTraitItr = DS.insert(&N, MemoryDescriptor()).first;
  } else {
    return;
  }
  // There are memory locations which are explicitly accessed in the loop and
  // which are covered by estimate memory locations from different estimate
  // memory trees. So only three types of combined results are possible:
  // read-only, shared or dependency.
  BitMemoryTrait CombinedTrait;
  DependenceImp::Descriptor CombinedDepDptr;
  unsigned NumberOfCombined = 0;
  for (; EMI != EME; ++EMI) {
    CombinedTrait &= EMI->get<BitMemoryTrait>();
    if (dropUnitFlag(EMI->get<BitMemoryTrait>()) != BitMemoryTrait::NoAccess)
      ++NumberOfCombined;
    auto Dptr = EMI->get<BitMemoryTrait>().toDescriptor(0, NumTraits);
    checkFirstPrivate(Numbers, R, EMI, Dptr);
    auto ExplicitItr = ExplicitAccesses.find(EMI->get<EstimateMemory>());
    if (ExplicitItr != ExplicitAccesses.end() &&
        dropUnitFlag(*ExplicitItr->get<BitMemoryTrait>())
          != BitMemoryTrait::NoAccess &&
        EMI->get<EstimateMemory>()->getAliasNode(*mAliasTree) == &N) {
      NodeTraitItr->set<trait::ExplicitAccess>();
      Dptr.set<trait::ExplicitAccess>();
      ++NumTraits.get<trait::ExplicitAccess>();
    }
    bcl::trait::unset<DependenceImp::Descriptor>(Dptr);
    auto EMTraitItr = NodeTraitItr->insert(
      EstimateMemoryTrait(EMI->get<EstimateMemory>(), std::move(Dptr))).first;
    if (dropUnitFlag(EMI->get<BitMemoryTrait>()) == BitMemoryTrait::Dependency) {
      storeDepIfNeed(EMI, EMTraitItr);
      bcl::trait::set(*EMTraitItr, CombinedDepDptr);
    }
  }
  for (auto &U : *Traits.get<UnknownList>()) {
    CombinedTrait &= U.get<BitMemoryTrait>();
    if (dropUnitFlag(U.get<BitMemoryTrait>()) != BitMemoryTrait::NoAccess)
      ++NumberOfCombined;
    auto Dptr = U.get<BitMemoryTrait>().toDescriptor(0, NumTraits);
    auto ExplicitItr = ExplicitUnknowns.find(U.get<Instruction>());
    if (ExplicitItr != ExplicitUnknowns.end() &&
        dropUnitFlag(*ExplicitItr->get<BitMemoryTrait>())
          != BitMemoryTrait::NoAccess &&
        ExplicitItr->get<AliasNode>() == &N) {
      NodeTraitItr->set<trait::ExplicitAccess>();
      Dptr.set<trait::ExplicitAccess>();
      ++NumTraits.get<trait::ExplicitAccess>();
    }
    if (dropUnitFlag(U.get<BitMemoryTrait>()) == BitMemoryTrait::Dependency)
      bcl::trait::set<DependenceImp::Descriptor>(CombinedDepDptr);
    NodeTraitItr->insert(
      UnknownMemoryTrait(U.get<Instruction>(), std::move(Dptr)));
  }
  // It may be 0 or 1 because address accesses (with no access property) does
  // not considered.
  if (NumberOfCombined > 1) {
    auto OriginalTrait = CombinedTrait;
    CombinedTrait |= BitMemoryTrait::AllUnitFlags;
    CombinedTrait &=
      dropUnitFlag(OriginalTrait) == BitMemoryTrait::NoAccess ?
      BitMemoryTrait::NoAccess :
      dropUnitFlag(OriginalTrait) == BitMemoryTrait::Readonly ?
      BitMemoryTrait::Readonly :
      hasSharedJoin(OriginalTrait) ? BitMemoryTrait::Shared :
      BitMemoryTrait::Dependency;
  }
  if (NodeTraitItr->is<trait::ExplicitAccess>()) {
    *NodeTraitItr = CombinedTrait.toDescriptor(NodeTraitItr->count(), NumTraits);
      bcl::trait::unset<DependenceImp::Descriptor>(*NodeTraitItr);
      bcl::trait::set(CombinedDepDptr, *NodeTraitItr);
      NodeTraitItr->set<trait::ExplicitAccess>();
  } else {
    *NodeTraitItr = CombinedTrait.toDescriptor(NodeTraitItr->count(), NumTraits);
     bcl::trait::unset<DependenceImp::Descriptor>(*NodeTraitItr);
     bcl::trait::set(CombinedDepDptr, *NodeTraitItr);
  }
  LLVM_DEBUG(dbgs() << "[PRIVATE]: set combined trait to ";
    NodeTraitItr->print(dbgs()); dbgs() << "\n";);
}

void PrivateRecognitionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<DFRegionInfoPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<LiveMemoryPass>();
  AU.addRequiredTransitive<EstimateMemoryPass>();
  AU.addRequired<DependenceAnalysisWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.setPreservesAll();
}

namespace {
/// This functor stores representation of a trait in a static map as a string.
class TraitToStringFunctor {
public:
  /// Static map from trait to its string representation.
  typedef bcl::StaticTraitMap<
    std::string, MemoryDescriptor> TraitToStringMap;

  /// Creates the functor.
  TraitToStringFunctor(TraitToStringMap &Map, llvm::StringRef Offset,
    const llvm::DominatorTree &DT) : mMap(&Map), mOffset(Offset), mDT(&DT) {}

  /// Stores representation of a trait in a static map as a string.
  template<class Trait> void operator()() {
    assert(mTS && "Trait set must not be null!");
    raw_string_ostream OS(mMap->value<Trait>());
    OS << mOffset;
    for (auto &T : *mTS) {
      if (!std::is_same<Trait, trait::AddressAccess>::value &&
           T.is<trait::NoAccess>() ||
          std::is_same<Trait, trait::AddressAccess>::value && !T.is<Trait>())
        continue;
      printLocationSource(OS, *T.getMemory(), mDT);
      traitToStr(T.get<Trait>(), OS);
      OS << " ";
    }
    for (auto &T : make_range(mTS->unknown_begin(), mTS->unknown_end())) {
      if (!std::is_same<Trait, trait::AddressAccess>::value &&
           T.is<trait::NoAccess>() ||
          std::is_same<Trait, trait::AddressAccess>::value && !T.is<Trait>())
        continue;
      OS << "<";
      if (auto Callee = [&T]() -> llvm::Function * {
            if (auto *Call = dyn_cast<CallBase>(T.getMemory()))
              return dyn_cast<Function>(
                  Call->getCalledOperand()->stripPointerCasts());
            else
              return nullptr;
          }())
        Callee->printAsOperand(OS, false);
      else
        T.getMemory()->printAsOperand(OS, false);
      OS << "> ";
    }
    OS << "\n";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(const trait::IRDependence *Dep, raw_string_ostream &OS) {
    if (!Dep || Dep->getLevels() == 0)
      return;
    OS << ":[";
    unsigned I = 0, EI = Dep->getLevels();
    if (Dep->getDistance(I).first)
      Dep->getDistance(I).first->print(OS);
    OS << ":";
    if (Dep->getDistance(I).second)
      Dep->getDistance(I).second->print(OS);
    for (++I; I < EI; ++I) {
      OS << ",";
      if (Dep->getDistance(I).first)
        Dep->getDistance(I).first->print(OS);
      OS << ":";
      if (Dep->getDistance(I).second)
        Dep->getDistance(I).second->print(OS);
    }
    OS << "]";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(const void *Dep, raw_string_ostream &OS) {}

  /// Returns a static trait map.
  TraitToStringMap & getStringMap() { return *mMap; }

  /// \brief Returns current trait set.
  ///
  /// \pre Trait set must not be null and has been specified by setTraitSet().
  const AliasTrait & getTraitSet() const {
    assert(mTS && "Trait set must not be null!");
    return *mTS;
  }

  /// Specifies current trait set.
  void setTraitSet(const AliasTrait &TS) { mTS = &TS; }

private:
  TraitToStringMap *mMap;
  const AliasTrait *mTS;
  std::string mOffset;
  const DominatorTree *mDT;
};

/// Prints a static map from trait to its string representation to a specified
/// output stream.
class TraitToStringPrinter {
public:
  /// Creates functor.
  TraitToStringPrinter(llvm::raw_ostream &OS, llvm::StringRef Offset) :
    mOS(OS), mOffset(Offset) {}

  /// Prints a specified trait.
  template<class Trait> void operator()(llvm::StringRef Str) {
    if (Str.empty())
      return;
    mOS << mOffset << Trait::toString() << ":\n" << Str;
  }

private:
  llvm::raw_ostream &mOS;
  std::string mOffset;
};
}

void PrivateRecognitionPass::print(raw_ostream &OS, const Module *M) const {
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &RInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto *F = cast<DFFunction>(RInfo.getTopLevelRegion())->getFunction();
  if (!GlobalOpts.AnalyzeLibFunc && hasFnAttr(*F, AttrKind::LibFunc))
    return;
  for_each_loop(LpInfo, [this, &OS, &RInfo, &DT, &AT, &GlobalOpts](Loop *L) {
    DebugLoc Loc = L->getStartLoc();
    std::string Offset(L->getLoopDepth(), ' ');
    OS << Offset;
    OS << "loop at depth " << L->getLoopDepth() << " ";
    tsar::print(OS, Loc, GlobalOpts.PrintFilenameOnly);
    OS << "\n";
    auto N = RInfo.getRegionFor(L);
    auto &Info = getPrivateInfo();
    auto Itr = Info.find(N);
    assert(Itr != Info.end() && "Privatiability information must be specified!");
    TraitToStringFunctor::TraitToStringMap TraitToStr;
    TraitToStringFunctor ToStrFunctor(TraitToStr, Offset + "  ", DT);
    auto ATRoot = AT.getTopLevelNode();
    for (auto &TS : Itr->get<DependenceSet>()) {
      if (TS.getNode() == ATRoot)
        continue;
      ToStrFunctor.setTraitSet(TS);
      TS.for_each(ToStrFunctor);
    }
    TraitToStr.for_each(TraitToStringPrinter(OS, Offset + " "));
  });
}

FunctionPass *llvm::createPrivateRecognitionPass() {
  return new PrivateRecognitionPass();
}
