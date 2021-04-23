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
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/SCEVUtils.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/InitializePasses.h>
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
  INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
  INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_END(DefinedMemoryPass, "def-mem",
  "Defined Memory Region Analysis", false, true)

bool llvm::DefinedMemoryPass::runOnFunction(Function & F) {
  auto &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto &AliasTree = getAnalysis<EstimateMemoryPass>().getAliasTree();
  const auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  auto &DI = getAnalysis<DelinearizationPass>().getDelinearizeInfo();
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto &DL = F.getParent()->getDataLayout();
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &GDM = getAnalysis<GlobalDefinedMemoryWrapper>();
  if (GDM) {
    ReachDFFwk ReachDefFwk(AliasTree, TLI, RegionInfo, DT, DI, SE, DL, GO,
                           mDefInfo, *GDM);
    solveDataFlowUpward(&ReachDefFwk, DFF);
  } else {
    ReachDFFwk ReachDefFwk(AliasTree, TLI, RegionInfo, DT, DI, SE, DL, GO,
                           mDefInfo);
    solveDataFlowUpward(&ReachDefFwk, DFF);
  }
  return false;
}

void DefinedMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<GlobalDefinedMemoryWrapper>();
  AU.addRequired<DelinearizationPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
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
  AddAccessFunctor(AAResults &AA, const DataLayout &DL, const DFRegionInfo &DFI,
      const DominatorTree &DT, const Instruction &Inst, DefUseSet &DU) :
    mAA(AA), mDL(DL), mDFI(DFI), mDT(DT), mInst(Inst), mDU(DU) {}

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
  // Return true if a specified memory location is alive when the current
  // instruction is executed.
  bool isAlive(const EstimateMemory &EM) {
    if (auto AI = dyn_cast<AllocaInst>(EM.getTopLevelParent()->front())) {
      DFNode *StartRegion = nullptr, *EndRegion = nullptr;
      for (auto *U1 : AI->users())
        if (auto *BCI = dyn_cast<BitCastInst>(U1))
          for (auto *U2 : BCI->users())
            if (auto *II = dyn_cast<IntrinsicInst>(U2))
              if (II->getIntrinsicID() == llvm::Intrinsic::lifetime_end) {
                if (mDT.dominates(II, &mInst))
                  return false;
                EndRegion = mDFI.getRegionFor(II->getParent());
              } else if (II->getIntrinsicID() ==
                         llvm::Intrinsic::lifetime_start) {
                StartRegion = mDFI.getRegionFor(II->getParent());
              }
      if (StartRegion && EndRegion) {
        while (StartRegion != mDFI.getTopLevelRegion() &&
               ![&EndRegion, &StartRegion]() {
                 auto Tmp = EndRegion;
                 while (Tmp && Tmp != StartRegion)
                   Tmp = Tmp->getParent();
                 return Tmp;
               }())
          StartRegion = StartRegion->getParent();
        if (auto *DFF = dyn_cast<DFFunction>(StartRegion))
          return true;
        if (auto *DFB = dyn_cast<DFBlock>(StartRegion))
          return DFB->getBlock() != mInst.getParent();
        if (auto *DFL = dyn_cast<DFLoop>(StartRegion))
          return DFL->getLoop()->contains(&mInst);
        if (auto *DFR = dyn_cast<DFRegion>(StartRegion))
          return is_contained(DFR->getNodes(),
                              mDFI.getRegionFor(mInst.getParent()));
      }
    }
    return true;
  }

  AAResults &mAA;
  const DataLayout &mDL;
  const DFRegionInfo &mDFI;
  const DominatorTree &mDT;
  const Instruction &mInst;
  DefUseSet &mDU;
};

