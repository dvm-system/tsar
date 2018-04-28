//===--- tsar_private.cpp - Private Variable Analyzer -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to analyze variables which can be privatized.
//
//===----------------------------------------------------------------------===//

#include "tsar_private.h"
#include "tsar_dbg_output.h"
#include "DefinedMemory.h"
#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "tsar_graph.h"
#include "LiveMemory.h"
#include "MemoryCoverage.h"
#include "MemoryTraitUtils.h"
#include "MemoryAccessUtils.h"
#include "tsar_utility.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/DependenceAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <utility.h>

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private"

STATISTIC(NumPrivate, "Number of private locations found");
STATISTIC(NumLPrivate, "Number of last private locations found");
STATISTIC(NumSToLPrivate, "Number of second to last private locations found");
STATISTIC(NumDPrivate, "Number of dynamic private locations found");
STATISTIC(NumFPrivate, "Number of first private locations found");
STATISTIC(NumDeps, "Number of unsorted dependencies found");
STATISTIC(NumReadonly, "Number of read-only locations found");
STATISTIC(NumShared, "Number of shared locations found");
STATISTIC(NumAddressAccess, "Number of locations address of which is evaluated");
STATISTIC(NumHeaderAccess, "Number of traits caused by memory accesses in a loop header");

char PrivateRecognitionPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateRecognitionPass, "private",
                      "Private Variable Analysis", false, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(LiveMemoryPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(PrivateRecognitionPass, "private",
                    "Private Variable Analysis", false, true)

bool PrivateRecognitionPass::runOnFunction(Function &F) {
  releaseMemory();
#ifdef DEBUG
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
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  GraphNumbering<const AliasNode *> Numbers;
  numberGraph(mAliasTree, &Numbers);
  resolveCandidats(Numbers, DFF);
  return false;
}

namespace tsar {
namespace detail {
/// \brief Identifiers of recognized traits.
///
/// This is a helpful enumeration which must not be used outside the private
/// recognition pass. It is easy to join different traits. For example,
/// Readonly & LastPrivate = 0011001 = LastPrivate & FirstPrivate. So if some
/// part of memory locations is read-only and other part is last private a union
/// is last private and first private (for details see resolve... methods).
enum TraitId : unsigned long long {
  NoAccess = 11111111_b,
  Readonly = 11110111_b,
  Shared = 11110011_b,
  Private = 01111111_b,
  FirstPrivate = 01110111_b,
  SecondToLastPrivate = 01011111_b,
  LastPrivate = 00111111_b,
  DynamicPrivate = 00011111_b,
  Dependency = 00000011_b,
  AddressAccess = 11111101_b,
  HeaderAccess = 11111110_b,
};

constexpr inline TraitId operator&(TraitId LHS, TraitId RHS) noexcept {
  return static_cast<TraitId>(
    static_cast<std::underlying_type<TraitId>::type>(LHS) &
    static_cast<std::underlying_type<TraitId>::type>(RHS));
}
constexpr inline TraitId operator|(TraitId LHS, TraitId RHS) noexcept {
  return static_cast<TraitId>(
    static_cast<std::underlying_type<TraitId>::type>(LHS) |
    static_cast<std::underlying_type<TraitId>::type>(RHS));
}
constexpr inline TraitId operator~(TraitId What) noexcept {
  // We use `... & NoAccess` to avoid reversal of unused bits.
  return static_cast<TraitId>(
    ~static_cast<std::underlying_type<TraitId>::type>(What) & NoAccess);
}

/// Drops bits which identifies single-bit traits.
static inline TraitId dropUnitFlag(TraitId T) noexcept {
  return T | ~AddressAccess | ~HeaderAccess;
}

/// Drops a single bit which identifies shared trait (shared becomes read-only).
static inline TraitId dropSharedFlag(TraitId T) noexcept {
  return T | ~(~Readonly | Shared);
}

/// Internal representation of traits of memory locations.
class TraitImp {
public:
  TraitImp() = default;
  TraitImp(TraitId Id) noexcept : mId(static_cast<decltype(mId)>(Id)) {}
  TraitImp & operator=(TraitId Id) noexcept { return *this = TraitImp(Id); }
  TraitImp & operator&=(const TraitImp &With) noexcept {
    mId &= With.mId;
    return *this;
  }
  TraitImp & operator|=(const TraitImp &With) noexcept {
    mId != With.mId;
    return *this;
  }
  bool operator!() const noexcept { return !mId; }
  operator TraitId () const noexcept{ return get(); }
  TraitId get() const noexcept { return static_cast<TraitId>(mId); }
private:
  std::underlying_type<TraitId>::type mId = NoAccess;
};

/// Internal representation of loop-carried dependencies.
class DependenceImp {
  friend struct UpdateFunctor;
  friend struct DumpFunctor;
  friend struct SummarizeFunctor;

public:
  using Distances = SmallPtrSet<const SCEV *, 4>;
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
      trait::IRDependence::DistanceRange Dist(nullptr, nullptr);
      SmallVector<const SCEV *, 4> MaxOps(mDep->mDists.get<Trait>().size());
      SmallVector<const SCEV *, 4> MinOps(mDep->mDists.get<Trait>().size());
      if (!(mDep->mFlags.get<Trait>() & trait::Dependence::UnknownDistance)) {
        std::size_t Idx = 0;
        for (auto D : mDep->mDists.get<Trait>()) {
          MaxOps[Idx] = mSE->getSMaxExpr(mSE->getNegativeSCEV(D), D);
          MinOps[Idx] = mSE->getNotSCEV(MaxOps[Idx]);
          ++Idx;
        }
        Dist.first = mSE->getNotSCEV(mSE->getUMaxExpr(MinOps));
        Dist.second = mSE->getUMaxExpr(MaxOps);
      }
      mSet->template set<Trait>(
        new trait::IRDependence(mDep->mFlags.get<Trait>(), Dist));
    }
    DependenceImp *mDep;
    TraitSet *mSet;
    ScalarEvolution *mSE;
  };

  /// Returns descriptor.
  const Descriptor & get() const noexcept { return mDptr; }

  /// Uses specified descriptor, flags, and distance to update
  /// information about dependencies (see UpdateFunctor for details).
  void update(Descriptor Dptr, trait::Dependence::Flag F, const SCEV * Dist) {
    Dptr.for_each(UpdateFunctor{ this, F, Dist });
  }

  /// Uses specified dependence description to update underlying
  /// information about dependencies (see UpdateFunctor for details).
  void update(DependenceImp &Dep) {
    Dep.mDptr.for_each(UpdateDepFunctor{ this, &Dep });
  }

  /// Print information about dependencies.
  void print(raw_ostream &OS) { mDptr.for_each(DumpFunctor{ this, OS }); }

  /// Print information about dependencies.
  void dump() { print(dbgs()); dbgs() << "\n"; }

private:
  /// This functor updates specified dependence description mDep.
  struct UpdateFunctor {
    template<class Trait> void operator()() {
      mDep->mDptr.set<Trait>();
      if (!mDist)
        mFlag |= trait::Dependence::UnknownDistance;
      mDep->mFlags.get<Trait>() |= mFlag;
      if (!(mDep->mFlags.get<Trait>() & trait::Dependence::UnknownDistance))
        mDep->mDists.get<Trait>().insert(mDist);
      else
        mDep->mDists.get<Trait>().clear();
    }
    DependenceImp *mDep;
    trait::Dependence::Flag mFlag;
    const SCEV *mDist;
  };

  /// This functor updates specified dependence description mDep.
  struct UpdateDepFunctor {
    template<class Trait> void operator()() {
      mDep->mDptr.set<Trait>();
      mDep->mFlags.get<Trait>() |= mSrc->mFlags.get<Trait>();
      if (!(mDep->mFlags.get<Trait>() & trait::Dependence::UnknownDistance))
        mDep->mDists.get<Trait>().insert(
          mSrc->mDists.get<Trait>().begin(), mSrc->mDists.get<Trait>().end());
      else
        mDep->mDists.get<Trait>().clear();
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
      for (const SCEV *D : mDep->mDists.get<Trait>()) {
        mOS << " ";
        D->print(mOS);
      }
      mOS << " }}";
    }
    DependenceImp *mDep;
    raw_ostream &mOS;
  };

  Descriptor mDptr;
  bcl::tagged_tuple<
    bcl::tagged<Distances, trait::Flow>,
    bcl::tagged<Distances, trait::Anti>,
    bcl::tagged<Distances, trait::Output>> mDists;
  bcl::tagged_tuple<
    bcl::tagged<trait::Dependence::Flag, trait::Flow>,
    bcl::tagged<trait::Dependence::Flag, trait::Anti>,
    bcl::tagged<trait::Dependence::Flag, trait::Output>> mFlags;
};
}
}

