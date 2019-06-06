//===- Delinearization.cpp -- Delinearization Engine ------------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
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
// This file allows to perform metadata-based delinearization of array accesses.
//
//===----------------------------------------------------------------------===/

#include "Delinearization.h"
#include "DelinearizeJSON.h"
#include "GlobalOptions.h"
#include "KnownFunctionTraits.h"
#include "MemoryAccessUtils.h"
#include "tsar_query.h"
#include "tsar_utility.h"
#include "tsar/Support/SCEVUtils.h"
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils/Local.h>
#include <bcl/Json.h>
#include <utility>
#include <cmath>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "delinearize"

char DelinearizationPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(DelinearizationPass, "delinearize",
  "Array Access Delinearizer", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(DelinearizationPass, "delinearize",
  "Array Access Delinearizer", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

std::pair<const Array *, const Array::Range *>
DelinearizeInfo::findRange(const Value *ElementPtr) const {
  auto Itr = mRanges.find(ElementPtr);
  if (Itr != mRanges.end()) {
    auto *TargetArray = Itr->getArray();
    auto *TargetElement = TargetArray->getRange(Itr->getElementIdx());
    return std::make_pair(TargetArray, TargetElement);
  }
  return std::make_pair(nullptr, nullptr);
}

void DelinearizeInfo::updateRangeCache() {
  mRanges.clear();
  for (auto &ArrayEntry : mArrays) {
    int Idx = 0;
    for (auto Itr = ArrayEntry->begin(), ItrE = ArrayEntry->end();
         Itr != ItrE; ++Itr, ++Idx)
      mRanges.try_emplace(Itr->Ptr, ArrayEntry, Idx);
  }
}

namespace {
template<class GEPItrT>
void extractSubscriptsFromGEPs(
    const GEPItrT &GEPBeginItr, const GEPItrT &GEPEndItr,
    SmallVectorImpl<Value *> &Idxs) {
  for (auto *GEP : make_range(GEPBeginItr, GEPEndItr)) {
    unsigned NumOperands = GEP->getNumOperands();
    if (NumOperands == 2) {
      Idxs.push_back(GEP->getOperand(1));
    } else {
      if (auto *SecondOp = dyn_cast<Constant>(GEP->getOperand(1))) {
        if (!SecondOp->isZeroValue())
          Idxs.push_back(GEP->getOperand(1));
      } else {
        Idxs.push_back(GEP->getOperand(1));
      }
      for (unsigned I = 2; I < NumOperands; ++I) {
        Idxs.push_back(GEP->getOperand(I));
      }
    }
  }
}

std::pair<Value *, bool> GetUnderlyingArray(Value *Ptr, const DataLayout &DL) {
  auto BasePtrInfo = GetUnderlyingObjectWithMetadata(Ptr, DL);
  if (!BasePtrInfo.second && isa<LoadInst>(BasePtrInfo.first))
    Ptr = cast<LoadInst>(BasePtrInfo.first)->getPointerOperand();
  else
    Ptr = BasePtrInfo.first;
  bool IsAddressOfVariable = (Ptr != BasePtrInfo.first);
  return std::make_pair(Ptr, IsAddressOfVariable);
}

#ifdef LLVM_DEBUG
void delinearizationLog(const DelinearizeInfo &Info, ScalarEvolution &SE,
    bool IsSafeTypeCast, raw_ostream  &OS) {
  for (auto &ArrayInfo : Info.getArrays()) {
    OS << "[DELINEARIZE]: results for array ";
    ArrayInfo->getBase()->print(OS, true);
    OS << "\n";
    OS << "  number of dimensions: " << ArrayInfo->getNumberOfDims() << "\n";
    for (std::size_t I = 0, EI = ArrayInfo->getNumberOfDims(); I < EI; ++I) {
      OS << "    " << I << ": ";
      ArrayInfo->getDimSize(I)->print(OS);
      OS << "\n";
    }
    OS << "  accesses:\n";
    for (auto &Range : *ArrayInfo) {
      OS << "    address: ";
      Range.Ptr->print(OS, true);
      OS << "\n";
      for (auto *S : Range.Subscripts) {
        OS << "      SCEV: ";
        S->print(OS);
        OS << "\n";
        auto Info = computeSCEVAddRec(S, SE);
        const SCEV *Coef, *ConstTerm;
        if ((!IsSafeTypeCast || Info.second) &&
            isa<SCEVAddRecExpr>(Info.first)) {
          auto AddRec = cast<SCEVAddRecExpr>(Info.first);
          Coef = AddRec->getStepRecurrence(SE);
          ConstTerm = AddRec->getStart();
        } else {
          Coef = SE.getZero(S->getType());
          ConstTerm = S;
        }
        OS << "      a: ";
        Coef->print(OS);
        OS << "\n";
        OS << "      b: ";
        ConstTerm->print(OS);
        OS << "\n";
        if (!Info.second)
          OS << "      with unsafe cast\n";
      }
    }
  }
}
#endif
}

const Array *DelinearizeInfo::findArray(const Value *Ptr,
    const DataLayout &DL) const {
  auto Info = GetUnderlyingArray(const_cast<Value *>(Ptr), DL);
  return findArray(Info.first, Info.second);
}

void DelinearizationPass::cleanSubscripts(Array &ArrayInfo) {
  assert(ArrayInfo.isDelinearized() && "Array must be delinearized!");
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: simplify subscripts for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto LastConstDim = ArrayInfo.getNumberOfDims();
  for (LastConstDim; LastConstDim > 0; --LastConstDim)
    if (!isa<SCEVConstant>(ArrayInfo.getDimSize(LastConstDim - 1)))
      break;
  if (LastConstDim == 0)
    return;
  for (auto &Range : ArrayInfo) {
    assert(ArrayInfo.getNumberOfDims() - LastConstDim <= Range.Subscripts.size()
      && "Unknown subscripts in right dimensions with constant sizes!");
    // In some cases zero subscript is dropping out by optimization passes.
    // So, we try to add extra zero subscripts.
    auto ExtraZeroCount = Range.is(Array::Range::NeedExtraZero) ?
      ArrayInfo.getNumberOfDims() - Range.Subscripts.size() : 0;
    std::size_t DimIdx = 0, SubscriptIdx = 0;
    std::size_t DimIdxE = std::min(LastConstDim - 1, Range.Subscripts.size());
    SmallVector<const SCEV *, 4> NewSubscripts;
    for (;  DimIdx < DimIdxE; ++DimIdx) {
      auto *Subscript = Range.Subscripts[SubscriptIdx++];
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: subscript " << DimIdx << ": "
                        << *Subscript << "\n");
      for (std::size_t I = DimIdx + 1; I < LastConstDim; ++I) {
        auto Div =
          divide(*mSE, Subscript, ArrayInfo.getDimSize(I), mIsSafeTypeCast);
        if (Div.Remainder->isZero()) {
          Subscript = Div.Quotient;
        } else if (ExtraZeroCount > 0) {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add extra zero subscript\n");
          Subscript = mSE->getZero(mIndexTy);
          --ExtraZeroCount;
          // We insert zero subscript before current subscript, so we should
          // reprocess it.
          --SubscriptIdx;
          break;
        } else {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize\n");
          Subscript = nullptr;
          break;
        }
      }
      if (!Subscript)
        break;
      NewSubscripts.push_back(Subscript);
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: set to " << *Subscript << "\n");
    }
    if (DimIdx < DimIdxE)
      continue;
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add " << ExtraZeroCount
                      << " extra zero subscripts to " << SubscriptIdx << "\n");
    for (std::size_t I = 0; I < ExtraZeroCount; ++I)
      NewSubscripts.push_back(mSE->getZero(mIndexTy));
    // Add subscripts for constant dimensions.
    for (auto EI = Range.Subscripts.size(); SubscriptIdx < EI; ++SubscriptIdx)
      NewSubscripts.push_back(Range.Subscripts[SubscriptIdx]);
    std::swap(Range.Subscripts, NewSubscripts);
    assert(Range.Subscripts.size() <= ArrayInfo.getNumberOfDims() &&
      "Unable to delinearize element access!");
    Range.setProperty(Array::Range::IsDelinearized);
  }
}