/// This functor adds into a specified DefUseSet all locations from a specified
/// AliasNode which aliases with a memory accessed by a specified instruction.
class AddUnknownAccessFunctor :
  public AddAccessFunctor<AddUnknownAccessFunctor> {
public:
  AddUnknownAccessFunctor(AAResults &AA, const DataLayout &DL,
      const DFRegionInfo &DFI, const DominatorTree &DT, const Instruction &Inst,
      DefUseSet &DU)
    : AddAccessFunctor<AddUnknownAccessFunctor>(AA, DL, DFI, DT, Inst, DU) {}

  void addEstimate(AliasEstimateNode &N) {
    for (auto &EM : N) {
      if (!EM.isExplicit() || !this->isAlive(EM))
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
};

#ifndef NDEBUG
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     MemoryLocationRange::LocKind Kind) {
  typedef MemoryLocationRange::LocKind LocKind;
  switch (Kind) {
  case LocKind::DEFAULT:
    OS << "DEFAULT";
    break;
  case LocKind::NON_COLLAPSABLE:
    OS << "NON_COLLAPSABLE";
    break;
  case LocKind::COLLAPSED:
    OS << "COLLAPSED";
    break;
  case LocKind::EXPLICIT:
    OS << "EXPLICIT";
    break;
  case LocKind::INVALID_KIND:
    OS << "INVALID_KIND";
    break;
  default:
    OS << "UNKNOWN";
    break;
  }
  return OS;
}
#endif

template<typename FuncT>
void addLocation(DFRegion *R, const MemoryLocationRange &Loc,
    const ReachDFFwk *Fwk, FuncT &&Inserter, bool IgnoreLoopMismatch=false) {
  typedef MemoryLocationRange::Dimension Dimension;
  typedef MemoryLocationRange::LocKind LocKind;
  assert(Fwk && "Data-flow framework must not be null");
  if (Loc.Kind == LocKind::COLLAPSED)
    assert(Loc.DimList.size() > 0 &&
        "Collapsed array location must not be empty!");
  if (Loc.Kind == LocKind::COLLAPSED || Loc.Kind == LocKind::NON_COLLAPSABLE) {
    Inserter(Loc);
    return;
  }
  if (Loc.Kind == LocKind::EXPLICIT)
    return;
  MemoryLocationRange ResLoc(Loc);
  auto LocInfo = Fwk->getDelinearizeInfo()->findRange(Loc.Ptr);
  auto ArrayPtr = LocInfo.first;
  if (!ArrayPtr || !ArrayPtr->isDelinearized() || !LocInfo.second->isValid()) {
    LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Failed to delinearize location.\n");
    ResLoc.Kind = LocKind::DEFAULT;
    Inserter(ResLoc);
    return;
  }
  LLVM_DEBUG(
    for (auto I = 0; I < ArrayPtr->getNumberOfDims(); I++) {
      dbgs() << "[ARRAY LOCATION] Dimension `" << I << "` size: " <<
          *ArrayPtr->getDimSize(I) << "\n";
    }
    dbgs() << "[ARRAY LOCATION] Array info: " <<
      *ArrayPtr->getBase() << ", IsAddress: " <<
      ArrayPtr->isAddressOfVariable() << ".\n";
  );
  auto BaseType = ArrayPtr->getBase()->getType();
  auto ArrayType = cast<PointerType>(BaseType)->getElementType();
  if (ArrayPtr->isAddressOfVariable()) {
    ResLoc.Kind = LocKind::NON_COLLAPSABLE;
    Inserter(ResLoc);
    return;
  }
  LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Array type: " << *ArrayType << ".\n");
  LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Element type: " <<
      ArrayType->getTypeID() << ".\n");
  auto ArraySizeInfo = arraySize(ArrayType);
  if (ArraySizeInfo == std::make_tuple(0, 1, ArrayType) &&
      ArrayPtr->getNumberOfDims() != 1) {
    LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Failed to get array size.\n");
    ResLoc.Kind = LocKind::NON_COLLAPSABLE;
    Inserter(ResLoc);
    return;
  }
  auto ElemType = std::get<2>(ArraySizeInfo);
  LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Array element type:  " <<
      *ElemType << ".\n");
  if (Fwk->getDataLayout().getTypeStoreSize(ElemType).isScalable()) {
    ResLoc.Kind = LocKind::NON_COLLAPSABLE;
    Inserter(ResLoc);
    return;
  }
  if (LocInfo.second->Subscripts.size() != ArrayPtr->getNumberOfDims()) {
    ResLoc.Kind = LocKind::NON_COLLAPSABLE;
    Inserter(ResLoc);
    return;
  }
  size_t DimensionN = 0;
  ResLoc.Ptr = ArrayPtr->getBase();
  ResLoc.LowerBound = 0;
  ResLoc.UpperBound = Fwk->getDataLayout().getTypeStoreSize(ElemType).
      getFixedSize();
  ResLoc.Kind = LocKind::COLLAPSED;
  for (auto *S : LocInfo.second->Subscripts) {
    ResLoc.Kind = LocKind::NON_COLLAPSABLE;
    LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Subscript: " << *S << "\n");
    Dimension DimInfo;
    uint64_t DimSize = 0;
    if (auto *C = dyn_cast<SCEVConstant>(ArrayPtr->getDimSize(DimensionN)))
      DimSize = C->getAPInt().getZExtValue();
    auto *SE = Fwk->getScalarEvolution();
    assert(SE && "ScalarEvolution must be specified!");
    auto AddRecInfo = computeSCEVAddRec(S, *SE);
    if (Fwk->getGlobalOptions().IsSafeTypeCast && !AddRecInfo.second)
      break;
    auto SCEV = AddRecInfo.first;
    if (auto C = dyn_cast<SCEVConstant>(SCEV)) {
      auto Range = C->getAPInt().getSExtValue();
      DimInfo.Start = Range;
      DimInfo.TripCount = 1;
      DimInfo.Step = 1;
      ResLoc.DimList.push_back(DimInfo);
      ResLoc.Kind = LocKind::COLLAPSED;
    } else if (auto C = dyn_cast<SCEVAddRecExpr>(SCEV)) {
      if (!R) {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Region == nullptr.\n");
        ResLoc.Kind = LocKind::DEFAULT;
        break;
      }
      if (auto *DFL = dyn_cast<DFLoop>(R)) {
        assert(DFL->getLoop() && C->getLoop() &&
            "Loops for the DFLoop and AddRecExpr must be specified!");
        if (!C->getLoop()->contains(DFL->getLoop()) && !IgnoreLoopMismatch) {
          LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Invalid matching of loops.\n");
          if (!IgnoreLoopMismatch)
            return;
        }
      } else {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] R is not a loop region.\n");
        if (!IgnoreLoopMismatch)
          return;
      }
      auto *StepSCEV = C->getStepRecurrence(*SE);
      if (!isa<SCEVConstant>(StepSCEV)) {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Non-constant step.\n");
        break;
      }
      auto TripCount = SE->getSmallConstantTripCount(C->getLoop());
      if (TripCount == 0) {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Unknown or non-constant "
                             "trip count.\n");
        break;
      }
      int64_t SignedRangeMin = SE->getSignedRangeMin(SCEV).getSExtValue();
      if (SignedRangeMin < 0) {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Range bounds must be "
                              "non-negative.\n");
        break;
      }
      DimInfo.Start = SignedRangeMin;
      DimInfo.TripCount = TripCount;
      DimInfo.Step = cast<SCEVConstant>(StepSCEV)->getAPInt().getZExtValue();
      if (DimInfo.Start + DimInfo.Step * (DimInfo.TripCount - 1) >= DimSize &&
          DimensionN != 0) {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Array index out of bounds.");
        break;
      }
      ResLoc.DimList.push_back(DimInfo);
      ResLoc.Kind = LocKind::COLLAPSED;
    } else {
      break;
    }
    ++DimensionN;
  }
  if (ResLoc.DimList.size() != ArrayPtr->getNumberOfDims()) {
    LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Collapse incomplete location. Kind: "
        << ResLoc.Kind << ", bounds: [" << Loc.LowerBound << ", " <<
        Loc.UpperBound << "]\n");
    assert(ResLoc.Kind != LocKind::COLLAPSED &&
        "An incomplete array location cannot be of the `collapsed` kind!");
    MemoryLocationRange NewLoc(Loc);
    NewLoc.Kind = ResLoc.Kind;
    Inserter(NewLoc);
    return;
  }
  assert(ResLoc.Kind == LocKind::COLLAPSED &&
      "Array location must be of the `collapsed` kind!");
  LLVM_DEBUG(
    dbgs() << "[ARRAY LOCATION] Dimension info:\n";
    for (size_t I = 0; I < ResLoc.DimList.size(); I++) {
      auto &DimInfo = ResLoc.DimList[I];
      dbgs() << "Dimension: '" << I << "':\n";
      dbgs() << "\tStart:" << DimInfo.Start << "\n";
      dbgs() << "\tTripCount:" << DimInfo.TripCount << "\n";
      dbgs() << "\tStep:" << DimInfo.Step << "\n";
    }
  );
  Inserter(ResLoc);
}

