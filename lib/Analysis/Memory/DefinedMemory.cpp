//===--- DefinedMemory.cpp --- Defined Memory Analysis ----------*- C++ -*-===//
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
// This file implements passes to determine must/may defined locations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/GlobalDefinedMemory.h"
#include "tsar/Support/Utils.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Dominators.h>
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
  "Defined Memory Region Analysis", false, true)
  LLVM_DEBUG(INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass));
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(DefinedMemoryPass, "def-mem",
  "Defined Memory Region Analysis", false, true)

bool llvm::DefinedMemoryPass::runOnFunction(Function & F) {
  auto &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &AliasTree = getAnalysis<EstimateMemoryPass>().getAliasTree();
  const DominatorTree *DT = nullptr;
  LLVM_DEBUG(DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree());
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  
  auto GDM = getAnalysisIfAvailable<GlobalDefinedMemory>();
  if (GDM == nullptr) {
    ReachDFFwk ReachDefFwk(AliasTree, TLI, DT, mDefInfo);
    solveDataFlowUpward(&ReachDefFwk, DFF);
  }
  else {
    ReachDFFwk ReachDefFwk(AliasTree, TLI, DT, mDefInfo, GDM->getInterprocDefInfo());
    solveDataFlowUpward(&ReachDefFwk, DFF);
  }
  return false;
}

void DefinedMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  LLVM_DEBUG(AU.addRequired<DominatorTreeWrapperPass>());
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<GlobalDefinedMemory>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createDefinedMemoryPass() {
  return new DefinedMemoryPass();
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
    for (auto *I : N)
      mDU.addUnknownInst(I);
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
        case ModRefInfo::ModRef: mDU.addUse(ALoc); mDU.addMayDef(ALoc); break;
        case ModRefInfo::Mod: mDU.addMayDef(ALoc); break;
        case ModRefInfo::Ref: mDU.addUse(ALoc); break;
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
        if (AR.template is_any<trait::CoverAlias, trait::CoincideAlias>()) {
          addMust(ALoc);
        } else if (AR.template is<trait::ContainedAlias>()) {
          int64_t OffsetLoc, OffsetALoc;
          GetPointerBaseWithConstantOffset(mLoc.Ptr, OffsetLoc, this->mDL);
          GetPointerBaseWithConstantOffset(ALoc.Ptr, OffsetALoc, this->mDL);
          // Base - OffsetLoc --------------|mLoc.Ptr| --- mLoc.Size --- |
          // Base - OffsetALoc -|ALoc.Ptr| ---- ALoc.Size -------------------- |
          //--------------------|ALoc.Ptr|--| ------ addMust(...) ------ |
          MemoryLocationRange Range(ALoc.Ptr, OffsetLoc - OffsetALoc,
            OffsetLoc - OffsetALoc + mLoc.Size, ALoc.AATags);
          if (this->mDU.hasDef(Range))
            continue;
          addMust(Range);
        } else {
          addMay(ALoc);
        }
      }
    }
  }

private:
  void addMust(const MemoryLocationRange &Loc) {
    static_cast<ImpTy *>(this)->addMust(Loc);
  }

  void addMay(const MemoryLocationRange &Loc) {
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
  void addMust(const MemoryLocationRange &Loc) { MUST; } \
  void addMay(const MemoryLocationRange &Loc) { MAY; } \
};

ADD_ACCESS_FUNCTOR(AddDefFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addMayDef(Loc), mDU.addDef(Loc))
ADD_ACCESS_FUNCTOR(AddMayDefFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addMayDef(Loc), mDU.addMayDef(Loc))
ADD_ACCESS_FUNCTOR(AddUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addUse(Loc), mDU.addUse(Loc))
ADD_ACCESS_FUNCTOR(AddDefUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation,
  mDU.addMayDef(Loc); mDU.addUse(Loc), mDU.addDef(Loc); mDU.addUse(Loc))
ADD_ACCESS_FUNCTOR(AddMayDefUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation,
  mDU.addMayDef(Loc); mDU.addUse(Loc), mDU.addMayDef(Loc); mDU.addUse(Loc))

#ifndef NDEBUG
void intializeDefUseSetLog(
    const DFNode &N, const DefUseSet &DU, const DominatorTree *DT) {
  dbgs() << "[DEFUSE] Def/Use locations for ";
  if (isa<DFBlock>(N)) {
    dbgs() << "the following basic block:\n";
    TSAR_LLVM_DUMP(cast<DFBlock>(N).getBlock()->dump());
  } else if (isa<DFEntry>(N) || isa<DFExit>(N) || isa<DFLatch>(N)) {
    dbgs() << "an empty boundary basic block:\n";
  } else {
    dbgs() << "a collapsed region:\n";
  }
  dbgs() << "Outward exposed must define locations:\n";
  for (auto &Loc : DU.getDefs())
    printLocationSource(dbgs(), Loc, DT), dbgs() << "\n";
  dbgs() << "Outward exposed may define locations:\n";
  for (auto &Loc : DU.getMayDefs())
    printLocationSource(dbgs(), Loc, DT), dbgs() << "\n";
  dbgs() << "Outward exposed uses:\n";
  for (auto &Loc : DU.getUses())
    printLocationSource(dbgs(), Loc, DT), dbgs() << "\n";
  dbgs() << "Explicitly accessed locations:\n";
  for (auto &Loc : DU.getExplicitAccesses())
    printLocationSource(dbgs(), Loc, DT), dbgs() << "\n";
  for (auto *I : DU.getExplicitUnknowns())
    I->print(dbgs()), dbgs() << "\n";
  dbgs() << "[END DEFUSE]\n";
};

