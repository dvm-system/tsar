//===--- DefinedMemory.h --- Defined Memory Analysis ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to determine must/may defined locations.
//
//===----------------------------------------------------------------------===//

#include "DefinedMemory.h"
#include "tsar_dbg_output.h"
#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "MemoryAccessUtils.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/Debug.h>
#include <functional>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "def-mem"

char DefinedMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(DefinedMemoryPass, "def-mem",
  "Defined Memory Region Analysis", true, true)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
#else
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
#endif
INITIALIZE_PASS_END(DefinedMemoryPass, "def-mem",
  "Defined Memory Region Analysis", true, true)

bool llvm::DefinedMemoryPass::runOnFunction(Function & F) {
  auto &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
#else
AliasAnalysis &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
#endif
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &AliasTree = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  ReachDFFwk ReachDefFwk(AliasTree, TLI, mDefInfo);
  solveDataFlowUpward(&ReachDefFwk, DFF);
  return false;
}

void DefinedMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AU.addRequired<AliasAnalysis>();
#else
  AU.addRequired<AAResultsWrapperPass>();
#endif
  AU.setPreservesAll();
}

FunctionPass * llvm::createDefinedMemoryPass() {
  return new DefinedMemoryPass();
}

bool DefUseSet::hasExplicitAccess(const llvm::MemoryLocation &Loc) const {
  for (auto I = mExplicitAccesses.begin(), E = mExplicitAccesses.end();
    I != E; ++I) {
    if (I->isForwardingAliasSet() || I->empty())
      continue;
    for (auto LocI = I->begin(), LocE = I->end(); LocI != LocE; ++LocI)
      if (LocI->getValue() == Loc.Ptr)
        return true;
  }
  return false;
}