/// This functor adds into a specified DefUseSet all locations from a specified
/// AliasNode which aliases with a specified memory location. Derived classes
/// should be used to specify access modes (Def/MayDef/Use).
template<class ImpTy>
class AddKnownAccessFunctor :
    public AddAccessFunctor<AddKnownAccessFunctor<ImpTy>> {
  using Base = AddAccessFunctor<AddKnownAccessFunctor<ImpTy>>;
public:
  AddKnownAccessFunctor(AAResults &AA, const DataLayout &DL,
      const DFRegionInfo &DFI, const DominatorTree &DT, const Instruction &Inst,
      const MemoryLocation &Loc, DefUseSet &DU, ReachDFFwk *DFF) :
    Base(AA, DL, DFI, DT, Inst, DU),  mLoc(Loc), DFF(DFF) {}

  void addEstimate(AliasEstimateNode &N) {
    for (auto &EM : N) {
      if (!EM.isExplicit() || !this->isAlive(EM))
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
          auto UpperBound =
              mLoc.Size.isPrecise()
                  ? LocationSize::precise(
                      OffsetLoc - OffsetALoc + mLoc.Size.getValue())
                  : mLoc.Size.hasValue()
                        ? LocationSize::upperBound(
                            OffsetLoc - OffsetALoc + mLoc.Size.getValue())
                        : LocationSize::unknown();
          MemoryLocationRange Range(ALoc.Ptr,
            LocationSize::precise(OffsetLoc - OffsetALoc),
            UpperBound, ALoc.AATags);
          if (this->mDU.hasDef(Range))
            continue;
          addMust(Range);
        } else if (!AR.template is<trait::NoAlias>()) {
          addMay(ALoc);
        }
      }
    }
  }