void initializeTransferBeginLog(const DFNode &N, const DefinitionInfo &In,
    const DominatorTree *DT) {
  dbgs() << "[TRANSFER REACH] Transfer function for ";
  if (isa<DFBlock>(N)) {
    dbgs() << "the following basic block:\n";
    TSAR_LLVM_DUMP(cast<DFBlock>(N).getBlock()->dump());
  } else if (isa<DFEntry>(N) || isa<DFExit>(N) || isa<DFLatch>(N)) {
    dbgs() << "an empty boundary basic block:\n";
  } else {
    dbgs() << "a collapsed region:\n";
  }
  dbgs() << "IN:\n";
  dbgs() << "MUST REACH DEFINITIONS:\n";
  In.MustReach.dump(DT);
  dbgs() << "MAY REACH DEFINITIONS:\n";
  In.MayReach.dump(DT);
}

void initializeTransferEndLog(const DefinitionInfo &Out, bool HasChanges,
    const DominatorTree *DT) {
  dbgs() << "OUT ";
  if (HasChanges)
    dbgs() << "with changes:\n";
  else
    dbgs() << "without changes:\n";
  dbgs() << "MUST REACH DEFINITIONS:\n";
  Out.MustReach.dump(DT);
  dbgs() << "MAY REACH DEFINITIONS:\n";
  Out.MayReach.dump(DT);
  dbgs() << "[END TRANSFER]\n";

}
#endif
}

