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
#include "tsar_utility.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/AliasAnalysis.h>
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
STATISTIC(NumShared, "Number of shraed locations found");
STATISTIC(NumAddressAccess, "Number of locations address of which is evaluated");

char PrivateRecognitionPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateRecognitionPass, "private",
                      "Private Variable Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(LiveMemoryPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
#else
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
#endif
INITIALIZE_PASS_END(PrivateRecognitionPass, "private",
                    "Private Variable Analysis", true, true)

bool PrivateRecognitionPass::runOnFunction(Function &F) {
  releaseMemory();
#ifdef DEBUG
  for (const BasicBlock &BB : F)
    assert((&F.getEntryBlock() == &BB || BB.getNumUses() > 0 )&&
      "Data-flow graph must not contain unreachable nodes!");
#endif
  LoopInfo &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
#else
  AliasAnalysis &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
#endif
  DFRegionInfo &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  mDefInfo = &getAnalysis<DefinedMemoryPass>().getDefInfo();
  mLiveInfo = &getAnalysis<LiveMemoryPass>().getLiveInfo();
  mAliasTracker = new AliasSetTracker(AA);
  mAliasTree = &getAnalysis<EstimateMemoryPass>().getAliasTree();
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    mAliasTracker->add(&*I);
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  GraphNumbering<const AliasNode *> Numbers;
  numberGraph(mAliasTree, &Numbers);
  resolveCandidats(Numbers, DFF);
  delete mAliasTracker, mAliasTracker = nullptr;
  return false;
}