private:
  void addMust(const MemoryLocationRange &Loc) {
    addLocation(nullptr, Loc, DFF,
        [this](auto &Loc) { static_cast<ImpTy *>(this)->addMust(Loc); });
  }

  void addMay(const MemoryLocationRange &Loc) {
    addLocation(nullptr, Loc, DFF,
        [this](auto &Loc) { static_cast<ImpTy *>(this)->addMay(Loc); });
  }

  const MemoryLocation &mLoc;
  ReachDFFwk *DFF;
};

/// This macro determine functors according to a specified memory access modes.
#define ADD_ACCESS_FUNCTOR(NAME, BASE, MEM, MAY, MUST, DFF) \
class NAME : public BASE<NAME> { \
public: \
  NAME(AAResults &AA, const DataLayout &DL, const DFRegionInfo &DFI, \
      const DominatorTree &DT, const Instruction &Inst, \
      MEM &Loc, DefUseSet &DU, ReachDFFwk *DFF) : \
    BASE<NAME>(AA, DL, DFI, DT, Inst, Loc, DU, DFF) {} \
private: \
  friend BASE<NAME>; \
  void addMust(const MemoryLocationRange &Loc) { MUST; } \
  void addMay(const MemoryLocationRange &Loc) { MAY; } \
};

ADD_ACCESS_FUNCTOR(AddDefFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addMayDef(Loc), mDU.addDef(Loc), DFF)
ADD_ACCESS_FUNCTOR(AddMayDefFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addMayDef(Loc), mDU.addMayDef(Loc), DFF)
ADD_ACCESS_FUNCTOR(AddUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation, mDU.addUse(Loc), mDU.addUse(Loc), DFF)
ADD_ACCESS_FUNCTOR(AddDefUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation,
  mDU.addMayDef(Loc); mDU.addUse(Loc), mDU.addDef(Loc); mDU.addUse(Loc), DFF)