namespace {
/// \brief This is a base class for functors which adds memory locations from a
/// specified AliasNode into a specified DefUseSet.
///
/// The derived classes may override add... methods.
template<class ImpTy> class AddAccessFunctor {
public:
  /// Create functor.
  AddAccessFunctor(AAResults &AA, const DataLayout &DL, DefUseSet &DU) :
    mAA(AA), mDL(DL), mDU(DU) {}

  /// Switches processing of a node to an appropriate add... method.
  void operator()(AliasNode *N) {
    assert(N && "Node must not be null!");
    auto Imp = static_cast<ImpTy *>(this);
    switch (N->getKind()) {
    default: llvm_unreachable("Unknown kind of alias node!"); break;
    case AliasNode::KIND_TOP: break;
    case AliasNode::KIND_ESTIMATE:
      Imp->addEstimate(cast<AliasEstimateNode>(*N)); break;
    case AliasNode::KIND_UNKNOWN:
      Imp->addUnknown(cast<AliasUnknownNode>(*N)); break;
    }
  }

  /// Implements default processing of estimate alias nodes.
  void addEstimate(AliasEstimateNode &N) {
    for (auto &EM : N)
      for (auto *Ptr : EM) {
        MemoryLocation Loc(Ptr, EM.getSize(), EM.getAAInfo());
        mDU.addMayDef(Loc);
        mDU.addUse(Loc);
      }
  }

  /// Implements default processing of unknown alias node.
  void addUnknown(AliasUnknownNode &N) {
    for (auto &I : N)
      mDU.addUnknownInst(&(*I));
  }

protected:
  AAResults &mAA;
  const DataLayout &mDL;
  DefUseSet &mDU;
};

/// This functor adds into a specified DefUseSet all locations from a specified
/// AliasNode which aliases with a memory accessed by a specified instruction.
class AddUnknownAccessFunctor :
  public AddAccessFunctor<AddUnknownAccessFunctor> {
public:
  AddUnknownAccessFunctor(AAResults &AA, const DataLayout &DL,
      const Instruction &Inst, DefUseSet &DU) :
    AddAccessFunctor<AddUnknownAccessFunctor>(AA, DL, DU), mInst(Inst) {}

  void addEstimate(AliasEstimateNode &N) {
    for (auto &EM : N) {
      if (!EM.isExplicit())
        continue;
      for (auto *APtr : EM) {
        MemoryLocation ALoc(APtr, EM.getSize(), EM.getAAInfo());
        // A condition below is necessary even in case of store instruction.
        // It is possible that Loc1 aliases Loc2 and Loc1 is already written
        // when Loc2 is evaluated. In this case evaluation of Loc1 should not be
        // repeated.
        if (mDU.hasDef(ALoc))
          continue;
        switch (mAA.getModRefInfo(&mInst, ALoc)) {
        case MRI_ModRef: mDU.addUse(ALoc); mDU.addMayDef(ALoc); break;
        case MRI_Mod: mDU.addMayDef(ALoc); break;
        case MRI_Ref: mDU.addUse(ALoc); break;
        }
      }
    }
  }
private:
  const Instruction &mInst;
};

/// This functor adds into a specified DefUseSet all locations from a specified
/// AliasNode which aliases with a specified memory location. Derived classes
/// should be used to specify access modes (Def/MayDef/Use).
template<class ImpTy>
class AddKnownAccessFunctor :
    public AddAccessFunctor<AddKnownAccessFunctor<ImpTy>> {
  using Base = AddAccessFunctor<AddKnownAccessFunctor<ImpTy>>;
public:
  AddKnownAccessFunctor(AAResults &AA, const DataLayout &DL,
      const MemoryLocation &Loc, DefUseSet &DU) :
    Base(AA, DL, DU), mLoc(Loc) {}

  void addEstimate(AliasEstimateNode &N) {
    for (auto &EM : N) {
      if (!EM.isExplicit())
        continue;
      for (auto *APtr : EM) {
        MemoryLocation ALoc(APtr, EM.getSize(), EM.getAAInfo());
        // A condition below is necessary even in case of store instruction.
        // It is possible that Loc1 aliases Loc2 and Loc1 is already written
        // when Loc2 is evaluated. In this case evaluation of Loc1 should not be
        // repeated.
        if (this->mDU.hasDef(ALoc))
          continue;
        auto AR = aliasRelation(this->mAA, this->mDL, mLoc, ALoc);
        if (AR.template is<trait::CoverAlias>())
          addMust(ALoc);
        else
          addMay(ALoc);
      }
    }
  }

private:
  void addMust(const MemoryLocation &Loc) {
    static_cast<ImpTy *>(this)->addMust(Loc);
  }

  void addMay(const MemoryLocation &Loc) {
    static_cast<ImpTy *>(this)->addMay(Loc);
  }

  const MemoryLocation &mLoc;
};

/// This macro determine functors according to a specified memory access modes.
#define ADD_ACCESS_FUNCTOR(NAME, BASE, MEM, MAY, MUST) \
class NAME : public BASE<NAME> { \
public: \
  NAME(AAResults &AA, const DataLayout &DL, MEM &Loc, DefUseSet &DU) : \
    BASE<NAME>(AA, DL, Loc, DU) {} \
private: \
  friend BASE<NAME>; \
  void addMust(const MemoryLocation &Loc) { MUST; } \
  void addMay(const MemoryLocation &Loc) { MAY; } \
};

ADD_ACCESS_FUNCTOR(AddDefFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addDef(Loc), mDU.addMayDef(Loc))
ADD_ACCESS_FUNCTOR(AddMayDefFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addMayDef(Loc), mDU.addMayDef(Loc))
ADD_ACCESS_FUNCTOR(AddUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addUse(Loc), mDU.addUse(Loc))
ADD_ACCESS_FUNCTOR(AddDefUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation,
  mDU.addDef(Loc); mDU.addUse(Loc), mDU.addMayDef(Loc); mDU.addUse(Loc))
ADD_ACCESS_FUNCTOR(AddMayDefUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation,
  mDU.addMayDef(Loc); mDU.addUse(Loc), mDU.addMayDef(Loc); mDU.addUse(Loc))
}