void DataFlowTraits<ReachDFFwk*>::initialize(
  DFNode *N, ReachDFFwk *DFF, GraphType) {
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null");
  if (llvm::isa<DFRegion>(N))
    return;
  auto &AT = DFF->getAliasTree();
  auto Pair = DFF->getDefInfo().insert(std::make_pair(N, std::make_tuple(
    llvm::make_unique<DefUseSet>(),llvm::make_unique<ReachSet>())));

  auto *InterprocDefInfo = DFF->getInterprocdefInfo();
  auto &TLI = DFF->getTLI();
  // DefUseSet will be calculated here for nodes different to regions.
  // For nodes which represented regions this attribute has been already
  // calculated in collapse() function.
  auto &DU = Pair.first->get<DefUseSet>();
  auto *DFB = dyn_cast<DFBlock>(N);
  if (!DFB)
    return;
  BasicBlock *BB = DFB->getBlock();
  Function *F = BB->getParent();
  assert(BB && "Basic block must not be null!");
  for (Instruction &I : BB->getInstList()) {
    // TODO (kaniandr@gmail.com): LLVM analysis says that memory marker
    // intrinsics may access memory, so we exclude these intrinsics from
    // analysis manually. Is it correct? For example, may be we should set that
    // 'lifetime' intrinsics write memory?
    if (auto II = llvm::dyn_cast<IntrinsicInst>(&I))
      if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
          isDbgInfoIntrinsic(II->getIntrinsicID()))
        continue;
    if (I.getType() && I.getType()->isPointerTy())
      DU->addAddressAccess(&I);
    auto isAddressAccess = [&F](const Value *V) {
      if (const ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(V)) {
        if (!NullPointerIsDefined(F, CPN->getType()->getAddressSpace()))
          return false;
      } else if (isa<UndefValue>(V) || !V->getType() ||
                 !V->getType()->isPointerTy()) {
        return false;
      } else if (auto F = dyn_cast<Function>(V)) {
        // In LLVM it is not possible to take address of intrinsic function.
        if (F->isIntrinsic())
          return false;
      }
      return true;
    };
    for (auto *Op : I.operand_values()) {
      if (isAddressAccess(Op))
        DU->addAddressAccess(Op);
      if (auto *CE = dyn_cast<ConstantExpr>(Op)) {
        SmallVector<ConstantExpr *, 4> WorkList{ CE };
        do {
          auto *Expr = WorkList.pop_back_val();
          for (auto *ExprOp : Expr->operand_values()) {
            if (isAddressAccess(ExprOp))
              DU->addAddressAccess(ExprOp);
            if (auto ExprCEOp = dyn_cast<ConstantExpr>(ExprOp))
              WorkList.push_back(ExprCEOp);
          }
        } while (!WorkList.empty());
      }
    }
    // Any call may access some addresses even if it does not access memory.
    // TODO (kaniandr@gmail.com): use interprocedural analysis to clarify list
    // of unknown address accesses.
    if (ImmutableCallSite(&I))
      DU->addAddressUnknowns(&I);
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
    for_each_memory(I, TLI,
      [&DL, &AT, InterprocDefInfo, &TLI, &DU](Instruction &I, MemoryLocation &&Loc, unsigned Idx,
          AccessInfo R, AccessInfo W) {
        auto *EM = AT.find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        auto &AA = AT.getAliasAnalysis();
        /// List of ambiguous pointers contains only one pointer for each set
        /// of must alias locations. So, it is not guaranteed that Loc is
        /// presented in this list. If it is not presented there than it will
        /// not be presented in MustDefs, MayDefs and Uses. Some other location
        /// which must alias Loc will be presented there. Hence, it is
        /// necessary to add other location which must alias Loc in the list
        /// of explicit accesses.
        for (auto *APtr : *EM) {
          MemoryLocation ALoc(APtr, EM->getSize(), EM->getAAInfo());
          auto AR = aliasRelation(AA, DL, Loc, ALoc);
          if (AR.template is<trait::CoincideAlias>())
            DU->addExplicitAccess(ALoc);
        }
        auto AN = EM->getAliasNode(AT);
        assert(AN && "Alias node must not be null!");
        ImmutableCallSite CS(&I);
        if (CS) {
          //получаю функцию F (CS~f(args))
          auto F = CS.getCalledFunction();
          //если информаци€ по ней уже есть
          //нова€ переменна§ в захвате у л€мбды
          if (InterprocDefInfo != nullptr && (*InterprocDefInfo).count(F)) {
            //инициализаци€
            W = R = AccessInfo::No;
            auto defUseSet = std::move((*InterprocDefInfo)[F]);
            //получаю текущий аргумент
            //const Value *arg = CS.getArgument(Idx);
            // заполн€ю W,R
			MemoryLocationRange arg = MemoryLocationRange::getForArgument(CS, Idx, TLI);
            if ((defUseSet->getDefs()).contain(arg)) {
              W = AccessInfo::Must;;
            } else if ((defUseSet->getMayDefs()).contain(arg)) {
              W = AccessInfo::May;
            }
            if ((defUseSet->getUses()).contain(arg)) {
              R = AccessInfo::Must;;
            }
          }
          else {
            switch (AA.getArgModRefInfo(CS, Idx)) {
            case ModRefInfo::NoModRef: W = R = AccessInfo::No; break;
            case ModRefInfo::Mod: W = AccessInfo::May; R = AccessInfo::No; break;
            case ModRefInfo::Ref: W = AccessInfo::No; R = AccessInfo::May; break;
            case ModRefInfo::ModRef: W = R = AccessInfo::May; break;
            }
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
        auto *AN = AT.findUnknown(I);
        if (!AN)
          return;
        DU->addExplicitUnknown(&I);
        DU->addUnknownInst(&I);
        auto &AA = AT.getAliasAnalysis();
        for_each_alias(&AT, AN, AddUnknownAccessFunctor(AA, DL, I, *DU));
      }
    );
  }
  LLVM_DEBUG(intializeDefUseSetLog(*N, *DU, DFF->getDomTree()));
}


bool DataFlowTraits<ReachDFFwk*>::transferFunction(
  ValueType V, DFNode *N, ReachDFFwk *DFF, GraphType) {
  // Note, that transfer function is never evaluated for the entry node.
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null");
  LLVM_DEBUG(initializeTransferBeginLog(*N, V, DFF->getDomTree()));
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
      LLVM_DEBUG(initializeTransferEndLog(RS->getOut(), true, DFF->getDomTree()));
      return true;
    }
    LLVM_DEBUG(initializeTransferEndLog(RS->getOut(), false, DFF->getDomTree()));
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
    LLVM_DEBUG(initializeTransferEndLog(RS->getOut(), true, DFF->getDomTree()));
    return true;
  }
  LLVM_DEBUG(initializeTransferEndLog(RS->getOut(), false, DFF->getDomTree()));
  return false;
}

void ReachDFFwk::collapse(DFRegion *R) {
  assert(R && "Region must not be null!");
  typedef RegionDFTraits<ReachDFFwk *> RT;
  auto &AT = getAliasTree();
  auto &AA = AT.getAliasAnalysis();
  auto Pair = getDefInfo().insert(std::make_pair(R, std::make_tuple(
    llvm::make_unique<DefUseSet>(), llvm::make_unique<ReachSet>())));
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
    DefUse->addExplicitUnknowns(DU->getExplicitUnknowns());
    for (auto Loc : DU->getAddressAccesses())
      DefUse->addAddressAccess(Loc);
    for (auto Loc : DU->getAddressUnknowns())
      DefUse->addAddressUnknowns(Loc);
    for (auto Inst : DU->getUnknownInsts())
      DefUse->addUnknownInst(Inst);
  }
  LLVM_DEBUG(intializeDefUseSetLog(*R, *DefUse, getDomTree()));
}
