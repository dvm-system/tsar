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
#include "tsar/Support/GlobalOptions.h"
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

/// If memory location `Loc` is associated with an array, collect information
/// about array accesses, and create new memory location `ResLoc`. If
/// information was collected successfully and meets certain
/// conditions, `ResLoc.DimList` will be filled with information about the
/// dimensions of the array, and the `ResLoc.Kind` will be set to `Collapsed`.
/// Otherwise, the `ResLoc.Kind` will be set to `NonCollapsable`.
///
/// If `R == nullptr`, this means that the collection of information is about a
/// single array access within the basic block, this is used in
/// `DataFlowTraits<ReachDFFwk*>::initialize()`.
///
/// Return a pair consisting of an aggregated memory location and boolean value.
/// If returned location is of kind `Collapsed` and does not belong to the
/// loop nest associated with region `R`, this value will be set to `false`,
/// `true` otherwise.
std::pair<MemoryLocationRange, bool> aggregate(
    DFRegion *R, const MemoryLocationRange &Loc, const ReachDFFwk *Fwk) {
  typedef MemoryLocationRange::Dimension Dimension;
  typedef MemoryLocationRange::LocKind LocKind;
  assert(Fwk && "Data-flow framework must not be null");
  assert(!(Loc.Kind & LocKind::Collapsed) || Loc.DimList.size() > 0 &&
      "Collapsed array location must not be empty!");
  // We will try to aggregate location if it is of kind `Default` only.
  if (Loc.Kind != LocKind::Default)
    return std::make_pair(Loc, true);
  MemoryLocationRange ResLoc(Loc);
  auto LocInfo = Fwk->getDelinearizeInfo()->findRange(Loc.Ptr);
  auto ArrayPtr = LocInfo.first;
  if (!ArrayPtr || !ArrayPtr->isDelinearized() || !LocInfo.second->isValid()) {
    LLVM_DEBUG(dbgs() << "[AGGREGATE] Failed to delinearize location.\n");
    ResLoc.Kind = LocKind::Default;
    return std::make_pair(ResLoc, true);
  }
  LLVM_DEBUG(dbgs() << "[AGGREGATE] Array info: " << *ArrayPtr->getBase() <<
      ", IsAddress: " << ArrayPtr->isAddressOfVariable() << ".\n");
  auto ArrayType{getPointerElementType(*ArrayPtr->getBase())};
  if (!ArrayType || ArrayPtr->isAddressOfVariable()) {
    ResLoc.Kind = LocKind::NonCollapsable;
    return std::make_pair(ResLoc, true);
  }
  LLVM_DEBUG(dbgs() << "[AGGREGATE] Array type: " << *ArrayType << ".\n");
  LLVM_DEBUG(dbgs() << "[AGGREGATE] Element type: " <<
      ArrayType->getTypeID() << ".\n");
  auto ArraySizeInfo = arraySize(ArrayType);
  if (ArraySizeInfo == std::make_tuple(0, 1, ArrayType) &&
      ArrayPtr->getNumberOfDims() != 1) {
    LLVM_DEBUG(dbgs() << "[AGGREGATE] Failed to get array size.\n");
    ResLoc.Kind = LocKind::NonCollapsable;
    return std::make_pair(ResLoc, true);
  }
  auto ElemType = std::get<2>(ArraySizeInfo);
  LLVM_DEBUG(dbgs() << "[AGGREGATE] Array element type:  " <<
      *ElemType << ".\n");
  if (Fwk->getDataLayout().getTypeStoreSize(ElemType).isScalable() ||
      LocInfo.second->Subscripts.size() != ArrayPtr->getNumberOfDims() ||
      !Loc.LowerBound.hasValue() || !Loc.UpperBound.hasValue() ||
      Loc.LowerBound.getValue() != 0 ||
      Loc.UpperBound.getValue() != Fwk->getDataLayout().
                                   getTypeStoreSize(ElemType).getFixedSize()) {
    ResLoc.Kind = LocKind::NonCollapsable;
    return std::make_pair(ResLoc, true);
  }
  ResLoc.Ptr = ArrayPtr->getBase();
  ResLoc.LowerBound = 0;
  ResLoc.UpperBound = Fwk->getDataLayout().getTypeStoreSize(ElemType).
      getFixedSize();
  ResLoc.Kind = LocKind::Collapsed;
  auto *SE = Fwk->getScalarEvolution();
  assert(SE && "ScalarEvolution must be specified!");
  std::size_t DimensionN = 0;
  std::size_t LoopMatchCount = 0, ConstMatchCount = 0;
  for (auto *S : LocInfo.second->Subscripts) {
    ResLoc.Kind = LocKind::NonCollapsable;
    LLVM_DEBUG(dbgs() << "[AGGREGATE] Subscript: " << *S << "\n");
    if (!ArrayPtr->isKnownDimSize(DimensionN) && DimensionN != 0) {
      LLVM_DEBUG(dbgs() << "[AGGREGATE] Failed to get dimension "
                           "size for required dimension `" << DimensionN <<
                           "`.\n");
      break;
    }
    Dimension DimInfo;
    if (auto *C = dyn_cast<SCEVConstant>(ArrayPtr->getDimSize(DimensionN))) {
      DimInfo.DimSize = C->getAPInt().getZExtValue();
    } else {
      LLVM_DEBUG(dbgs() << "[AGGREGATE] Non-constant dimension size.\n");
      if (DimensionN != 0)
        break;
    }
    auto AddRecInfo = computeSCEVAddRec(S, *SE);
    if (Fwk->getGlobalOptions().IsSafeTypeCast && !AddRecInfo.second)
      break;
    auto SCEV = AddRecInfo.first;
    if (auto C = dyn_cast<SCEVConstant>(SCEV)) {
      if (C->getAPInt().isNegative())
        break;
      ++ConstMatchCount;
      DimInfo.Start = C->getAPInt().getSExtValue();
      DimInfo.TripCount = 1;
      DimInfo.Step = 1;
      ResLoc.DimList.push_back(DimInfo);
      ResLoc.Kind = LocKind::Collapsed;
    } else if (auto C = dyn_cast<SCEVAddRecExpr>(SCEV)) {
      if (!R) {
        LLVM_DEBUG(dbgs() << "[AGGREGATE] Region == nullptr.\n");
        ResLoc.Kind = LocKind::Default;
        break;
      }
      if (auto *DFL = dyn_cast<DFLoop>(R)) {
        assert(DFL->getLoop() && C->getLoop() &&
            "Loops for the DFLoop and AddRecExpr must be specified!");
        if (DFL->getLoop()->contains(C->getLoop()))
          ++LoopMatchCount;
      }
      auto *StepSCEV = C->getStepRecurrence(*SE);
      if (!isa<SCEVConstant>(StepSCEV)) {
        LLVM_DEBUG(dbgs() << "[AGGREGATE] Non-constant step.\n");
        break;
      }
      auto TripCount = SE->getSmallConstantTripCount(C->getLoop());
      if (TripCount <= 1) {
        LLVM_DEBUG(dbgs() << "[AGGREGATE] Unknown or non-constant "
                             "trip count.\n");
        break;
      }
      int64_t SignedRangeMin = SE->getSignedRangeMin(SCEV).getSExtValue();
      if (SignedRangeMin < 0) {
        LLVM_DEBUG(dbgs() << "[AGGREGATE] Range bounds must be "
                              "non-negative.\n");
        break;
      }
      DimInfo.Start = SignedRangeMin;
      DimInfo.TripCount = TripCount - 1;
      DimInfo.Step = std::abs(cast<SCEVConstant>(StepSCEV)->
                              getAPInt().getSExtValue());
      if (DimInfo.Start + DimInfo.Step * (DimInfo.TripCount - 1) >=
          DimInfo.DimSize && DimensionN != 0) {
        LLVM_DEBUG(dbgs() << "[AGGREGATE] Array index out of bounds.");
        break;
      }
      ResLoc.DimList.push_back(DimInfo);
      ResLoc.Kind = LocKind::Collapsed;
    } else {
      break;
    }
    ++DimensionN;
  }
  if (ResLoc.DimList.size() != ArrayPtr->getNumberOfDims()) {
    LLVM_DEBUG(dbgs() << "[AGGREGATE] Collapse incomplete location. Kind: "
        << ResLoc.getKindAsString() << "\n");
    assert(ResLoc.Kind != LocKind::Collapsed &&
        "An incomplete array location cannot be of the `collapsed` kind!");
    MemoryLocationRange NewLoc(Loc);
    NewLoc.Kind = ResLoc.Kind;
    return std::make_pair(NewLoc, true);
  }
  bool IsOwn = true;
  if (isa_and_nonnull<DFLoop>(R)) {
    if (LoopMatchCount == 0 && ConstMatchCount != ArrayPtr->getNumberOfDims()) {
      IsOwn = false;
    } else if (LoopMatchCount > 0 &&
               LoopMatchCount + ConstMatchCount < ArrayPtr->getNumberOfDims()) {
      return std::make_pair(Loc, true);
    }
  }
  assert(ResLoc.Kind == LocKind::Collapsed &&
      "Array location must be of the `collapsed` kind!");
  LLVM_DEBUG(
    dbgs() << "[AGGREGATE] Aggregated location: ";
    printLocationSource(dbgs(), ResLoc, &Fwk->getDomTree(), true);
    dbgs() << "\n";
  );
  return std::make_pair(ResLoc, IsOwn);
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
          auto BaseLoc{
              GetPointerBaseWithConstantOffset(mLoc.Ptr, OffsetLoc, this->mDL)};
          auto BaseALoc{GetPointerBaseWithConstantOffset(ALoc.Ptr, OffsetALoc,
                                                         this->mDL)};
          MemoryLocationRange Range;
          if (BaseLoc == BaseALoc) {
            // Base - OffsetLoc --------------|mLoc.Ptr| --- mLoc.Size --- |
            // Base - OffsetALoc -|ALoc.Ptr| ---- ALoc.Size --------------------
            // |
            //--------------------|ALoc.Ptr|--| ------ addMust(...) ------ |
            auto UpperBound =
                mLoc.Size.isPrecise()
                    ? LocationSize::precise(OffsetLoc - OffsetALoc +
                                            mLoc.Size.getValue())
                : mLoc.Size.hasValue()
                    ? LocationSize::upperBound(OffsetLoc - OffsetALoc +
                                               mLoc.Size.getValue())
                    : mLoc.Size;
            Range = MemoryLocationRange{
                ALoc.Ptr, LocationSize::precise(OffsetLoc - OffsetALoc),
                UpperBound, ALoc.AATags};
          } else {
            Range = MemoryLocationRange{ALoc.Ptr, 0, mLoc.Size , ALoc.AATags};
          }
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
    static_cast<ImpTy *>(this)->addMust(aggregate(nullptr, Loc, DFF).first);
    static_cast<ImpTy *>(this)->addMust(Loc);
  }

  void addMay(const MemoryLocationRange &Loc) {
    static_cast<ImpTy *>(this)->addMay(aggregate(nullptr, Loc, DFF).first);
    static_cast<ImpTy *>(this)->addMay(Loc);
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
    printLocationSource(dbgs(), Loc, &DT, true), dbgs() << "\n";
  dbgs() << "Outward exposed may define locations:\n";
  for (auto &Loc : DU.getMayDefs())
    printLocationSource(dbgs(), Loc, &DT, true), dbgs() << "\n";
  dbgs() << "Outward exposed uses:\n";
  for (auto &Loc : DU.getUses())
    printLocationSource(dbgs(), Loc, &DT, true), dbgs() << "\n";
  dbgs() << "Explicitly accessed locations:\n";
  for (auto &Loc : DU.getExplicitAccesses())
    printLocationSource(dbgs(), Loc, &DT, true), dbgs() << "\n";
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
      } else if (auto *GV{dyn_cast<GlobalValue>(V)};
                 GV && GV->hasGlobalUnnamedAddr()) {
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
    /// Is it safe to ignore intrinsics here? It seems that all intrinsics in
    /// LLVM does not use addresses to perform computations instead of
    /// memory accesses. We also ignore intrinsics in PrivateAnalysisPass.
    if (auto *Call = dyn_cast<CallBase>(&I); Call && !isa<IntrinsicInst>(I)) {
      bool UnknownAddressAccess = true;
      auto F = llvm::dyn_cast<Function>(
        Call->getCalledOperand()->stripPointerCasts());
      if (F && InterDUInfo) {
        auto InterDUItr = InterDUInfo->find(F);
        if (InterDUItr != InterDUInfo->end()) {
          auto &DUS = InterDUItr->get<DefUseSet>();
          if (DUS->getAddressUnknowns().empty()) {
            UnknownAddressAccess = false;
            for (auto *Ptr : DUS->getAddressAccesses()) {
              if (auto *GV{
                      dyn_cast<GlobalValue>(getUnderlyingObject(Ptr, 0))})
                if (AT.find(MemoryLocation(GV, LocationSize::precise(0))))
                  DU->addAddressTransitives(GV, &I);
              else UnknownAddressAccess = true;
            }
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
    auto &AA = AT.getAliasAnalysis();
    auto add = [&AT, &AA, &DL, &DFI, &DT, &DU,
                DFF](AliasNode *AN, const Instruction &I,
                     const MemoryLocation &Loc, AccessInfo W, AccessInfo R) {
      switch (W) {
      case AccessInfo::No:
        if (R != AccessInfo::No)
          for_each_alias(&AT, AN,
                         AddUseFunctor(AA, DL, DFI, DT, I, Loc, *DU, DFF));
        break;
      case AccessInfo::May:
        if (R != AccessInfo::No)
          for_each_alias(
              &AT, AN, AddMayDefUseFunctor(AA, DL, DFI, DT, I, Loc, *DU, DFF));
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
    };
    for_each_memory(I, TLI,
      [&AA, &DL, &AT, InterDUInfo, &DU, &add](Instruction &I,
          MemoryLocation &&Loc, unsigned Idx, AccessInfo R, AccessInfo W) {
        auto *EM = AT.find(Loc);
        // EM may be smaller than Loc if it is known that access out of the
        // EM size leads to undefined behavior.
        Loc.Size = EM->getSize();
        assert(EM && "Estimate memory location must not be null!");
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
        add(AN, I, Loc, W, R);
      },
      [&DL, &DFI, &DT, &AT, &DU, InterDUInfo, &add](Instruction &I, AccessInfo,
          AccessInfo) {
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          auto F = llvm::dyn_cast<Function>(
            Call->getCalledOperand()->stripPointerCasts());
          if (F && InterDUInfo)
            if (auto InterDUItr{InterDUInfo->find(F)};
                InterDUItr != InterDUInfo->end()) {
              auto [IsPure, GlobalsOnly] =
                  isPure(*F, *InterDUItr->get<DefUseSet>());
              if (IsPure)
                return;
              if (GlobalsOnly) {
                bool HasUnknownAccess{false};
                for (auto &Loc :
                     InterDUItr->get<DefUseSet>()->getExplicitAccesses())
                  if (auto *GV{dyn_cast<GlobalValue>(
                          getUnderlyingObject(Loc.Ptr, 0))}) {
                    auto EM{
                        AT.find(MemoryLocation(GV, LocationSize::precise(0)))};
                    if (!EM) {
                      HasUnknownAccess = true;
                      continue;
                    }
                    EM = EM->getTopLevelParent();
                    AccessInfo W{AccessInfo::No}, R{AccessInfo::No};
                    assert(!EM->isAmbiguous() &&
                           "Global value cannot be ambiguous!");
                    MemoryLocation Loc{EM->front(), EM->getSize(),
                                       EM->getAAInfo()};
                    if (InterDUItr->get<DefUseSet>()->hasDef(Loc))
                      W = AccessInfo::Must;
                    else if (InterDUItr->get<DefUseSet>()->hasMayDef(Loc))
                      W = AccessInfo::May;
                    if (InterDUItr->get<DefUseSet>()->hasUse(Loc))
                      R = AccessInfo::Must;
                    add(EM->getAliasNode(AT), I, Loc, W, R);
                  }
                if (!HasUnknownAccess)
                  return;
              }
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
  typedef std::pair<MemoryLocationRange, bool> AggrResult;
  typedef std::pair<MemoryLocationRange, MemoryLocationRange> LocationPair;
  typedef MemorySet<MemoryLocationRange> LocationSet;
  typedef llvm::SmallVector<LocationPair, 4> LocationList;
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
  }
  // Distribute memory locations into two collections: a set of own memory
  // locations of the region and a list of other memory locations.
  auto distribute = [](const AggrResult &Res, const MemoryLocationRange &OrigLoc,
                       LocationSet &Own, LocationList &Other) {
    if (Res.second)
      Own.insert(Res.first);
    else
      Other.push_back(std::make_pair(OrigLoc, Res.first));
  };
  #ifndef NDEBUG
  auto printLocInfo = [this](llvm::StringRef Msg,
                             const MemoryLocationRange &Loc) {
    dbgs() << Msg;
    printLocationSource(dbgs(), Loc, &getDomTree());
    dbgs() << " (" << Loc.getKindAsString() << ")\n";
  };
  #endif
  for (DFNode *N : R->getNodes()) {
    auto DefItr = getDefInfo().find(N);
    assert(DefItr != getDefInfo().end() &&
      DefItr->get<ReachSet>() && DefItr->get<DefUseSet>() &&
      "Data-flow value must be specified!");
    auto &RS = DefItr->get<ReachSet>();
    auto &DU = DefItr->get<DefUseSet>();
    auto &MustReachIn = RS->getIn().MustReach;
    LocationSet OwnUses, OwnMayDefs, OwnDefs;
    LocationList OtherUses, OtherMayDefs, OtherDefs;
    // We calculate a set of locations (Uses)
    // which get values outside the loop or from previous loop iterations.
    // These locations can not be privatized.
    for (auto &Loc : DU->getUses()) {
      if (Loc.Kind & MemoryLocationRange::LocKind::Hint)
        continue;
      auto *EM = AT.find(Loc);
      if (auto *DFL{dyn_cast<DFLoop>(R)}; DFL && EM) {
        EM = EM->getTopLevelParent();
        if (pointsToLocalMemory(*EM->front(), *DFL->getLoop()))
          continue;
      }
      LLVM_DEBUG(printLocInfo("[COLLAPSE] Collapse Use location: ", Loc));
      distribute(aggregate(R, Loc, this), Loc, OwnUses, OtherUses);
    }
    OwnUses.clarify(OtherUses);
    for (auto &Loc : OwnUses) {
      SmallVector<MemoryLocationRange, 4> UseDiff;
      if (MustReachIn.subtractFrom(Loc, UseDiff)) {
        for (auto &L : UseDiff) {
          LLVM_DEBUG(printLocInfo("[COLLAPSE] Add Use location: ", L));
          DefUse->addUse(L);
        }
      } else {
        LLVM_DEBUG(printLocInfo("[COLLAPSE] Add entire Use location: ", Loc));
        DefUse->addUse(Loc);
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
    //
    // When calculating a set of must define locations (Defs), we need to
    // consider only those definitions that reach the exit from the current
    // region. We cannot just insert all location from the nested regions into
    // Defs. Consider the following example.
    // for (;;) { // R1
    //   if (...) {
    //     for (int I = 0; I < 6; ++I) // R2
    //       A[I] = ...
    //   } else {
    //     for (int J = 4; J < 10; ++J) // R3
    //       A[J] = ...
    //   }
    // }
    // `A[0, 6)` has a definition in `R2`, `A[4, 10)` has a definition in `R3`,
    // but only `A[4, 6)` reaches the exit from `R1`. We need to use
    // `ExitingDefs.MustReach.findCoveredBy(Loc, Locs)` to find those parts of
    // a specified memory location that reach the exit from the current region.
    // If `R1` is a loop region and it has a trip count greater than zero, then
    // we need to check `LatchDefs` in addition to `ExitingDefs`.
    for (auto &Loc : DU->getDefs()) {
      if (Loc.Kind & MemoryLocationRange::LocKind::Hint)
        continue;
      if (isa<DFLoop>(R) && HasTrips) {
        LLVM_DEBUG(dbgs() << "[COLLAPSE] Check LatchDefs.\n");
        llvm::SmallVector<MemoryLocationRange, 4> Locs;
        LatchDefs->MustReach.findCoveredBy(Loc, Locs);
        if (!Locs.empty()) {
          for (auto &L : Locs) {
            LLVM_DEBUG(printLocInfo(
                "[COLLAPSE] Add location from a latch node: ", L));
            distribute(aggregate(R, L, this), Loc, OwnDefs, OtherDefs);
          }
        } else {
          LLVM_DEBUG(printLocInfo(
              "[COLLAPSE] Collapse MayDef location of a latch node: ", Loc));
          distribute(aggregate(R, Loc, this), Loc, OwnMayDefs, OtherMayDefs);
        }
      }
      LLVM_DEBUG(dbgs() << "[COLLAPSE] Check ExitingDefs.\n");
      llvm::SmallVector<MemoryLocationRange, 4> Locs;
      ExitingDefs.MustReach.findCoveredBy(Loc, Locs);
      if (!Locs.empty()) {
        for (auto &L : Locs) {
          LLVM_DEBUG(printLocInfo("[COLLAPSE] Collapse Def location: ", L));
          distribute(aggregate(R, L, this), Loc, OwnDefs, OtherDefs);
        }
      } else {
        LLVM_DEBUG(printLocInfo("[COLLAPSE] Collapse MayDef location: ", Loc));
        distribute(aggregate(R, Loc, this), Loc, OwnMayDefs, OtherMayDefs);
      }
    }
    OwnDefs.clarify(OtherDefs);
    DefUse->addDefs(OwnDefs);
    for (auto &Loc : DU->getMayDefs()) {
      if (Loc.Kind & MemoryLocationRange::LocKind::Hint)
        continue;
      LLVM_DEBUG(printLocInfo("[COLLAPSE] Collapse MayDef location: ", Loc));
      distribute(aggregate(R, Loc, this), Loc, OwnMayDefs, OtherMayDefs);
    }
    OwnMayDefs.clarify(OtherMayDefs);
    DefUse->addMayDefs(OwnMayDefs);
    DefUse->addExplicitAccesses(DU->getExplicitAccesses());
    DefUse->addExplicitUnknowns(DU->getExplicitUnknowns());
    for (auto Loc : DU->getAddressAccesses())
      DefUse->addAddressAccess(Loc);
    for (auto &&[Loc, Insts] : DU->getAddressTransitives())
      for (auto *I : Insts)
        DefUse->addAddressTransitives(Loc, I);
    for (auto Loc : DU->getAddressUnknowns())
      DefUse->addAddressUnknowns(Loc);
    for (auto Inst : DU->getUnknownInsts())
      DefUse->addUnknownInst(Inst);
  }
  for (auto &Loc : DefUse->getExplicitAccesses()) {
    auto *EM{AT.find(Loc)};
    if (EM) {
      if (auto *DFL{dyn_cast<DFLoop>(R)})
        if (auto *GEP{dyn_cast<GetElementPtrInst>(EM->front())})
          if (all_of(GEP->indices(), [this, DFL](auto &Idx) {
                auto *SE{getScalarEvolution()};
                auto *S{SE->getSCEV(Idx)};
                return SE->isLoopInvariant(S, DFL->getLoop());
              }))
            EM = nullptr;
          else
            EM = EM->getTopLevelParent();
    }
    LLVM_DEBUG(printLocInfo("[COLLAPSE] Collapse Explicit Access: ", Loc));
    MemoryLocationRange ExpLoc = aggregate(R, Loc, this).first;
    MemoryLocationRange NewLoc(Loc);
    NewLoc.Kind |= MemoryLocationRange::LocKind::Hint;
    // We use top-level parent (EM) if it is available to update def-use set.
    // In the following example  `for (int I = 98; I >= 0; --I)  A[I] = A[I + 1];`
    // `A[I]` does not alias `A[I+1]` because these are two different elements
    // of array if a value of `I` is specified. In this case `A[I]` is not presented
    // in  the `Use` set, so analysis recognizes `A` as privitizable. To overcome
    // this issue we conservatively update def-use set according to accesses
    // to the whole array `A`.
    if (!DefUse->hasDef(Loc) &&
        (ExpLoc.Kind == MemoryLocationRange::LocKind::Collapsed &&
         DefUse->hasDef(ExpLoc)))
      DefUse->addDef(NewLoc);
    if (!DefUse->hasMayDef(Loc) &&
        ((ExpLoc.Kind == MemoryLocationRange::LocKind::Collapsed &&
          DefUse->hasMayDef(ExpLoc)) ||
         (EM && any_of(*EM, [EM, &DefUse](auto *Ptr) {
            return DefUse->hasMayDef(
                MemoryLocation{Ptr, EM->getSize(), EM->getAAInfo()});
          }))))
      DefUse->addMayDef(NewLoc);
    if (!DefUse->hasUse(Loc) &&
        ((ExpLoc.Kind == MemoryLocationRange::LocKind::Collapsed &&
          DefUse->hasUse(ExpLoc)) ||
         (EM && any_of(*EM, [EM, &DefUse](auto *Ptr) {
            return DefUse->hasUse(
                MemoryLocation{Ptr, EM->getSize(), EM->getAAInfo()});
          }))))
      DefUse->addUse(NewLoc);
  }
  LLVM_DEBUG(intializeDefUseSetLog(*R, *DefUse, getDomTree()));
}