#ifndef NDEBUG
static void updateTraitsLog(const EstimateMemory *EM, TraitImp T) {
  dbgs() << "[PRIVATE]: update traits of ";
  printLocationSource(
    dbgs(), MemoryLocation(EM->front(), EM->getSize(), EM->getAAInfo()));
  dbgs() << " to ";
  bcl::bitPrint(T, dbgs());
  dbgs() << "\n";
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
    const SCEV *Dist, MapTy &Deps) {
  assert(EM && "Estimate memory location must not be null!");
  auto Itr = Deps.try_emplace(EM, nullptr).first;
  if (!Itr->template get<DependenceImp>())
    Itr->template get<DependenceImp>().reset(new DependenceImp);
  Itr->template get<DependenceImp>()->update(Dptr, F, Dist);
  DEBUG(updateDependenceLog(*EM, *Itr->template get<DependenceImp>()));
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
static inline void mergeDependence(const EstimateMemory *To, TraitId ToTrait,
    const EstimateMemory *From, MapTy &Deps) {
  assert(To && "Estimate memory must not be null!");
  assert(From && "Estimate memory must not be null!");
  auto FromItr = Deps.find(From);
  DependenceImp *FromDep = nullptr;
  if (FromItr != Deps.end()) {
    FromDep = FromItr->template get<DependenceImp>().get();
    assert(FromDep &&
      "Location is stored in dependence map without dependence description!");
  } else if (dropUnitFlag(ToTrait) != Dependency) {
    return;
  }
  auto ToItr = Deps.try_emplace(To, nullptr).first;
  if (!ToItr->template get<DependenceImp>())
    ToItr->template get<DependenceImp>().reset(new DependenceImp);
  if (FromDep)
    ToItr->template get<DependenceImp>()->update(*FromDep);
  DEBUG(updateDependenceLog(*To, *ToItr->template get<DependenceImp>()));
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

void PrivateRecognitionPass::collectHeaderAccesses(Loop *L,
    TraitMap &ExplicitAccesses, UnknownMap &ExplicitUnknowns) {
  assert(L && "Loop must not be null!");
  for (auto &I : *L->getHeader()) {
    if (!I.mayReadOrWriteMemory())
      continue;
    for_each_memory(I, *mTLI,
      [this, &ExplicitAccesses](Instruction &, MemoryLocation &&Loc,
          unsigned, AccessInfo, AccessInfo) {
        auto *EM = mAliasTree->find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        auto Itr = ExplicitAccesses.find(EM);
        assert(Itr != ExplicitAccesses.end() &&
          "Explicitly accessed memory must be stored in a list of explicit accesses!");
        *Itr->get<TraitImp>() &= HeaderAccess;
      },
      [this, &ExplicitUnknowns](Instruction &I, AccessInfo, AccessInfo) {
        auto Itr = ExplicitUnknowns.find(&I);
        assert(Itr != ExplicitUnknowns.end() &&
          "Explicitly accessed memory must be stored in a list of explicit accesses!");
        *Itr->get<TraitImp>() &= HeaderAccess;
      });
  }
}

void PrivateRecognitionPass::resolveCandidats(
    const GraphNumbering<const AliasNode *> &Numbers, DFRegion *R) {
  assert(R && "Region must not be null!");
  if (auto *L = dyn_cast<DFLoop>(R)) {
    DEBUG(
      dbgs() << "[PRIVATE]: analyze "; L->getLoop()->dump();
      if (DebugLoc DbgLoc = L->getLoop()->getStartLoc()) {
        dbgs() << " at ";
        DbgLoc.print(dbgs());
      }
      dbgs() << "\n";
    );
    auto PrivInfo = mPrivates.insert(
      std::make_pair(L, llvm::make_unique<DependencySet>(*mAliasTree)));
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
    collectDependencies(L->getLoop(), Deps);
    resolveAccesses(R->getLatchNode(), R->getExitNode(),
      *DefItr->get<DefUseSet>(), *LiveItr->get<LiveSet>(), Deps,
      ExplicitAccesses, ExplicitUnknowns, NodeTraits);
    collectHeaderAccesses(L->getLoop(), ExplicitAccesses, ExplicitUnknowns);
    resolvePointers(*DefItr->get<DefUseSet>(), ExplicitAccesses);
    resolveAddresses(L, *DefItr->get<DefUseSet>(), ExplicitAccesses, NodeTraits);
    propagateTraits(Numbers, *R, ExplicitAccesses, ExplicitUnknowns, NodeTraits,
      Deps, *PrivInfo.first->get<DependencySet>());
  }
  for (auto I = R->region_begin(), E = R->region_end(); I != E; ++I)
    resolveCandidats(Numbers, *I);
}

void PrivateRecognitionPass::insertDependence(const Dependence &Dep,
    const MemoryLocation &Src, const MemoryLocation Dst,
    trait::Dependence::Flag Flag, Loop &L, DependenceMap &Deps) {
  auto Dir = Dep.getDirection(L.getLoopDepth());
  if (Dir == Dependence::DVEntry::EQ) {
    DEBUG(dbgs() << "[PRIVATE]: ignore loop independent dependence\n");
    return;
  }
  assert((Dep.isOutput() || Dep.isAnti() || Dep.isFlow()) &&
    "Unknown kind of dependency!");
  DependenceImp::Descriptor Dptr;
  if (Dep.isOutput())
    Dptr.set<trait::Output>();
  else if (Dir == Dependence::DVEntry::ALL)
    Dptr.set<trait::Flow, trait::Anti>();
  else if (Dep.isFlow())
    if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE)
      Dptr.set<trait::Flow>();
    else
      Dptr.set<trait::Anti>();
  else if (Dep.isAnti())
    if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE)
      Dptr.set<trait::Anti>();
    else
      Dptr.set<trait::Flow>();
  else
    Dptr.set<trait::Flow, trait::Anti>();
  auto Dist = Dep.getDistance(L.getLoopDepth());
  updateDependence(mAliasTree->find(Src),
    Dptr, trait::Dependence::LoadStoreCause | Flag, Dist, Deps);
  updateDependence(mAliasTree->find(Dst),
    Dptr, trait::Dependence::LoadStoreCause | Flag, Dist, Deps);
}