namespace tsar {
namespace detail {
/// \brief Identifiers of recognized traits.
///
/// This is a helpful enumeration which must not be used outside the private
/// recognition pass. It is easy to join different traits. For example,
/// Shared & LastPrivate = 00110 = LastPrivate & FirstPrivate. So if some part
/// of memory locations is shared and other part is last private a union is
/// last private and first private (for details see resolve... methods).
enum TraitId : unsigned long long {
  NoAccess = 111111_b,
  Shared = 111101_b,
  Private = 011111_b,
  FirstPrivate = 011101_b,
  SecondToLastPrivate = 010111_b,
  LastPrivate = 001111_b,
  DynamicPrivate = 000111_b,
  Dependency = 000001_b,
  AddressAccess = 111110_b,
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

/// Internal representation of traits of some memory location (see .cpp file).
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
}
}

void PrivateRecognitionPass::resolveCandidats(
    const GraphNumbering<const AliasNode *> &Numbers, DFRegion *R) {
  assert(R && "Region must not be null!");
  if (auto *L = dyn_cast<DFLoop>(R)) {
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
    resolveAccesses(R->getLatchNode(), R->getExitNode(),
      *DefItr->get<DefUseSet>(), *LiveItr->get<LiveSet>(),
      ExplicitAccesses, ExplicitUnknowns, NodeTraits);
    resolvePointers(*DefItr->get<DefUseSet>(), ExplicitAccesses);
    resolveAddresses(L, *DefItr->get<DefUseSet>(), ExplicitAccesses, NodeTraits);
    propagateTraits(Numbers, *R, ExplicitAccesses, ExplicitUnknowns, NodeTraits,
      *PrivInfo.first->get<DependencySet>());
  }
  for (auto I = R->region_begin(), E = R->region_end(); I != E; ++I)
    resolveCandidats(Numbers, *I);
}

void PrivateRecognitionPass::resolveAccesses(const DFNode *LatchNode,
    const DFNode *ExitNode, const tsar::DefUseSet &DefUse,
    const tsar::LiveSet &LS, TraitMap &ExplicitAccesses,
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
      I->get<TraitList>().push_front(std::make_pair(Base, TraitImp()));
      Pair.first->get<TraitImp>() =
        &I->get<TraitList>().front().get<TraitImp>();
    }
    auto &CurrTraits = *Pair.first->get<TraitImp>();
    if (!DefUse.hasUse(Loc)) {
      if (!LS.getOut().overlap(Loc))
        CurrTraits &= Private;
      else if (DefUse.hasDef(Loc))
        CurrTraits &= LastPrivate;
      else if (LatchDefs.MustReach.contain(Loc) &&
        !ExitingDefs.MayReach.overlap(Loc))
        // These location will be stored as second to last private, i.e.
        // the last definition of these locations is executed on the
        // second to the last loop iteration (on the last iteration the
        // loop condition check is executed only).
        // It is possible that there is only one (last) iteration in
        // the loop. In this case the location has not been assigned and
        // must be declared as a first private.
        CurrTraits &= SecondToLastPrivate & FirstPrivate;
      else
        // There is no certainty that the location is always assigned
        // the value in the loop. Therefore, it must be declared as a
        // first private, to preserve the value obtained before the loop
        // if it has not been assigned.
        CurrTraits &= DynamicPrivate & FirstPrivate;
    } else if (DefUse.hasMayDef(Loc) || DefUse.hasDef(Loc)) {
      CurrTraits &= Dependency;
    } else {
      CurrTraits &= Shared;
    }
  }
  for (const auto &Unknown : DefUse.getExplicitUnknowns()) {
    const auto N = mAliasTree->findUnknown(Unknown);
    assert(N && "Alias node for unknown memory location must not be null!");
    auto I = NodeTraits.find(N);
    auto &AA = mAliasTree->getAliasAnalysis();
    ImmutableCallSite CS(Unknown);
    TraitId TID = (CS && AA.onlyReadsMemory(CS)) ?
      TraitId::Shared : TraitId::Dependency;
    I->get<UnknownList>().push_front(std::make_pair(Unknown, TraitImp(TID)));
    auto &CurrTraits = ExplicitUnknowns.insert(std::make_pair(Unknown,
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
      if ((*LocTraits->get<TraitImp>() | ~AddressAccess) == Private ||
          (*LocTraits->get<TraitImp>() | ~AddressAccess) == Shared)
        continue;
      const EstimateMemory *Ptr = mAliasTree->find(MemoryLocation::get(LI));
      assert(Ptr && "Estimate memory location must not be null!");
      auto PtrTraits = ExplicitAccesses.find(Ptr);
      assert(PtrTraits != ExplicitAccesses.end() &&
        "Traits of location must be initialized!");
      if ((*PtrTraits->get<TraitImp>() | ~AddressAccess) == Shared)
        continue;
      // Location can not be declared as copy in or copy out without
      // additional analysis because we do not know which memory must
      // be copy. Let see an example:
      // for (...) { P = &X; *P = ...; P = &Y; } after loop P = &Y, not &X.
      // P = &Y; for (...) { *P = ...; P = &X; } before loop P = &Y, not &X.
      // Note that case when location is shared, but pointer is not shared
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
    AliasMap &NodeTraits, DependencySet &DS) {
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
            if (EA != ExplicitAccesses.end())
              *EA->get<TraitImp>() &= EMToT.get<TraitImp>();
            else
              NTItr->get<TraitList>().push_front(
                std::make_pair(Parent, std::move(EMToT.get<TraitImp>())));
          }
        }
        for (auto &UToT : *CT.get<UnknownList>())
          NTItr->get<UnknownList>().push_front(std::move(UToT));
      }
    }
    auto &TL = NTItr->get<TraitList>();
    for (auto BI = TL.before_begin(), I = TL.begin(), E = TL.end(); I != E;)
      removeRedundant(N, NTItr->get<TraitList>(), BI, I);
    TraitPair NT(&NTItr->get<TraitList>(), &NTItr->get<UnknownList>());
    storeResults(Numbers, R, *N, ExplicitAccesses, ExplicitUnknowns, NT, DS);
    ChildTraits.push(std::move(NT));
    Prev = N;
  }
  std::vector<const AliasNode *> Coverage;
  explicitAccessCoverage(DS, *mAliasTree, Coverage);
  // All descendant nodes for nodes in `Coverage` accessed some part of
  // explicitly accessed memory. The conservativeness of analysis implies
  // that memory accesses from this nodes arise loop carried dependencies.
  for (auto *N : Coverage)
    for (auto &Child : make_range(N->child_begin(), N->child_end()))
      for (auto *Descendant : make_range(df_begin(&Child), df_end(&Child))) {
        auto I = DS.find(Descendant);
        if (I != DS.end() && !I->is<trait::NoAccess>())
          I->set<trait::Dependency>();
      }
}