ADD_ACCESS_FUNCTOR(AddMayDefUseFunctor, AddKnownAccessFunctor,
  const MemoryLocation,
  mDU.addMayDef(Loc); mDU.addUse(Loc), mDU.addMayDef(Loc); mDU.addUse(Loc), DFF)

#ifndef NDEBUG
void intializeDefUseSetLog(
    const DFNode &N, const DefUseSet &DU, const DominatorTree &DT) {
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
    printLocationSource(dbgs(), Loc, &DT), dbgs() << " (" << Loc.Kind << ")\n";
  dbgs() << "Outward exposed may define locations:\n";
  for (auto &Loc : DU.getMayDefs())
    printLocationSource(dbgs(), Loc, &DT), dbgs() << " (" << Loc.Kind << ")\n";
  dbgs() << "Outward exposed uses:\n";
  for (auto &Loc : DU.getUses())
    printLocationSource(dbgs(), Loc, &DT), dbgs() << " (" << Loc.Kind << ")\n";
  dbgs() << "Explicitly accessed locations:\n";
  for (auto &Loc : DU.getExplicitAccesses())
    printLocationSource(dbgs(), Loc, &DT), dbgs() << "\n";
  for (auto *I : DU.getExplicitUnknowns())
    I->print(dbgs()), dbgs() << "\n";
  dbgs() << "[END DEFUSE]\n";
};

void initializeTransferBeginLog(const DFNode &N, const DefinitionInfo &In,
    const DominatorTree &DT) {
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
  In.MustReach.dump(&DT);
  dbgs() << "MAY REACH DEFINITIONS:\n";
  In.MayReach.dump(&DT);
}