void PrivateRecognitionPass::collectDependencies(Loop *L, DependenceMap &Deps) {
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
      ImmutableCallSite SrcCS(*SrcItr);
      for (auto DstItr = SrcItr; DstItr != EndItr; ++DstItr) {
        if (!(**DstItr).mayReadOrWriteMemory())
          continue;
        ImmutableCallSite DstCS(*DstItr);
        trait::Dependence::Flag Flag = trait::Dependence::May |
          trait::Dependence::UnknownDistance |
          (!SrcCS && !DstCS ? trait::Dependence::CallCause :
            trait::Dependence::UnknownCause);
        DependenceImp::Descriptor Dptr;
        Dptr.set<trait::Flow, trait::Anti, trait::Output>();
        auto insertUnknownDep =
          [this, &AA, &SrcItr, &DstItr, &Dptr, Flag, &Deps](Instruction &,
            MemoryLocation &&Loc, unsigned, AccessInfo, AccessInfo) {
          if (AA.getModRefInfo(*SrcItr, Loc) == MRI_NoModRef)
            return;
          if (AA.getModRefInfo(*DstItr, Loc) == MRI_NoModRef)
            return;
          updateDependence(mAliasTree->find(Loc), Dptr, Flag, nullptr, Deps);
        };
        auto stab = [](Instruction &, AccessInfo, AccessInfo) {};
        for_each_memory(**SrcItr, *mTLI, insertUnknownDep, stab);
        for_each_memory(**DstItr, *mTLI, insertUnknownDep, stab);
      }
    } else {
      for (auto DstItr = SrcItr; DstItr != EndItr; ++DstItr) {
        auto Dst = getLoadOrStoreLocation(*DstItr);
        if (!Dst.Ptr)
          continue;
        if (auto D = mDepInfo->depends(*SrcItr, *DstItr, true)) {
          DEBUG(
            dbgs() << "[PRIVATE]: dependence found: ";
            D->dump(dbgs());
            (**SrcItr).dump();
            (**DstItr).dump();
          );
          if (!D->isAnti() && !D->isFlow() && !D->isOutput()) {
            DEBUG(dbgs() << "[PRIVATE]: ignore input dependence\n");
            continue;
          }
          // Do not use Dependence::isLoopIndependent() to check loop
          // independent dependencies. This method returns `may` instead of
          // `must`. This means that if it returns `true` than dependency
          // may be loop-carried or may arise inside a single iteration.
          insertDependence(*D, Src, Dst, trait::Dependence::No, *L, Deps);
        }
      }
    }
  }
}