void PrivateRecognitionPass::checkFirstPrivate(
    const GraphNumbering<const AliasNode *> &Numbers,
    const DFRegion &R,
    const TraitList::iterator &TraitItr, DependencyDescriptor &Dptr) {
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
      if (!isAmbiguousCover(LatchDefs.MustReach, *Descendant) &&
          !isAmbiguousCover(ExitingDefs.MustReach, *Descendant))
        continue;
    }
    DefLeafs.push_back(Descendant);
  }
  if (cover(*mAliasTree, Numbers, *EM, DefLeafs.begin(), DefLeafs.end()))
    return;
  TraitItr->get<TraitImp>() &= FirstPrivate;
  Dptr.set<trait::FirstPrivate>();
}

void PrivateRecognitionPass::removeRedundant(
    const AliasNode *N, TraitList &Traits,
    TraitList::iterator &BeforeCurrItr, TraitList::iterator &CurrItr) {
  assert(CurrItr != Traits.end() && "Iterator must be valid!");
  auto BeforeI = CurrItr, I = CurrItr, E = Traits.end();
  auto Current = CurrItr->get<EstimateMemory>();
  for (++I; I != E;) {
    if (Current == I->get<EstimateMemory>()) {
      CurrItr = Traits.erase_after(BeforeCurrItr);
      return;
    }
    auto Ancestor = ancestor(Current, I->get<EstimateMemory>());
    if (Ancestor == I->get<EstimateMemory>()) {
      I->get<TraitImp>() &= CurrItr->get<TraitImp>();
      CurrItr = Traits.erase_after(BeforeCurrItr);
      return;
    }
    if (Ancestor == Current) {
      CurrItr->get<TraitImp>() &= I->get<TraitImp>();
      I = Traits.erase_after(BeforeI);
    } else {
      ++BeforeI; ++I;
    }
  }
  // Now, it is necessary to find the largest estimate location which covers
  // the current one and is associated with the currently analyzed node `N`.
  // Note, that if current location is not stored in `N` it means that this
  // locations is stored in one of proper descendant of `N`. It also means
  // that proper ancestors of the location in estimate tree is stored in
  // proper ancestors of `N` (see propagateTraits()) and the current locations
  // should not be analyzed.
  if (Current->getAliasNode(*mAliasTree) == N) {
    while (Current->getParent() &&
      Current->getParent()->getAliasNode(*mAliasTree) == N)
      Current = Current->getParent();
    CurrItr->get<EstimateMemory>() = Current;
  }
  ++BeforeCurrItr; ++CurrItr;
}