void DataFlowTraits<ReachDFFwk*>::initialize(
  DFNode *N, ReachDFFwk *DFF, GraphType) {
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null");
  if (llvm::isa<DFRegion>(N))
    return;
  auto &AT = DFF->getAliasTree();
  auto Pair = DFF->getDefInfo().insert(std::make_pair(N, std::make_tuple(
    llvm::make_unique<DefUseSet>(AT.getAliasAnalysis()),
    llvm::make_unique<ReachSet>())));
  // DefUseSet will be calculated here for nodes different to regions.
  // For nodes which represented regions this attribute has been already
  // calculated in collapse() function.
  auto &DU = Pair.first->get<DefUseSet>();
  auto *DFB = dyn_cast<DFBlock>(N);
  if (!DFB)
    return;
  BasicBlock *BB = DFB->getBlock();
  assert(BB && "Basic block must not be null!");
  for (Instruction &I : BB->getInstList()) {
    if (I.getType() && I.getType()->isPointerTy())
      DU->addAddressAccess(&I);
    for (auto Op : make_range(I.value_op_begin(), I.value_op_end())) {
      if (!Op->getType() || !Op->getType()->isPointerTy())
        continue;
      if (auto F = dyn_cast<Function>(Op))
        /// TODO (kaniandr@gmail.com): may be some other intrinsics also should
        /// be ignored, see llvm::AliasSetTracker::addUnknown() for details.
        switch (F->getIntrinsicID()) {
        default: break;
        case Intrinsic::dbg_declare: case Intrinsic::dbg_value:
        case Intrinsic::assume:
          continue;
        }
      DU->addAddressAccess(Op);
    }
    if (!I.mayReadOrWriteMemory())
      continue;
    // 1. Must/may def-use information will be set for location accessed in a
    // current instruction.
    // 2. Must/may def-use information will be set for all explicitly mentioned
    // locations (except locations with unknown descriptions) aliases location
    // accessed in a current instruction. Note that this attribute will be also
    // set for locations which are accessed implicitly.
    // 3. Unknown instructions will be remembered in DefUseSet.
    auto &DL = I.getModule()->getDataLayout();
    for_each_memory(I, DFF->getTLI(),
      [&DL, &AT, &DU](Instruction &I, MemoryLocation &&Loc, unsigned Idx,
          AccessInfo R, AccessInfo W) {
        stripToBase(DL, Loc);
        DU->addExplicitAccess(Loc);
        auto *EM = AT.find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        auto AN = EM->getAliasNode(AT);
        assert(AN && "Alias node must not be null!");
        auto &AA = AT.getAliasAnalysis();
        ImmutableCallSite CS(&I);
        if (CS) {
          switch (AA.getArgModRefInfo(CS, Idx)) {
          case MRI_NoModRef: W = R = AccessInfo::No; break;
          case MRI_Mod: W = AccessInfo::May; R = AccessInfo::No; break;
          case MRI_Ref: W = AccessInfo::No; R = AccessInfo::May; break;
          case MRI_ModRef: W = R = AccessInfo::May; break;
          }
        }
        switch (W) {
        case AccessInfo::No:
          if (R != AccessInfo::No)
            for_each_alias(&AT, AN, AddUseFunctor(AA, DL, Loc, *DU));
          break;
        case AccessInfo::May:
          if (R != AccessInfo::No)
            for_each_alias(&AT, AN, AddMayDefUseFunctor(AA, DL, Loc, *DU));
          else
            for_each_alias(&AT, AN, AddMayDefFunctor(AA, DL, Loc, *DU));
          break;
        case AccessInfo::Must:
          if (R != AccessInfo::No)
            for_each_alias(&AT, AN, AddDefUseFunctor(AA, DL, Loc, *DU));
          else
            for_each_alias(&AT, AN, AddDefFunctor(AA, DL, Loc, *DU));
          break;
        }
      },
      [&DL, &AT, &DU](Instruction &I, AccessInfo, AccessInfo) {
        DU->addExplicitAccess(&I);
        DU->addUnknownInst(&I);
        auto *AN = AT.findUnknown(I);
        assert(AN && "Alias node must not be null!");
        auto &AA = AT.getAliasAnalysis();
        for_each_alias(&AT, AN, AddUnknownAccessFunctor(AA, DL, I, *DU));
      }
    );
  }
  DEBUG(
    dbgs() << "[DEFUSE] Def/Use locations for the following basic block:";
  DFB->getBlock()->print(dbgs());
  dbgs() << "Outward exposed must define locations:\n";
  for (auto &Loc : DU->getDefs())
    (printLocationSource(dbgs(), Loc), dbgs() << "\n");
  dbgs() << "Outward exposed may define locations:\n";
  for (auto &Loc : DU->getMayDefs())
    (printLocationSource(dbgs(), Loc), dbgs() << "\n");
  dbgs() << "Outward exposed uses:\n";
  for (auto &Loc : DU->getUses())
    (printLocationSource(dbgs(), Loc), dbgs() << "\n");
  dbgs() << "[END DEFUSE]\n";
  );
}