void PrivateRecognitionPass::resolveAccesses(const DFNode *LatchNode,
    const DFNode *ExitNode, const tsar::DefUseSet &DefUse,
    const tsar::LiveSet &LS, const DependenceMap &Deps,
    TraitMap &ExplicitAccesses,  UnknownMap &ExplicitUnknowns,
    AliasMap &NodeTraits) {
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
      I->get<TraitList>().push_front(std::make_pair(Base, TraitImp()));
      Pair.first->get<TraitImp>() =
        &I->get<TraitList>().front().get<TraitImp>();
    }
    auto &CurrTraits = *Pair.first->get<TraitImp>();
    TraitImp SharedTrait = !Deps.count(Base) ? Shared : NoAccess;
    if (!DefUse.hasUse(Loc)) {
      if (!LS.getOut().overlap(Loc))
        CurrTraits &= Private & SharedTrait;
      else if (DefUse.hasDef(Loc))
        CurrTraits &= LastPrivate & SharedTrait;
      else if (LatchDefs.MustReach.contain(Loc) &&
        !ExitingDefs.MayReach.overlap(Loc))
        // These location will be stored as second to last private, i.e.
        // the last definition of these locations is executed on the
        // second to the last loop iteration (on the last iteration the
        // loop condition check is executed only).
        // It is possible that there is only one (last) iteration in
        // the loop. In this case the location has not been assigned and
        // must be declared as a first private.
        CurrTraits &= SecondToLastPrivate & FirstPrivate & SharedTrait;
      else
        // There is no certainty that the location is always assigned
        // the value in the loop. Therefore, it must be declared as a
        // first private, to preserve the value obtained before the loop
        // if it has not been assigned.
        CurrTraits &= DynamicPrivate & FirstPrivate & SharedTrait;
    } else if ((DefUse.hasMayDef(Loc) || DefUse.hasDef(Loc)) &&
        SharedTrait == NoAccess) {
      CurrTraits &= Dependency;
    } else {
      CurrTraits &= Readonly;
    }
    DEBUG(updateTraitsLog(Base, CurrTraits));
  }
  for (const auto &Unknown : DefUse.getExplicitUnknowns()) {
    const auto N = mAliasTree->findUnknown(Unknown);
    assert(N && "Alias node for unknown memory location must not be null!");
    auto I = NodeTraits.find(N);
    auto &AA = mAliasTree->getAliasAnalysis();
    ImmutableCallSite CS(Unknown);
    TraitId TID = (CS && AA.onlyReadsMemory(CS)) ?
      TraitId::Readonly : TraitId::Dependency;
    I->get<UnknownList>().push_front(std::make_pair(Unknown, TraitImp(TID)));
    ExplicitUnknowns.insert(std::make_pair(Unknown,
      std::make_tuple(N, &I->get<UnknownList>().front().get<TraitImp>())));
  }
}