void PrivateRecognitionPass::storeResults(
    const GraphNumbering<const tsar::AliasNode *> &Numbers,
    const DFRegion &R, const AliasNode &N,
    const TraitMap &ExplicitAccesses, const UnknownMap &ExplicitUnknowns,
    const TraitPair &Traits, DependencySet &DS) {
  assert(DS.find(&N) == DS.end() && "Results must not be already stored!");
  DependencySet::iterator NodeTraitItr;
  auto EMI = Traits.get<TraitList>()->begin();
  auto EME = Traits.get<TraitList>()->end();
  if (!Traits.get<TraitList>()->empty()) {
    NodeTraitItr = DS.insert(&N, DependencyDescriptor()).first;
    auto SecondEM = Traits.get<TraitList>()->begin(); ++SecondEM;
    if (Traits.get<UnknownList>()->empty() && SecondEM == EME) {
      *NodeTraitItr = toDescriptor(EMI->get<TraitImp>(), 1);
      checkFirstPrivate(Numbers, R, EMI, *NodeTraitItr);
      auto ExplicitItr = ExplicitAccesses.find(EMI->get<EstimateMemory>());
      if (ExplicitItr != ExplicitAccesses.end() &&
          (*ExplicitItr->second | ~AddressAccess) != NoAccess &&
          EMI->get<EstimateMemory>()->getAliasNode(*mAliasTree) == &N)
        NodeTraitItr->set<trait::ExplicitAccess>();
      NodeTraitItr->insert(
        EstimateMemoryTrait(EMI->get<EstimateMemory>(), *NodeTraitItr));
      return;
    }
  } else if (!Traits.get<UnknownList>()->empty()) {
    NodeTraitItr = DS.insert(&N, DependencyDescriptor()).first;
  } else {
    return;
  }
  // There are memory locations which are explicitly accessed in the loop and
  // which are covered by estimate memory locations from different estimate
  // memory trees. So only two type of combined results are possible: shared or
  // dependency.
  TraitImp CombinedTrait;
  for (; EMI != EME; ++EMI) {
    CombinedTrait &= EMI->get<TraitImp>();
    auto Dptr = toDescriptor(EMI->get<TraitImp>(), 0);
    checkFirstPrivate(Numbers, R, EMI, Dptr);
    auto ExplicitItr = ExplicitAccesses.find(EMI->get<EstimateMemory>());
    if (ExplicitItr != ExplicitAccesses.end() &&
        (*ExplicitItr->get<TraitImp>() | ~AddressAccess) != NoAccess &&
        EMI->get<EstimateMemory>()->getAliasNode(*mAliasTree) == &N) {
      NodeTraitItr->set<trait::ExplicitAccess>();
      Dptr.set<trait::ExplicitAccess>();
    }
    NodeTraitItr->insert(
      EstimateMemoryTrait(EMI->get<EstimateMemory>(), std::move(Dptr)));
  }
  for (auto &U : *Traits.get<UnknownList>()) {
    CombinedTrait &= U.get<TraitImp>();
    auto Dptr = toDescriptor(U.get<TraitImp>(), 0);
    auto ExplicitItr = ExplicitUnknowns.find(U.get<Instruction>());
    if (ExplicitItr != ExplicitUnknowns.end() &&
        (*ExplicitItr->get<TraitImp>() | ~AddressAccess) != NoAccess &&
        ExplicitItr->get<AliasNode>() == &N) {
      NodeTraitItr->set<trait::ExplicitAccess>();
      Dptr.set<trait::ExplicitAccess>();
    }
    NodeTraitItr->insert(
      UnknownMemoryTrait(U.get<Instruction>(), std::move(Dptr)));
  }
  CombinedTrait &=
    (CombinedTrait | ~AddressAccess) == Shared ? Shared : Dependency;
  if (NodeTraitItr->is<trait::ExplicitAccess>()) {
    *NodeTraitItr = toDescriptor(CombinedTrait, NodeTraitItr->count());
    NodeTraitItr->set<trait::ExplicitAccess>();
  } else {
    *NodeTraitItr = toDescriptor(CombinedTrait, NodeTraitItr->count());
  }
}

DependencyDescriptor PrivateRecognitionPass::toDescriptor(
    const TraitImp &T, unsigned TraitNumber) {
  DependencyDescriptor Dptr;
  switch (T | ~AddressAccess) {
  default:
    llvm_unreachable("Unknown type of memory location dependency!");
    break;
  case NoAccess: Dptr.set<trait::NoAccess>(); break;
  case Shared: Dptr.set<trait::Shared>(); NumShared += TraitNumber; break;
  case Dependency: Dptr.set<trait::Dependency>(); NumDeps += TraitNumber; break;
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
  if (!(T & ~AddressAccess)) {
    Dptr.set<trait::AddressAccess>();
    NumAddressAccess += TraitNumber;
  }
  return Dptr;
}

void PrivateRecognitionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<LiveMemoryPass>();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AU.addRequired<AliasAnalysis>();
#else
  AU.addRequired<AAResultsWrapperPass>();
#endif
  AU.addRequired<EstimateMemoryPass>();
  AU.setPreservesAll();
}

namespace {
/// This functor stores representation of a trait in a static map as a string.
class TraitToStringFunctor {
public:
  /// Static map from trait to its string representation.
  typedef bcl::StaticTraitMap<
    std::string, DependencyDescriptor> TraitToStringMap;

  /// Creates the functor.
  TraitToStringFunctor(TraitToStringMap &Map, llvm::StringRef Offset) :
    mMap(&Map), mOffset(Offset) {}

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
      printLocationSource(OS, T.getMemory()->front());
      OS << ", ";
      if (T.getMemory()->getSize() == MemoryLocation::UnknownSize)
        OS << "?";
      else
        OS << T.getMemory()->getSize();
      OS << "> ";
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
  for_each(LpInfo, [this, &OS, &RInfo](Loop *L) {
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
    TraitToStringFunctor ToStrFunctor(TraitToStr, Offset + "  ");
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