bool DataFlowTraits<ReachDFFwk*>::transferFunction(
  ValueType V, DFNode *N, ReachDFFwk *DFF, GraphType) {
  // Note, that transfer function is never evaluated for the entry node.
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null");
  DEBUG(
    dbgs() << "[TRANSFER REACH]\n";
  if (auto DFB = dyn_cast<DFBlock>(N))
    DFB->getBlock()->dump();
  dbgs() << "IN:\n";
  dbgs() << "MUST REACH DEFINITIONS:\n";
  V.MustReach.dump();
  dbgs() << "MAY REACH DEFINITIONS:\n";
  V.MayReach.dump();
  dbgs() << "OUT:\n";
  );
  auto I = DFF->getDefInfo().find(N);
  assert(I != DFF->getDefInfo().end() &&
    I->get<ReachSet>() && I->get<DefUseSet>() &&
    "Data-flow value must be specified!");
  auto &RS = I->get<ReachSet>();
  RS->setIn(std::move(V)); // Do not use V below to avoid undefined behavior.
  if (llvm::isa<DFExit>(N)) {
    if (RS->getOut().MustReach != RS->getIn().MustReach ||
        RS->getOut().MayReach != RS->getIn().MayReach) {
      RS->setOut(RS->getIn());
      DEBUG(
        dbgs() << "MUST REACH DEFINITIONS:\n";
      RS->getOut().MustReach.dump();
      dbgs() << "MAY REACH DEFINITIONS:\n";
      RS->getOut().MayReach.dump();
      dbgs() << "[END TRANSFER]\n";
      );
      return true;
    }
    DEBUG(
      dbgs() << "MUST REACH DEFINITIONS:\n";
    RS->getOut().MustReach.dump();
    dbgs() << "MAY REACH DEFINITIONS:\n";
    RS->getOut().MayReach.dump();
    dbgs() << "[END TRANSFER]\n";
    );
    return false;
  }
  auto &DU = I->get<DefUseSet>();
  assert(DU && "Value of def-use attribute must not be null!");
  DefinitionInfo newOut;
  newOut.MustReach = LocationDFValue::emptyValue();
  newOut.MustReach.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.MustReach.merge(RS->getIn().MustReach);
  newOut.MayReach = LocationDFValue::emptyValue();
  // newOut.MayReach must contain both must and may defined locations.
  // Let us consider an example:
  // for(...) {
  //   if (...) {
  //     X = ...
  //     break;
  // }
  // MustReach must not contain X, because if conditional is always false
  // the X variable will not be written. But MayReach must contain X.
  // In the basic block that is associated with a body of if statement X is a
  // must defined location. So it is necessary to insert must defined locations
  // in the MayReach collection.
  newOut.MayReach.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.MayReach.insert(DU->getMayDefs().begin(), DU->getMayDefs().end());
  newOut.MayReach.merge(RS->getIn().MayReach);
  if (RS->getOut().MustReach != newOut.MustReach ||
    RS->getOut().MayReach != newOut.MayReach) {
    RS->setOut(std::move(newOut));
    DEBUG(
      dbgs() << "MUST REACH DEFINITIONS:\n";
    RS->getOut().MustReach.dump();
    dbgs() << "MAY REACH DEFINITIONS:\n";
    RS->getOut().MayReach.dump();
    dbgs() << "[END TRANSFER]\n";
    );
    return true;
  }
  DEBUG(
    dbgs() << "MUST REACH DEFINITIONS:\n";
  RS->getOut().MustReach.dump();
  dbgs() << "MAY REACH DEFINITIONS:\n";
  RS->getOut().MayReach.dump();
  dbgs() << "[END TRANSFER]\n";
  );
  return false;
}