void DelinearizationPass::fillArrayDimensionsSizes(Array &ArrayInfo) {
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute sizes of dimensions for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto NumberOfDims = ArrayInfo.getNumberOfDims();
  auto LastUnknownDim = NumberOfDims;
  if (NumberOfDims == 0) {
    for (auto &Range : ArrayInfo)
      if (Range.isElement())
        NumberOfDims = std::max(Range.Subscripts.size(), NumberOfDims);
    if (NumberOfDims == 0) {
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: no valid element found\n";
        dbgs() << "[DELINEARIZE]: unable to determine number of"
        " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
      return;
    }
    for (auto &Range : ArrayInfo) {
      if (Range.isElement() && NumberOfDims < Range.Subscripts.size())
        Range.setProperty(Array::Range::NeedExtraZero);
    }
    ArrayInfo.setNumberOfDims(NumberOfDims);
    ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = NumberOfDims - 1;
    LLVM_DEBUG(
      dbgs() << "[DELINEARIZE]: extract number of dimensions from subscripts: "
             << NumberOfDims << "\n");
  } else {
    auto LastConstDim = ArrayInfo.getNumberOfDims();
    for (auto I = ArrayInfo.getNumberOfDims(); I > 0; --I) {
      if (!dyn_cast_or_null<SCEVConstant>(ArrayInfo.getDimSize(I - 1)))
        break;
      LastConstDim = I - 1;
    }
    if (LastConstDim == 0) {
      ArrayInfo.setDelinearized();
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: all dimensions have constant sizes\n");
      return;
    }
    if (!ArrayInfo.isKnownDimSize(0))
      ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = LastConstDim - 1;
  }
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute non-constant dimension sizes\n");
  auto setUnknownDims = [this, &ArrayInfo](
      std::size_t LastUnknownIdx) {
    auto UnknownSize = mSE->getCouldNotCompute();
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: could not compute dimension size\n");
    ArrayInfo.setDimSize(LastUnknownIdx, UnknownSize);
    for (auto J : seq(decltype(LastUnknownIdx)(1), LastUnknownIdx)) {
      if (!ArrayInfo.isKnownDimSize(J))
        ArrayInfo.setDimSize(J, UnknownSize);
    }
  };
  auto *PrevDimSizesProduct = mSE->getConstant(mIndexTy, 1);
  auto DimIdx = LastUnknownDim;
  for (; DimIdx > 0; --DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process dimension " << DimIdx << "\n");
    const SCEV *DimSize;
    if (ArrayInfo.isKnownDimSize(DimIdx) > 0) {
      DimSize = ArrayInfo.getDimSize(DimIdx);
      if (DimSize->isZero()) {
        setUnknownDims(DimIdx);
        return;
      }
    } else {
      SmallVector<const SCEV *, 3> Expressions;
      for (auto &Range: ArrayInfo) {
        if (!Range.isElement() || Range.is(Array::Range::NeedExtraZero))
          continue;
        assert(Range.Subscripts.size() == NumberOfDims &&
          "Number of dimensions is inconsistent with number of subscripts!");
        for (auto J = DimIdx; J > 0; --J) {
          Expressions.push_back(Range.Subscripts[J - 1]);
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: use for GCD computation: ";
            TSAR_LLVM_DUMP(Expressions.back()->dump()));
        }
      }
      if (Expressions.empty()) {
        LLVM_DEBUG(
          dbgs() << "[DELINEARIZE]: no valid element found to compute GCD\n");
        setUnknownDims(DimIdx);
        return;
      }
      DimSize = findGCD(Expressions, *mSE, mIsSafeTypeCast);
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: GCD: ";
        TSAR_LLVM_DUMP(DimSize->dump()));
      if (isa<SCEVCouldNotCompute>(DimSize)) {
        setUnknownDims(DimIdx);
        return;
      }
      auto Div = divide(*mSE, DimSize, PrevDimSizesProduct, mIsSafeTypeCast);
      DimSize = Div.Quotient;
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        TSAR_LLVM_DUMP(PrevDimSizesProduct->dump());
        dbgs() << "[DELINEARIZE]: quotient ";
        TSAR_LLVM_DUMP(Div.Quotient->dump());
        dbgs() << "[DELINEARIZE]: remainder ";
        TSAR_LLVM_DUMP(Div.Remainder->dump()));
    }
    ArrayInfo.setDimSize(DimIdx, DimSize);
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: dimension size is ";
      TSAR_LLVM_DUMP(DimSize->dump()));
    DimSize = mSE->getTruncateOrZeroExtend(DimSize, mIndexTy);
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct, DimSize);
  }
  if (DimIdx == 0)
    ArrayInfo.setDelinearized();
}