void initializeTransferEndLog(const DefinitionInfo &Out, bool HasChanges,
    const DominatorTree &DT) {
  dbgs() << "OUT ";
  if (HasChanges)
    dbgs() << "with changes:\n";
  else
    dbgs() << "without changes:\n";
  dbgs() << "MUST REACH DEFINITIONS:\n";
  Out.MustReach.dump(&DT);
  dbgs() << "MAY REACH DEFINITIONS:\n";
  Out.MayReach.dump(&DT);
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
    std::make_unique<DefUseSet>(), std::make_unique<ReachSet>())));
  auto *InterDUInfo = DFF->getInterprocDefUseInfo();
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
    auto &DL = I.getModule()->getDataLayout();
    // Any call may access some addresses even if it does not access memory.
    // TODO (kaniandr@gmail.com): use interprocedural analysis to clarify list
    // of unknown address accesses. Now, accesses to global memory
    // in a function call leads to unknown address access.
    if (auto *Call = dyn_cast<CallBase>(&I)) {
      bool UnknownAddressAccess = true;
      auto F = llvm::dyn_cast<Function>(
        Call->getCalledOperand()->stripPointerCasts());
      if (F && InterDUInfo) {
        auto InterDUItr = InterDUInfo->find(F);
        if (InterDUItr != InterDUInfo->end()) {
          auto &DUS = InterDUItr->get<DefUseSet>();
          if (DUS->getAddressUnknowns().empty()) {
            UnknownAddressAccess = false;
            for (auto *Ptr : DUS->getAddressAccesses())
              UnknownAddressAccess |= isa<GlobalValue>(stripPointer(DL, Ptr));
          }
        }
      }
      if (UnknownAddressAccess)
        DU->addAddressUnknowns(&I);
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
    auto &DT = DFF->getDomTree();
    auto &DFI = DFF->getRegionInfo();
    for_each_memory(I, TLI,
      [&DL, &DFI, &DT, &AT, InterDUInfo, &TLI, &DU, DFF](Instruction &I,
          MemoryLocation &&Loc, unsigned Idx, AccessInfo R, AccessInfo W) {
        auto *EM = AT.find(Loc);
        // EM may be smaller than Loc if it is known that access out of the
        // EM size leads to undefined behavior.
        Loc.Size = EM->getSize();
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
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          auto F = llvm::dyn_cast<Function>(
            Call->getCalledOperand()->stripPointerCasts());
          bool InterprocAvailable = false;
          if (F && !F->isVarArg() && InterDUInfo) {
            auto InterDUItr = InterDUInfo->find(F);
            if (InterDUItr != InterDUInfo->end()) {
              InterprocAvailable = true;
              W = R = AccessInfo::No;
              auto &DUS = InterDUItr->get<DefUseSet>();
              auto Arg = F->arg_begin() + Idx;
              MemoryLocationRange ArgLoc(Arg, 0, Loc.Size);
              if (DUS->getDefs().contain(ArgLoc))
                W = AccessInfo::Must;
              else if (DUS->getDefs().overlap(ArgLoc) ||
                       DUS->getMayDefs().overlap(ArgLoc))
                W = AccessInfo::May;
              if (DUS->getUses().overlap(ArgLoc))
                R = AccessInfo::Must;
            }
          }
          if (!InterprocAvailable)
            switch (AA.getArgModRefInfo(Call, Idx)) {
              case ModRefInfo::NoModRef:
                W = R = AccessInfo::No; break;
              case ModRefInfo::Mod:
                W = AccessInfo::May; R = AccessInfo::No; break;
              case ModRefInfo::Ref:
                W = AccessInfo::No; R = AccessInfo::May; break;
              case ModRefInfo::ModRef:
                W = R = AccessInfo::May; break;
            }
        }
        switch (W) {
        case AccessInfo::No:
          if (R != AccessInfo::No)
            for_each_alias(&AT, AN,
                           AddUseFunctor(AA, DL, DFI, DT, I, Loc, *DU, DFF));
          break;
        case AccessInfo::May:
          if (R != AccessInfo::No)
            for_each_alias(&AT, AN,
                           AddMayDefUseFunctor(AA, DL, DFI, DT, I, Loc, *DU,
                                               DFF));
          else
            for_each_alias(&AT, AN,
                           AddMayDefFunctor(AA, DL, DFI, DT, I, Loc, *DU, DFF));
          break;
        case AccessInfo::Must:
          if (R != AccessInfo::No)
            for_each_alias(&AT, AN,
                           AddDefUseFunctor(AA, DL, DFI, DT, I, Loc, *DU, DFF));
          else
            for_each_alias(&AT, AN,
                           AddDefFunctor(AA, DL, DFI, DT, I, Loc, *DU, DFF));
          break;
        }
      },
      [&DL, &DFI, &DT, &AT, &DU, InterDUInfo](Instruction &I, AccessInfo,
          AccessInfo) {
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          auto F = llvm::dyn_cast<Function>(
            Call->getCalledOperand()->stripPointerCasts());
          if (F && InterDUInfo) {
            auto InterDUItr = InterDUInfo->find(F);
            if (InterDUItr != InterDUInfo->end())
              if (isPure(*F, *InterDUItr->get<DefUseSet>()))
                return;
          }
        }
        auto *AN = AT.findUnknown(I);
        if (!AN)
          return;
        DU->addExplicitUnknown(&I);
        DU->addUnknownInst(&I);
        auto &AA = AT.getAliasAnalysis();
          for_each_alias(&AT, AN,
                         AddUnknownAccessFunctor(AA, DL, DFI, DT, I, *DU));
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
    std::make_unique<DefUseSet>(), std::make_unique<ReachSet>())));
  auto &DefUse = Pair.first->get<DefUseSet>();
  assert(DefUse && "Value of def-use attribute must not be null!");
  // ExitingDefs.MustReach is a set of must define locations (Defs) for the
  // loop. These locations always have definitions inside the loop regardless
  // of execution paths of iterations of the loop.
  DFNode *ExitNode = R->getExitNode();
  const DefinitionInfo &ExitingDefs = RT::getValue(ExitNode, this);
  const DefinitionInfo *LatchDefs = nullptr;
  bool HasTrips = false;
  if (auto *DFL = dyn_cast<DFLoop>(R)) {
    LatchDefs = &RT::getValue(DFL->getLatchNode(), this);
    assert(getScalarEvolution() && "ScalarEvolution must be specified!");
    auto TripCount = getScalarEvolution()->getSmallConstantTripCount(
        DFL->getLoop());
    LLVM_DEBUG(dbgs() << "[COLLAPSE] Total trip count: " << TripCount << "\n");
    HasTrips = bool(TripCount > 0);
    LLVM_DEBUG(dbgs() << "[COLLAPSE] HasTrips: " << HasTrips << "\n");
  }
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
    for (auto &Loc : DU->getUses()) {
      LLVM_DEBUG(
        dbgs() << "[COLLAPSE] Check use: ";
        printLocationSource(dbgs(), Loc, &getDomTree());
        dbgs() << "\n";
      );
      bool StartInLoop = false, EndInLoop = false;
      auto *EM = AT.find(Loc);
      if (EM == nullptr)
        continue;
      EM = EM->getTopLevelParent();
      auto *V = EM->front();
      // We're looking for alloca->bitcast->lifetime.start/end instructions
      // in loops to exclude arrays that can be marked private
      if (auto *AI = dyn_cast<AllocaInst>(V)) {
        for (auto *V1 : AI->users()) {
          if (auto *BC = dyn_cast<BitCastInst>(V1)) {
            for (auto *V2 : BC->users()) {
              if (auto *II = dyn_cast<IntrinsicInst>(V2)) {
                auto *BB = II->getParent();
                if (auto *DFL = dyn_cast<DFLoop>(R)) {
                  auto *L = DFL->getLoop();
                  if (L->contains(BB)) {
                    auto ID = II->getIntrinsicID();
                    if (!StartInLoop &&
                        ID == llvm::Intrinsic::lifetime_start) {
                      StartInLoop = true;
                    } else if (!EndInLoop &&
                               ID == llvm::Intrinsic::lifetime_end) {
                      EndInLoop = true;
                    }
                    if (StartInLoop && EndInLoop)
                      break;
                  }
                }
              }
            }
          }
          if (StartInLoop && EndInLoop)
            break;
        }
      }
      if (StartInLoop && EndInLoop)
        continue;
      MemoryLocationRange NewLoc(Loc);
      auto &MustReachIn = RS->getIn().MustReach;
      if (NewLoc.DimList.empty()) {
        if (!MustReachIn.contain(NewLoc)) {
          LLVM_DEBUG(dbgs() << "[COLLAPSE] Collapse Use location.\n");
          addLocation(R, NewLoc, this,
              [&NewLoc](auto &Loc){ NewLoc = Loc; });
          if (NewLoc.DimList.empty()) {
            DefUse->addUse(NewLoc);
          }
        }
      }
      if (!NewLoc.DimList.empty()) {
        SmallVector<MemoryLocationRange, 4> UseDiff;
        if (MustReachIn.subtractFrom(NewLoc, UseDiff)) {
          for (auto &L : UseDiff) {
            LLVM_DEBUG(dbgs() << "[COLLAPSE] Add Use location.\n");
            addLocation(R, L, this,
                [&DefUse](auto &Loc){ DefUse->addUse(Loc); });
          }
        } else {
          LLVM_DEBUG(dbgs() << "[COLLAPSE] Add unbroken Use location.\n");
          addLocation(R, NewLoc, this,
                [&DefUse](auto &Loc){ DefUse->addUse(Loc); });
        }
      }
    }
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
    llvm::SmallVector<MemoryLocationRange, 4> Locs;
    for (auto &Loc : DU->getDefs()) {
      auto *DFL = dyn_cast<DFLoop>(R);
      if (DFL && HasTrips) {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Check LatchDefs.\n");
        LatchDefs->MustReach.findCoveredBy(Loc, Locs);
        if (!Locs.empty()) {
          LLVM_DEBUG(dbgs() <<
              "[ARRAY LOCATION] Add location from a latch node.\n");
          for (auto &L : Locs) {
            addLocation(R, L, this,
                [&DefUse](auto &Loc){ DefUse->addDef(Loc); });
          } 
        } else {
          LLVM_DEBUG(dbgs() <<
              "[COLLAPSE] Collapse MayDef location of a latch node.\n");
          addLocation(R, Loc, this,
              [&DefUse](auto &Loc){ DefUse->addMayDef(Loc); }, true);
        }
      } else {
        LLVM_DEBUG(dbgs() << "[ARRAY LOCATION] Check ExitingDefs.\n");
        ExitingDefs.MustReach.findCoveredBy(Loc, Locs);
        if (!Locs.empty()) {
          LLVM_DEBUG(dbgs() << "[COLLAPSE] Collapse Def location.\n");
          for (auto &L : Locs) {
            addLocation(R, L, this,
                [&DefUse](auto &Loc){ DefUse->addDef(Loc); });
          }
        } else {
          LLVM_DEBUG(dbgs() << "[COLLAPSE] Collapse MayDef location.\n");
          addLocation(R, Loc, this,
              [&DefUse](auto &Loc){ DefUse->addMayDef(Loc); }, true);
        }
      }
    }
    for (auto &Loc : DU->getMayDefs()) {
      LLVM_DEBUG(dbgs() << "[COLLAPSE] Collapse MayDef location.\n");
      addLocation(R, Loc, this,
          [&DefUse](auto &Loc){ DefUse->addMayDef(Loc); }, true);
    }
    DefUse->addExplicitAccesses(DU->getExplicitAccesses());
    DefUse->addExplicitUnknowns(DU->getExplicitUnknowns());
    for (auto Loc : DU->getAddressAccesses())
      DefUse->addAddressAccess(Loc);
    for (auto Loc : DU->getAddressUnknowns())
      DefUse->addAddressUnknowns(Loc);
    for (auto Inst : DU->getUnknownInsts())
      DefUse->addUnknownInst(Inst);
  }
  for (auto &Loc : DefUse->getExplicitAccesses()) {
    LLVM_DEBUG(dbgs() << "[COLLAPSE] Collapse Explicit Accesses.\n");
    MemoryLocationRange ExpLoc;
    addLocation(R, Loc, this, [&ExpLoc](auto &Loc){ ExpLoc = Loc; },
                      true);
    if (ExpLoc.Kind != MemoryLocationRange::LocKind::COLLAPSED)
      continue;
    MemoryLocationRange NewLoc(Loc);
    NewLoc.Kind = MemoryLocationRange::LocKind::EXPLICIT;
    if (DefUse->getDefs().contain(ExpLoc))
      DefUse->addDef(NewLoc);
    if (DefUse->getMayDefs().overlap(ExpLoc))
      DefUse->addMayDef(NewLoc);
    if (DefUse->getUses().overlap(ExpLoc))
      DefUse->addUse(NewLoc);
  }
  LLVM_DEBUG(intializeDefUseSetLog(*R, *DefUse, getDomTree()));
}