void PrivateRecognitionPass::resolvePointers(
    const tsar::DefUseSet &DefUse, TraitMap &ExplicitAccesses) {
  for (const auto &Loc : DefUse.getExplicitAccesses()) {
    // *p means that address of location should be loaded from p using 'load'.
    if (auto *LI = dyn_cast<LoadInst>(Loc.Ptr)) {
      auto *EM = mAliasTree->find(Loc);
      assert(EM && "Estimate memory location must not be null!");
      auto LocTraits = ExplicitAccesses.find(EM);
      assert(LocTraits != ExplicitAccesses.end() &&
        "Traits of location must be initialized!");
      if (dropSharedFlag(dropUnitFlag(*LocTraits->get<TraitImp>())) == Private ||
          dropUnitFlag(*LocTraits->get<TraitImp>()) == Readonly ||
          dropUnitFlag(*LocTraits->get<TraitImp>()) == Shared)
        continue;
      const EstimateMemory *Ptr = mAliasTree->find(MemoryLocation::get(LI));
      assert(Ptr && "Estimate memory location must not be null!");
      auto PtrTraits = ExplicitAccesses.find(Ptr);
      assert(PtrTraits != ExplicitAccesses.end() &&
        "Traits of location must be initialized!");
      if (dropUnitFlag(*PtrTraits->get<TraitImp>()) == Readonly)
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
      *LocTraits->get<TraitImp>() &= Dependency;
    }
  }
}

void PrivateRecognitionPass::resolveAddresses(DFLoop *L,
    const DefUseSet &DefUse, TraitMap &ExplicitAccesses, AliasMap &NodeTraits) {
  assert(L && "Loop must not be null!");
  for (Value *Ptr : DefUse.getAddressAccesses()) {
    const EstimateMemory* Base = mAliasTree->find(MemoryLocation(Ptr, 0));
    assert(Base && "Estimate memory location must not be null!");
    auto Root = Base->getTopLevelParent();
    // Do not remember an address:
    // * if it is stored in some location, for example
    // isa<LoadInst>(Root->front()), locations are analyzed separately;
    // * if it points to a temporary location and should not be analyzed:
    // for example, a result of a call can be a pointer.
    if (!isa<AllocaInst>(Root->front()) && !isa<GlobalVariable>(Root->front()))
      continue;
    Loop *Lp = L->getLoop();
    // If this is an address of a location declared in the loop do not
    // remember it.
    if (auto AI = dyn_cast<AllocaInst>(Root->front()))
      if (Lp->contains(AI->getParent()))
        continue;
    for (auto User : Ptr->users()) {
      auto UI = dyn_cast<Instruction>(User);
      if (!UI || !Lp->contains(UI->getParent()))
        continue;
      // The address is used inside the loop.
      // Remember it if it is used for computation instead of memory access or
      // if we do not know how it will be used.
      if (isa<PtrToIntInst>(User) ||
          (isa<StoreInst>(User) &&
          cast<StoreInst>(User)->getValueOperand() == Ptr)) {
        auto Pair = ExplicitAccesses.insert(std::make_pair(Base, nullptr));
        if (!Pair.second) {
          *Pair.first->get<TraitImp>() &= AddressAccess;
        } else {
          auto I = NodeTraits.find(Base->getAliasNode(*mAliasTree));
          I->get<TraitList>().push_front(
            std::make_pair(Base, TraitImp(NoAccess & AddressAccess)));
          Pair.first->get<TraitImp>() =
            &I->get<TraitList>().front().get<TraitImp>();
        }
        ++NumAddressAccess;
        break;
      }
    }
  }
}