void DelinearizationPass::findArrayDimesionsFromDbgInfo(Array &ArrayInfo) {
  SmallVector<DIMemoryLocation, 1> DILocs;
  // If base is an address of a memory which contains address of this array
  // we search dbg.declare and dbg.address only. However, if base is an
  // address of an array we search all metadata, because array may be a pointer
  // (like in C) or a reference (like in Fortran). So, in C dbg.value is used
  // to mark base and in Fortran dbg.address or dbg.declare may be used to
  // mark underlying memory.
  auto DIM = findMetadata(ArrayInfo.getBase(), DILocs, mDT,
    ArrayInfo.isAddressOfVariable() ? MDSearch::AddressOfVariable :
      MDSearch::Any);
  if (!DIM)
    return;
  assert(DIM->isValid() && "Debug memory location must be valid!");
  ArrayInfo.setMetadata();
  if (!DIM->Var->getType())
    return;
  auto VarDbgTy = stripDIType(DIM->Var->getType()).resolve();
  DINodeArray ArrayDims = nullptr;
  bool IsFirstDimPointer = false;
  if (VarDbgTy->getTag() == dwarf::DW_TAG_array_type) {
    ArrayDims = cast<DICompositeType>(VarDbgTy)->getElements();
  } else if (VarDbgTy->getTag() == dwarf::DW_TAG_pointer_type) {
    IsFirstDimPointer = true;
    auto BaseTy = cast<DIDerivedType>(VarDbgTy)->getBaseType();
    if (BaseTy && BaseTy.resolve()->getTag() == dwarf::DW_TAG_array_type)
      ArrayDims = cast<DICompositeType>(BaseTy)->getElements();
  }
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZE]: number of array dimensions for "
           << ArrayInfo.getBase()->getName() << " is ";
    dbgs() << (ArrayDims.size() + (IsFirstDimPointer ? 1 : 0)) << "\n");
  if (IsFirstDimPointer) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: first dimension is pointer\n");
    ArrayInfo.setNumberOfDims(ArrayDims.size() + 1);
  } else {
    ArrayInfo.setNumberOfDims(ArrayDims.size());
  }
  if (!ArrayDims)
    return;
  std::size_t PassPtrDim = IsFirstDimPointer ? 1 : 0;
  for (std::size_t DimIdx = 0; DimIdx < ArrayDims.size(); ++DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: size of " << DimIdx << " dimension is ");
    if (auto *DIDim = dyn_cast<DISubrange>(ArrayDims[DimIdx])) {
      auto DIDimCount = DIDim->getCount();
      if (DIDimCount.is<ConstantInt*>()) {
        auto Count = DIDimCount.get<ConstantInt *>()->getValue();
        if (Count.isNonNegative())
          ArrayInfo.setDimSize(DimIdx + PassPtrDim,
            mSE->getSCEV(DIDimCount.get<ConstantInt *>()));
        LLVM_DEBUG(dbgs() << Count << "\n");
      } else if (DIDimCount.is<DIVariable *>()) {
        auto DIVar = DIDimCount.get<DIVariable *>();
        if (auto V = MetadataAsValue::getIfExists(DIVar->getContext(), DIVar)) {
          SmallVector<DbgInfoIntrinsic *, 4> DbgInsts;
          // Do not use findDbgUsers(). It checks V->isUsedByMetadata() which
          // may return false for MetadataAsValue.
          for (User *U : V->users())
            if (DbgInfoIntrinsic *DII = dyn_cast<DbgInfoIntrinsic>(U))
              DbgInsts.push_back(DII);
          if (DbgInsts.size() == 1) {
            ArrayInfo.setDimSize(DimIdx + PassPtrDim,
              mSE->getSCEV(DbgInsts.front()->getVariableLocation()));
          }
        }
        LLVM_DEBUG(dbgs() << DIVar->getName() << "\n");
      } else {
        LLVM_DEBUG( dbgs() << "unknown\n");
      }
    }
  }
}