void ReachDFFwk::collapse(DFRegion *R) {
  assert(R && "Region must not be null!");
  typedef RegionDFTraits<ReachDFFwk *> RT;
  auto &AT = getAliasTree();
  auto &AA = AT.getAliasAnalysis();
  auto Pair = getDefInfo().insert(std::make_pair(R, std::make_tuple(
    llvm::make_unique<DefUseSet>(AA),
    llvm::make_unique<ReachSet>())));
  auto &DefUse = Pair.first->get<DefUseSet>();
  assert(DefUse && "Value of def-use attribute must not be null!");
  // ExitingDefs.MustReach is a set of must define locations (Defs) for the
  // loop. These locations always have definitions inside the loop regardless
  // of execution paths of iterations of the loop.
  DFNode *ExitNode = R->getExitNode();
  const DefinitionInfo &ExitingDefs = RT::getValue(ExitNode, this);
  for (DFNode *N : R->getNodes()) {
    auto DefItr = getDefInfo().find(N);
    assert(DefItr != getDefInfo().end() &&
      DefItr->get<ReachSet>() && DefItr->get<DefUseSet>() &&
      "Data-flow value must be specified!");
    auto &RS = DefItr->get<ReachSet>();
    auto &DU = DefItr->get<DefUseSet>();
    // We calculate a set of locations (Uses)
    // which get values outside the loop or from previous loop iterations.
    // These locations can not be privatized.
    for (auto &Loc : DU->getUses())
      if (!RS->getIn().MustReach.contain(Loc))
        DefUse->addUse(Loc);
    // It is possible that some locations are only written in the loop.
    // In this case this locations are not located at set of node uses but
    // they are located at set of node defs.
    // We calculate a set of must define locations (Defs) for the loop.
    // These locations always have definitions inside the loop regardless
    // of execution paths of iterations of the loop.
    // The set of may define locations (MayDefs) for the loop is also
    // calculated.
    // Note that if ExitingDefs.MustReach does not comprises a location it
    // means that it may have definition it the loop but it does not mean
    // that ExitingDefs.MayReach comprises it. In the following example
    // may definitions for the loop contains X, but ExitingDefs.MayReach does
    // not contain it (there is no iteration on which exit from this loop
    // occurs and X is written).
    // for (;;) {
    //   if (...)
    //     break;
    //   if (...)
    //     X = ...;
    // }
    for (auto &Loc : DU->getDefs()) {
      if (ExitingDefs.MustReach.contain(Loc))
        DefUse->addDef(Loc);
      else
        DefUse->addMayDef(Loc);
    }
    for (auto &Loc : DU->getMayDefs())
      DefUse->addMayDef(Loc);
    DefUse->addExplicitAccesses(DU->getExplicitAccesses());
    for (auto Loc : DU->getAddressAccesses())
      DefUse->addAddressAccess(Loc);
    for (auto Inst : DU->getUnknownInsts())
      DefUse->addUnknownInst(Inst);
  }
  DEBUG(
    dbgs() << "[DEFUSE] Def/Use locations for a collapsed region.\n";
    dbgs() << "Outward exposed must define locations:\n";
    for (auto &Loc : DefUse->getDefs())
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "Outward exposed may define locations:\n";
    for (auto &Loc : DefUse->getMayDefs())
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "Outward exposed uses:\n";
    for (auto &Loc : DefUse->getUses())
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
  );
}