void PrivateRecognitionPass::propagateTraits(
    const tsar::GraphNumbering<const AliasNode *> &Numbers,
    const tsar::DFRegion &R,
    TraitMap &ExplicitAccesses, UnknownMap &ExplicitUnknowns,
    AliasMap &NodeTraits, DependenceMap &Deps, DependencySet &DS) {
  DEBUG(dbgs() << "[PRIVATE]: propagate traits\n");
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
              *EA->get<TraitImp>() &= EMToT.get<TraitImp>();
              mergeDependence(Parent, *EA->get<TraitImp>(),
                EMToT.get<EstimateMemory>(), Deps);
            } else
              mergeDependence(Parent, EMToT.get<TraitImp>(),
                EMToT.get<EstimateMemory>(), Deps);
              NTItr->get<TraitList>().push_front(
                std::make_pair(Parent, std::move(EMToT.get<TraitImp>())));
          }
        }
        for (auto &UToT : *CT.get<UnknownList>())
          NTItr->get<UnknownList>().push_front(std::move(UToT));
      }
    }
    auto &TL = NTItr->get<TraitList>();
    DEBUG(removeRedundantLog(TL, "before"));
    for (auto BI = TL.before_begin(), I = TL.begin(), E = TL.end(); I != E;)
      removeRedundant(N, NTItr->get<TraitList>(), BI, I, Deps);
    DEBUG(removeRedundantLog(TL, "after"));
    TraitPair NT(&NTItr->get<TraitList>(), &NTItr->get<UnknownList>());
    storeResults(
      Numbers, R, *N, ExplicitAccesses, ExplicitUnknowns, Deps, NT, DS);
    ChildTraits.push(std::move(NT));
    Prev = N;
  }
  std::vector<const AliasNode *> Coverage;
  explicitAccessCoverage(DS, *mAliasTree, Coverage);
  // All descendant nodes for nodes in `Coverage` access some part of
  // explicitly accessed memory. The conservativeness of analysis implies
  // that memory accesses from this nodes arise loop carried dependencies.
  for (auto *N : Coverage)
    for (auto &Child : make_range(N->child_begin(), N->child_end()))
      for (auto *Descendant : make_range(df_begin(&Child), df_end(&Child))) {
        auto I = DS.find(Descendant);
        if (I != DS.end() && !I->is<trait::NoAccess>())
          I->set<trait::Flow, trait::Anti, trait::Output>();
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
  TraitItr->get<TraitImp>() &= FirstPrivate;
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
    mergeDependence(Current, CurrItr->get<TraitImp>(),
      CurrItr->get<EstimateMemory>(), Deps);
    CurrItr->get<EstimateMemory>() = Current;
  }
  for (++I; I != E;) {
    if (Current == I->get<EstimateMemory>()) {
      I->get<TraitImp>() &= CurrItr->get<TraitImp>();
      CurrItr = Traits.erase_after(BeforeCurrItr);
      return;
    }
    auto Ancestor = ancestor(Current, I->get<EstimateMemory>());
    if (Ancestor == I->get<EstimateMemory>()) {
      I->get<TraitImp>() &= CurrItr->get<TraitImp>();
      mergeDependence(
        I->get<EstimateMemory>(), I->get<TraitImp>(), Current, Deps);
      CurrItr = Traits.erase_after(BeforeCurrItr);
      return;
    }
    if (Ancestor == Current) {
      CurrItr->get<TraitImp>() &= I->get<TraitImp>();
      mergeDependence(
        Current, CurrItr->get<TraitImp>(), I->get<EstimateMemory>(), Deps);
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
    const DependenceMap &Deps, const TraitPair &Traits, DependencySet &DS) {
  assert(DS.find(&N) == DS.end() && "Results must not be already stored!");
  auto storeDepIfNeed = [this, &Deps](TraitList::iterator EMI,
      AliasTrait::iterator EMTraitItr) {
    auto EMToDep = Deps.find(EMI->get<EstimateMemory>());
    assert(EMToDep != Deps.end() &&
      "Dependence must be presented in the map!");
    auto Dep = EMToDep->get<DependenceImp>().get();
    Dep->get().for_each(DependenceImp::SummarizeFunctor<MemoryTraitSet>{
      Dep, &*EMTraitItr, mSE});
    DEBUG(
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
  DependencySet::iterator NodeTraitItr;
  auto EMI = Traits.get<TraitList>()->begin();
  auto EME = Traits.get<TraitList>()->end();
  if (!Traits.get<TraitList>()->empty()) {
    NodeTraitItr = DS.insert(&N, MemoryDescriptor()).first;
    auto SecondEM = Traits.get<TraitList>()->begin(); ++SecondEM;
    if (Traits.get<UnknownList>()->empty() && SecondEM == EME) {
      *NodeTraitItr = toDescriptor(EMI->get<TraitImp>(), 1);
      checkFirstPrivate(Numbers, R, EMI, *NodeTraitItr);
      auto ExplicitItr = ExplicitAccesses.find(EMI->get<EstimateMemory>());
      if (ExplicitItr != ExplicitAccesses.end() &&
          dropUnitFlag(*ExplicitItr->second) != NoAccess &&
          EMI->get<EstimateMemory>()->getAliasNode(*mAliasTree) == &N)
        NodeTraitItr->set<trait::ExplicitAccess>();
      bcl::trait::unset<DependenceImp::Descriptor>(*NodeTraitItr);
      auto EMTraitItr = NodeTraitItr->insert(
        EstimateMemoryTrait(EMI->get<EstimateMemory>(), *NodeTraitItr)).first;
      if (dropUnitFlag(EMI->get<TraitImp>()) == Dependency) {
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
  TraitImp CombinedTrait;
  DependenceImp::Descriptor CombinedDepDptr;
  for (; EMI != EME; ++EMI) {
    CombinedTrait &= EMI->get<TraitImp>();
    auto Dptr = toDescriptor(EMI->get<TraitImp>(), 0);
    checkFirstPrivate(Numbers, R, EMI, Dptr);
    auto ExplicitItr = ExplicitAccesses.find(EMI->get<EstimateMemory>());
    if (ExplicitItr != ExplicitAccesses.end() &&
        dropUnitFlag(*ExplicitItr->get<TraitImp>()) != NoAccess &&
        EMI->get<EstimateMemory>()->getAliasNode(*mAliasTree) == &N) {
      NodeTraitItr->set<trait::ExplicitAccess>();
      Dptr.set<trait::ExplicitAccess>();
    }
    bcl::trait::unset<DependenceImp::Descriptor>(Dptr);
    auto EMTraitItr = NodeTraitItr->insert(
      EstimateMemoryTrait(EMI->get<EstimateMemory>(), std::move(Dptr))).first;
    if (dropUnitFlag(EMI->get<TraitImp>()) == Dependency) {
      storeDepIfNeed(EMI, EMTraitItr);
      bcl::trait::set(*EMTraitItr, CombinedDepDptr);
    }
  }
  for (auto &U : *Traits.get<UnknownList>()) {
    CombinedTrait &= U.get<TraitImp>();
    auto Dptr = toDescriptor(U.get<TraitImp>(), 0);
    auto ExplicitItr = ExplicitUnknowns.find(U.get<Instruction>());
    if (ExplicitItr != ExplicitUnknowns.end() &&
        dropUnitFlag(*ExplicitItr->get<TraitImp>()) != NoAccess &&
        ExplicitItr->get<AliasNode>() == &N) {
      NodeTraitItr->set<trait::ExplicitAccess>();
      Dptr.set<trait::ExplicitAccess>();
    }
    if (dropUnitFlag(U.get<TraitImp>()) == Dependency)
      bcl::trait::set<DependenceImp::Descriptor>(CombinedDepDptr);
    NodeTraitItr->insert(
      UnknownMemoryTrait(U.get<Instruction>(), std::move(Dptr)));
  }
  CombinedTrait &=
    dropUnitFlag(CombinedTrait) == Readonly ? Readonly :
      dropUnitFlag(CombinedTrait) == Shared ? Shared : Dependency;
  if (NodeTraitItr->is<trait::ExplicitAccess>()) {
    *NodeTraitItr = toDescriptor(CombinedTrait, NodeTraitItr->count());
      bcl::trait::unset<DependenceImp::Descriptor>(*NodeTraitItr);
      bcl::trait::set(CombinedDepDptr, *NodeTraitItr);
      NodeTraitItr->set<trait::ExplicitAccess>();
  } else {
    *NodeTraitItr = toDescriptor(CombinedTrait, NodeTraitItr->count());
     bcl::trait::unset<DependenceImp::Descriptor>(*NodeTraitItr);
     bcl::trait::set(CombinedDepDptr, *NodeTraitItr);
  }
  DEBUG(dbgs() << "[PRIVATE]: set combined trait to ";
    NodeTraitItr->print(dbgs()); dbgs() << "\n";);
  /// Due to conservativeness of analysis type of dependencies must be the
  /// same for all locations in the node.
  /// Let us consider an example.
  /// for (...) X[...] = Y[...];
  /// Analysis can not be performed accurately if X and Y may alias.
  /// Dependence analysis pass tests the following pairs of accesses:
  /// W(X)-W(X), W(X)-R(Y), R(Y)-R(Y) (W means 'write' and R means 'read').
  /// So, if X produces 'output' dependence there is no way to understand that
  /// Y is also produced 'output' dependence (due to memory overlapping). Then
  /// it is necessary to iterate over all accessed locations and to update their
  /// traits.
  for (auto &T : *NodeTraitItr) {
    bcl::trait::set(CombinedDepDptr, T);
    DEBUG(
      dbgs() << "[PRIVATE]: conservatively update trait of ";
      printLocationSource(dbgs(), MemoryLocation(T.getMemory()->front(),
        T.getMemory()->getSize(), T.getMemory()->getAAInfo()));
      dbgs() << " to "; T.get().print(dbgs()); dbgs() << "\n";
    );
  }
}

MemoryDescriptor PrivateRecognitionPass::toDescriptor(
    const TraitImp &T, unsigned TraitNumber) {
  MemoryDescriptor Dptr;
  if (!(T & ~AddressAccess)) {
    Dptr.set<trait::AddressAccess>();
    NumAddressAccess += TraitNumber;
  }
  if (!(T & ~HeaderAccess)) {
    Dptr.set<trait::HeaderAccess>();
    NumHeaderAccess += TraitNumber;
  }
  if (dropUnitFlag(T) == Dependency) {
    Dptr.set<trait::Flow, trait::Anti, trait::Output>();
    NumDeps += TraitNumber;
    return Dptr;
  }
  switch (dropUnitFlag(dropSharedFlag(T))) {
  default:
    llvm_unreachable("Unknown type of memory location dependency!");
    break;
  case NoAccess: Dptr.set<trait::NoAccess>(); break;
  case Readonly: Dptr.set<trait::Readonly>(); NumReadonly += TraitNumber; break;
  case Private: Dptr.set<trait::Private>(); NumPrivate += TraitNumber; break;
  case FirstPrivate:
    Dptr.set<trait::FirstPrivate>(); NumFPrivate += TraitNumber;
    break;
  case FirstPrivate & LastPrivate:
    Dptr.set<trait::FirstPrivate>(); NumFPrivate += TraitNumber;
  case LastPrivate:
    Dptr.set<trait::LastPrivate>(); NumLPrivate += TraitNumber;
    break;
  case FirstPrivate & SecondToLastPrivate:
    Dptr.set<trait::FirstPrivate>(); NumFPrivate += TraitNumber;
  case SecondToLastPrivate:
    Dptr.set<trait::SecondToLastPrivate>(); NumSToLPrivate +=TraitNumber;
    break;
  case FirstPrivate & DynamicPrivate:
    Dptr.set<trait::FirstPrivate>(); NumFPrivate += TraitNumber;
  case DynamicPrivate:
    Dptr.set<trait::DynamicPrivate>(); NumDPrivate += TraitNumber;
    break;
  }
  // If shared is one of traits it has been set as read-only in `switch`.
  // Hence, do not move this condition before `switch` because it should
  // override read-only if necessary.
  if (!(T &  ~(~Readonly | Shared))) {
    Dptr.set<trait::Shared>();
    NumShared += TraitNumber;
  }
  return Dptr;
}

void PrivateRecognitionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<LiveMemoryPass>();
  AU.addRequired<EstimateMemoryPass>();
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
      OS << "<";
      printLocationSource(OS, T.getMemory()->front(), mDT);
      OS << ", ";
      if (T.getMemory()->getSize() == MemoryLocation::UnknownSize)
        OS << "?";
      else
        OS << T.getMemory()->getSize();
      OS << ">";
      traitToStr(T.get<Trait>(), OS);
      OS << " ";
    }
    for (auto T : make_range(mTS->unknown_begin(), mTS->unknown_end())) {
      if (!std::is_same<Trait, trait::AddressAccess>::value &&
           T.is<trait::NoAccess>() ||
          std::is_same<Trait, trait::AddressAccess>::value && !T.is<Trait>())
        continue;
      OS << "<";
      ImmutableCallSite CS(T.getMemory());
      if (auto Callee = [CS]() {
        return !CS ? nullptr : dyn_cast<Function>(
          CS.getCalledValue()->stripPointerCasts());
      }())
        Callee->printAsOperand(OS, false);
      else
        T.getMemory()->printAsOperand(OS, false);
      OS << "> ";
    }
    OS << "\n";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(trait::IRDependence *Dep, raw_string_ostream &OS) {
    if (!Dep)
      return;
    if (!Dep->getDistance().first && !Dep->getDistance().second)
      return;
    OS << ":[";
    if (Dep->getDistance().first)
      Dep->getDistance().first->print(OS);
    OS << ",";
    if (Dep->getDistance().second)
      Dep->getDistance().second->print(OS);
    OS << "]";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(void *Dep, raw_string_ostream &OS) {}

  /// Returns a static trait map.
  TraitToStringMap & getStringMap() { return *mMap; }

  /// \brief Returns current trait set.
  ///
  /// \pre Trait set must not be null and has been specified by setTraitSet().
  AliasTrait & getTraitSet() {
    assert(mTS && "Trait set must not be null!");
    return *mTS;
  }

  /// Specifies current trait set.
  void setTraitSet(AliasTrait &TS) { mTS = &TS; }

private:
  TraitToStringMap *mMap;
  AliasTrait *mTS;
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
  for_each(LpInfo, [this, &OS, &RInfo, &DT](Loop *L) {
    DebugLoc Loc = L->getStartLoc();
    std::string Offset(L->getLoopDepth(), ' ');
    OS << Offset;
    Loc.print(OS);
    OS << "\n";
    auto N = RInfo.getRegionFor(L);
    auto &Info = getPrivateInfo();
    auto Itr = Info.find(N);
    assert(Itr != Info.end() && Itr->get<DependencySet>() &&
      "Privatiability information must be specified!");
    TraitToStringFunctor::TraitToStringMap TraitToStr;
    TraitToStringFunctor ToStrFunctor(TraitToStr, Offset + "  ", DT);
    auto ATRoot = Itr->get<DependencySet>()->getAliasTree()->getTopLevelNode();
    for (auto &TS : *Itr->get<DependencySet>()) {
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