void DelinearizationPass::collectArrays(Function &F) {
  for (auto &I : instructions(F)) {
    auto processMemory = [this](Instruction &I, MemoryLocation Loc,
        unsigned,  AccessInfo, AccessInfo) {
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          return;
      }
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process instruction ";
        TSAR_LLVM_DUMP(I.dump()));
      auto &DL = I.getModule()->getDataLayout();
      auto *BasePtr = const_cast<Value *>(Loc.Ptr);
      bool IsAddressOfVariable;
      std::tie(BasePtr, IsAddressOfVariable) = GetUnderlyingArray(BasePtr, DL);
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: strip to "
               << (IsAddressOfVariable ? "address of " : "")
               << "base " << *BasePtr << "\n");
      auto ArrayPtr = mDelinearizeInfo.findArray(BasePtr, IsAddressOfVariable);
      if (!ArrayPtr) {
        ArrayPtr = *mDelinearizeInfo.getArrays().insert(
          new Array(BasePtr, IsAddressOfVariable)).first;
        findArrayDimesionsFromDbgInfo(*ArrayPtr);
      }
      auto NumberOfDims = ArrayPtr->getNumberOfDims();
      SmallVector<GEPOperator *, 4> GEPs;
      auto *GEP = dyn_cast<GEPOperator>(const_cast<Value *>(Loc.Ptr));
      while (GEP && (NumberOfDims == 0 || GEPs.size() < NumberOfDims)) {
        GEPs.push_back(GEP);
        GEP = dyn_cast<GEPOperator>(GEP->getPointerOperand());
      }
      SmallVector<Value *, 4> SubscriptValues;
      extractSubscriptsFromGEPs(GEPs.rbegin(), GEPs.rend(), SubscriptValues);
      auto &Range = ArrayPtr->addRange(const_cast<Value *>(Loc.Ptr));
      if (GEP)
        Range.setProperty(Array::Range::IgnoreGEP);
      // In some cases zero subscript is dropping out by optimization passes.
      // So, we try to add extra zero subscripts later. We add subscripts for
      // instructions which access a single element, for example, in case of
      // a call it is possible to pass a whole array as a parameter
      // (without GEPs).
      if (isa<LoadInst>(I) || isa<StoreInst>(I) ||
          isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        Range.setProperty(Array::Range::IsElement);
        if (SubscriptValues.size() < NumberOfDims) {
          ArrayPtr->setRangeRef();
          Range.setProperty(Array::Range::NeedExtraZero);
        }
      }
      if (!SubscriptValues.empty()) {
        ArrayPtr->setRangeRef();
        for (auto *V : SubscriptValues)
          Range.Subscripts.push_back(mSE->getSCEV(V));
      }
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: number of dimensions "
               << NumberOfDims << "\n";
        dbgs() << "[DELINEARIZE]: number of subscripts "
               << Range.Subscripts.size() << "\n";
        if (Range.is(Array::Range::NeedExtraZero))
          dbgs() << "[DELINEARIZE]: need extra zero subscripts\n";
        if (Range.is(Array::Range::IgnoreGEP))
          dbgs() << "[DELINEARIZE]: ignore some beginning GEPs\n";
        dbgs() << "[DELINEARIZE]: subscripts: \n";
        for (auto *Subscript : Range.Subscripts) {
          dbgs() << "  "; TSAR_LLVM_DUMP( Subscript->dump());
        }
      );
    };
    for_each_memory(I, *mTLI, processMemory,
      [](Instruction &, AccessInfo, AccessInfo) {});
  }
  // Now, we remove all object which is not arrays.
  for (auto Itr = mDelinearizeInfo.getArrays().begin(),
       ItrE = mDelinearizeInfo.getArrays().end(); Itr != ItrE;) {
    auto CurrItr = Itr++;
    if ((*CurrItr)->getNumberOfDims() == 0 && !(*CurrItr)->hasRangeRef()) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: not an array "
                        << (*CurrItr)->getBase()->getName() << "\n");
      mDelinearizeInfo.getArrays().erase(CurrItr);
    }
  }
}

bool DelinearizationPass::runOnFunction(Function &F) {
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZE]: process function " << F.getName() << "\n");
  releaseMemory();
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  mIsSafeTypeCast =
    getAnalysis<GlobalOptionsImmutableWrapper>().getOptions().IsSafeTypeCast;
  auto &DL = F.getParent()->getDataLayout();
  mIndexTy = DL.getIndexType(Type::getInt8PtrTy(F.getContext()));
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: index type is ";
    TSAR_LLVM_DUMP(mIndexTy->dump()));
  collectArrays(F);
  for (auto *ArrayInfo : mDelinearizeInfo.getArrays()) {
    fillArrayDimensionsSizes(*ArrayInfo);
    if (ArrayInfo->isDelinearized()) {
      cleanSubscripts(*ArrayInfo);
    } else {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize "
                        << ArrayInfo->getBase()->getName() << "\n");
    }
  }
  mDelinearizeInfo.updateRangeCache();
  LLVM_DEBUG(delinearizationLog(mDelinearizeInfo, *mSE, mIsSafeTypeCast, dbgs()));
  return false;
}

void DelinearizationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

void DelinearizationPass::print(raw_ostream &OS, const Module *M) const {
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  RawDelinearizeInfo Info = tsar::toJSON(mDelinearizeInfo, SE, mIsSafeTypeCast);
  OS << json::Parser<RawDelinearizeInfo>::unparse(Info) << '\n';
}

FunctionPass * createDelinearizationPass() { return new DelinearizationPass; }

RawDelinearizeInfo tsar::toJSON(const DelinearizeInfo &Info,
    ScalarEvolution &SE, bool IsSafeTypeCast) {
  RawDelinearizeInfo RawInfo;
  for (auto &ArrayInfo : Info.getArrays()) {
    std::string NameStr;
    raw_string_ostream NameOS(NameStr);
    NameOS.flush();
    ArrayInfo->getBase()->print(NameOS);
    std::vector<std::string> DimSizes(ArrayInfo->getNumberOfDims());
    for (std::size_t I = 0, EI = DimSizes.size(); I < EI; ++I) {
      raw_string_ostream DimSizeOS(DimSizes[I]);
      ArrayInfo->getDimSize(I)->print(DimSizeOS);
      DimSizeOS.flush();
    }
    std::vector<std::vector<std::vector<std::string>>> Accesses;
    for (auto &Range : *ArrayInfo) {
      std::vector<std::vector<std::string>> Subscripts;
      for (auto *S : Range.Subscripts) {
        Subscripts.emplace_back(2);
        auto &CoefStr = Subscripts.back().front();
        auto &ConstTermStr = Subscripts.back().back();
        auto Info = computeSCEVAddRec(S, SE);
        const SCEV *Coef, *ConstTerm;
        if ((!IsSafeTypeCast || Info.second) &&
            isa<SCEVAddRecExpr>(Info.first)) {
          auto AddRec = cast<SCEVAddRecExpr>(Info.first);
          Coef = AddRec->getStepRecurrence(SE);
          ConstTerm = AddRec->getStart();
        } else {
          Coef = SE.getZero(S->getType());
          ConstTerm = S;
        }
        raw_string_ostream CoefOS(CoefStr);
        Coef->print(CoefOS);
        CoefOS.flush();
        raw_string_ostream ConstTermOS(ConstTermStr);
        ConstTerm->print(ConstTermOS);
        ConstTermOS.flush();
      }
      Accesses.push_back(std::move(Subscripts));
    }
    RawInfo[RawDelinearizeInfo::Sizes].emplace(NameStr, std::move(DimSizes));
    RawInfo[RawDelinearizeInfo::Accesses].emplace(
      std::move(NameStr), std::move(Accesses));
  }
  return RawInfo;
}
