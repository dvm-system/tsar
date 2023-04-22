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

#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/DelinearizeJSON.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/SCEVUtils.h"
#include "tsar/Support/Utils.h"
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
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
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
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
bool extractSubscriptsFromGEPs(
    const GEPItrT &GEPBeginItr, const GEPItrT &GEPEndItr,
    std::size_t NumberOfDims,
    SmallVectorImpl<std::tuple<Value *, Type *>> &Idxs) {
  assert(Idxs.empty() && "List of indexes must be empty!");
  SmallVector<std::tuple<Value *, Type *>, 8> GEPs;
  for (auto *GEP : make_range(GEPBeginItr, GEPEndItr)) {
    unsigned NumOperands = GEP->getNumOperands();
    if (NumOperands == 2) {
      GEPs.emplace_back(GEP->getOperand(1),
                        gep_type_begin(GEP).getIndexedType());
    } else {
      SmallVector<Type *, 4> Types;
      for (auto I = gep_type_begin(GEP), EI = gep_type_end(GEP); I != EI; ++I) {
        if (I.isStruct()) {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: drop ending structure\n");
          GEPs.clear();
          break;
        }
        Types.push_back(I.getIndexedType());
      }
      if (Types.empty())
        continue;
      for (unsigned I =  Types.size(); I > 1; --I)
        GEPs.emplace_back(GEP->getOperand(I), Types[I - 1]);
      if (auto *SecondOp = dyn_cast<Constant>(GEP->getOperand(1))) {
        if (!SecondOp->isZeroValue())
          GEPs.emplace_back(GEP->getOperand(1), Types[0]);
      } else {
        GEPs.emplace_back(GEP->getOperand(1), Types[0]);
      }
    }
  }
  if (NumberOfDims > 0 && GEPs.size() > NumberOfDims) {
    std::copy(GEPs.rbegin(), GEPs.rbegin() + NumberOfDims,
      std::back_inserter(Idxs));
    return false;
  }
  std::copy(GEPs.rbegin(), GEPs.rend(), std::back_inserter(Idxs));
  return true;
}

std::tuple<Value *, Value *, bool> GetUnderlyingArray(Value *Ptr,
                                                     const DataLayout &DL) {
  auto BasePtrInfo = GetUnderlyingObjectWithMetadata(Ptr, DL);
  if (!BasePtrInfo.second && isa<LoadInst>(BasePtrInfo.first)) {
    Ptr = cast<LoadInst>(BasePtrInfo.first)->getPointerOperand();
  } else if (isa<ConstantExpr>(BasePtrInfo.first) ||
             !isa<AllocaInst>(BasePtrInfo.first) &&
                 !isa<Constant>(BasePtrInfo.first)) {
    Ptr = BasePtrInfo.first;
    SmallVector<DbgVariableIntrinsic *, 4> DbgUsers;
    findAllDbgUsers(DbgUsers, BasePtrInfo.first);
    auto CurrFuncDbgItr = find_if(DbgUsers, [](auto *U) {
      auto DbgLoc = U->getDebugLoc();
      return DbgLoc && !DbgLoc.getInlinedAt();
    });
    // Attached metadata may refer to an inlined function after callee has been
    // inlined. Try to find metadata related to the current function.
    if (CurrFuncDbgItr == DbgUsers.end()) {
      auto InlineBasePtr = getUnderlyingObject(BasePtrInfo.first, 1);
      if (InlineBasePtr != BasePtrInfo.first)
        return GetUnderlyingArray(InlineBasePtr, DL);
    }
  } else {
    Ptr = BasePtrInfo.first;
  }
  bool IsAddressOfVariable = (Ptr != BasePtrInfo.first);
  return std::make_tuple(Ptr, BasePtrInfo.first, IsAddressOfVariable);
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
  return findArray(std::get<0>(Info), std::get<2>(Info));
}

void DelinearizationPass::cleanSubscripts(Array &ArrayInfo) {
  assert(ArrayInfo.isDelinearized() && "Array must be delinearized!");
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: simplify subscripts for "
                    << (ArrayInfo.isAddressOfVariable() ? "address of " : "")
                    << "base " << ArrayInfo.getBase()->getName() << "\n");
  auto addExtraConstZero =
      [this, &ArrayInfo](const Array::Range &Range, unsigned ExtraZeroCount,
                         const llvm::Type *DereferenceTy, unsigned Idx,
                         SmallVectorImpl<const SCEV *> &NewSubscripts) -> bool {
    auto *ATy{dyn_cast<ArrayType>(DereferenceTy)};
    if (!ATy)
      return false;
    unsigned IdxE = Range.Types.size();
    for (; ATy && Idx < IdxE;
         ATy = dyn_cast<ArrayType>(ATy->getElementType())) {
      if (ATy->getElementType() == Range.Types[Idx]) {
        NewSubscripts.push_back(Range.Subscripts[Idx]);
        ++Idx;
      } else if (ExtraZeroCount > 0) {
        --ExtraZeroCount;
        NewSubscripts.push_back(mSE->getZero(mIndexTy));
      } else {
        return false;
      }
    }
    if (!ATy)
      return Idx == IdxE;
    for (unsigned I = 0; I < ExtraZeroCount; ++I)
      NewSubscripts.push_back(mSE->getZero(mIndexTy));
    return true;
  };
  auto LastConstDim = ArrayInfo.getNumberOfDims();
  for (LastConstDim; LastConstDim > 0; --LastConstDim)
    if (!isa<SCEVConstant>(ArrayInfo.getDimSize(LastConstDim - 1)))
      break;
  if (LastConstDim == 0) {
    for (auto &Range : ArrayInfo) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process access ";
                 Range.Ptr->print(dbgs()); dbgs() << "\n");
      if (auto ExtraZeroCount{
          Range.is(Array::Range::NeedExtraZero)
              ? ArrayInfo.getNumberOfDims() - Range.Subscripts.size()
                                  : 0};
          ExtraZeroCount > 0) {
        SmallVector<const SCEV *, 4> NewSubscripts;
        auto *BaseType{ArrayInfo.getBase()->getType()};
        if (auto *GV{dyn_cast<GlobalValue>(ArrayInfo.getBase())})
          BaseType = GV->getValueType();
        else if (auto *AI{dyn_cast<AllocaInst>(ArrayInfo.getBase())})
          BaseType = AI->getAllocatedType();
        if (!addExtraConstZero(Range, ExtraZeroCount, BaseType, 0,
                               NewSubscripts)) {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize access to "
                               "constant dimensions\n");
          continue;
        }
        std::swap(Range.Subscripts, NewSubscripts);
        LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add " << ExtraZeroCount
                          << " extra zero subscripts\n");
      }
      assert(Range.Subscripts.size() <= ArrayInfo.getNumberOfDims() &&
             "Unable to delinearize element access!");
      Range.setProperty(Array::Range::IsDelinearized);
    }
    return;
  }
  for (auto &Range : ArrayInfo) {
    if (Range.is(Array::Range::NoGEP))
      continue;
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process access ";
               Range.Ptr->print(dbgs()); dbgs() << "\n");
    assert((!Range.isElement() || Range.is(Array::Range::NeedExtraZero) ||
            ArrayInfo.getNumberOfDims() - LastConstDim <=
                Range.Subscripts.size()) &&
           "Unknown subscripts in right dimensions with constant sizes!");
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
    if (ExtraZeroCount > 0) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add " << ExtraZeroCount
                        << " extra zero subscripts after " << SubscriptIdx
                        << "\n");
      if (Range.Subscripts.size() > SubscriptIdx) {
        NewSubscripts.push_back(Range.Subscripts[SubscriptIdx]);
        if (!addExtraConstZero(Range, ExtraZeroCount, Range.Types[SubscriptIdx],
                               SubscriptIdx + 1, NewSubscripts)) {
          if (SubscriptIdx == 0 ||
              !(NewSubscripts.pop_back(),
                addExtraConstZero(Range, ExtraZeroCount,
                                  Range.Types[SubscriptIdx - 1], SubscriptIdx,
                                  NewSubscripts))) {
            LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize\n");
            continue;
          }
        }
      } else {
        for (std::size_t I = 0; I < ExtraZeroCount; ++I)
          NewSubscripts.push_back(mSE->getZero(mIndexTy));
      }
    } else {
      // Add subscripts for constant dimensions.
      for (auto EI = Range.Subscripts.size(); SubscriptIdx < EI; ++SubscriptIdx)
        NewSubscripts.push_back(Range.Subscripts[SubscriptIdx]);
    }
    std::swap(Range.Subscripts, NewSubscripts);
    assert(Range.Subscripts.size() <= ArrayInfo.getNumberOfDims() &&
      "Unable to delinearize element access!");
    Range.setProperty(Array::Range::IsDelinearized);
  }
}

void DelinearizationPass::fillArrayDimensionsSizes(Array &ArrayInfo) {
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute sizes of dimensions for "
                    << (ArrayInfo.isAddressOfVariable() ? "address of " : "")
                    << "base " << ArrayInfo.getBase()->getName() << "\n");
  auto NumberOfDims = ArrayInfo.getNumberOfDims();
  auto LastUnknownDim = NumberOfDims;
  if (NumberOfDims == 0) {
    for (auto &Range : ArrayInfo)
        NumberOfDims = std::max(Range.Subscripts.size(), NumberOfDims);
    if (NumberOfDims == 0) {
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: no valid element found\n";
        dbgs() << "[DELINEARIZE]: unable to determine number of"
        " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
      return;
    }
    for (auto &Range : ArrayInfo) {
      if (Range.isElement() && NumberOfDims > Range.Subscripts.size())
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
    if (ArrayInfo.isKnownDimSize(DimIdx)) {
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
            Expressions.back()->print(dbgs()); dbgs() << "\n");
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
        DimSize->print(dbgs()); dbgs() << "\n");
      if (isa<SCEVCouldNotCompute>(DimSize)) {
        setUnknownDims(DimIdx);
        return;
      }
      // Exclude not loop-invariant factors from the dimension size.
      SmallPtrSet<const Loop *, 4> LoopsToCheck;
      for (auto &Range: ArrayInfo)
        if (auto *Inst = dyn_cast<Instruction>(Range.Ptr)) {
          auto *BB = Inst->getParent();
          auto *L = mLI->getLoopFor(BB);
          while (L) {
            LoopsToCheck.insert(L);
            L = L->getParentLoop();
          }
        }
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: found " << LoopsToCheck.size()
                        << " loops related to array uses\n");
      if (!LoopsToCheck.empty()) {
        SmallVector<const SCEV *, 4> InvariantFactors;
        if (auto *Factors = dyn_cast<SCEVMulExpr>(DimSize)) {
          for (auto *Expr : Factors->operands())
            if (all_of(LoopsToCheck, [this, Expr](const Loop *L) {
                  return mSE->isLoopInvariant(Expr, L);
                }))
              InvariantFactors.push_back(Expr);
        } else if (all_of(LoopsToCheck, [this, DimSize](const Loop *L) {
                     return mSE->isLoopInvariant(DimSize, L);
                   })) {
          InvariantFactors.push_back(DimSize);
        }
        if (InvariantFactors.empty()) {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: size is not invariant\n");
          DimSize = mSE->getCouldNotCompute();
          setUnknownDims(DimIdx);
          return;
        }
        DimSize = mSE->getMulExpr(InvariantFactors);
      }
      auto Div = divide(*mSE, DimSize, PrevDimSizesProduct, mIsSafeTypeCast);
      DimSize = Div.Quotient;
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        PrevDimSizesProduct->print(dbgs()); dbgs() << "\n";
        dbgs() << "[DELINEARIZE]: quotient ";
        Div.Quotient->print(dbgs()); dbgs() << "\n";
        dbgs() << "[DELINEARIZE]: remainder ";
        Div.Remainder->print(dbgs()); dbgs() << "\n");
    }
    ArrayInfo.setDimSize(DimIdx, DimSize);
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: dimension size is ";
      DimSize->print(dbgs()); dbgs() << "\n");
    DimSize = mSE->getTruncateOrZeroExtend(DimSize, mIndexTy);
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct, DimSize);
  }
  if (DimIdx == 0)
    ArrayInfo.setDelinearized();
}

void DelinearizationPass::findArrayDimensionsFromDbgInfo(Array &ArrayInfo) {
  if (auto *AI = dyn_cast<AllocaInst>(ArrayInfo.getBase()))
    if (!ArrayInfo.isAddressOfVariable() &&
        !AI->isArrayAllocation() && !AI->getAllocatedType()->isArrayTy())
      return;
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
  auto VarDbgTy = stripDIType(DIM->Var->getType());
  DINodeArray ArrayDims = nullptr;
  bool IsFirstDimPointer = false;
  if (VarDbgTy->getTag() == dwarf::DW_TAG_array_type) {
    ArrayDims = cast<DICompositeType>(VarDbgTy)->getElements();
  } else if (VarDbgTy->getTag() == dwarf::DW_TAG_pointer_type) {
    IsFirstDimPointer = true;
    auto BaseTy = cast<DIDerivedType>(VarDbgTy)->getBaseType();
    if (BaseTy && BaseTy->getTag() == dwarf::DW_TAG_array_type)
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
          SmallVector<DbgVariableIntrinsic *, 4> DbgInsts;
          // Do not use findDbgUsers(). It checks V->isUsedByMetadata() which
          // may return false for MetadataAsValue.
          for (User *U : V->users())
            if (auto *DII = dyn_cast<DbgVariableIntrinsic>(U))
              DbgInsts.push_back(DII);
          if (DbgInsts.size() == 1) {
            auto *DimSize{
                mSE->getSCEV(DbgInsts.front()->getVariableLocationOp(0))};
            if (!DimSize->getType()->isPointerTy())
              ArrayInfo.setDimSize(DimIdx + PassPtrDim, DimSize);
            LLVM_DEBUG(dbgs()
                       << (isa<SCEVUnknown>(DimSize) ? " unknown" : "known "));
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
  DenseSet<const Value *> Visited;
  for (auto &I : instructions(F)) {
    auto processMemory = [this, &Visited](Instruction &I, MemoryLocation Loc,
        unsigned,  AccessInfo, AccessInfo) {
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          return;
      }
      if (!Visited.insert(Loc.Ptr).second)
        return;
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process instruction ";
        I.print(dbgs()); dbgs() << "\n");
      auto &DL = I.getModule()->getDataLayout();
      auto *BasePtr = const_cast<Value *>(Loc.Ptr);
      auto *DataPtr = BasePtr;
      bool IsAddressOfVariable;
      std::tie(BasePtr, DataPtr, IsAddressOfVariable) =
        GetUnderlyingArray(BasePtr, DL);
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: strip to "
               << (IsAddressOfVariable ? "address of " : "")
               << "base " << *BasePtr << "\n");
      auto ArrayPtr = mDelinearizeInfo.findArray(BasePtr, IsAddressOfVariable);
      if (!ArrayPtr) {
        ArrayPtr = *mDelinearizeInfo.getArrays().insert(
          new Array(BasePtr, IsAddressOfVariable)).first;
        findArrayDimensionsFromDbgInfo(*ArrayPtr);
      }
      auto NumberOfDims = ArrayPtr->getNumberOfDims();
      SmallVector<GEPOperator *, 4> GEPs;
      auto RangePtr = Loc.Ptr;
      auto *GEP = dyn_cast<GEPOperator>(const_cast<Value *>(RangePtr));
      while (!GEP) {
        if (Operator::getOpcode(RangePtr) == Instruction::BitCast ||
            Operator::getOpcode(RangePtr) == Instruction::AddrSpaceCast)
          RangePtr = cast<Operator>(RangePtr)->getOperand(0);
        else
          break;
        GEP = dyn_cast<GEPOperator>(const_cast<Value *>(RangePtr));
      }
      while (GEP) {
        GEPs.push_back(GEP);
        GEP = dyn_cast<GEPOperator>(GEP->getPointerOperand());
      }
      auto &Range = ArrayPtr->addRange(const_cast<Value *>(Loc.Ptr));
      if (!isa<GEPOperator>(RangePtr) &&
          !(RangePtr == BasePtr && !IsAddressOfVariable) &&
          !(RangePtr == DataPtr && IsAddressOfVariable))
        Range.setProperty(Array::Range::NoGEP);
      SmallVector<std::tuple<Value *, Type *>, 4> SubscriptValues;
      bool UseAllSubscripts = extractSubscriptsFromGEPs(
        GEPs.begin(), GEPs.end(), NumberOfDims, SubscriptValues);
      if (!UseAllSubscripts)
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
        for (auto &&[V, T] : SubscriptValues) {
          Range.Subscripts.push_back(mSE->getSCEV(V));
          Range.Types.push_back(T);
        }
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
        if (Range.is(Array::Range::NoGEP))
          dbgs() << "[DELINEARIZE]: no GEPs found\n";
        dbgs() << "[DELINEARIZE]: subscripts: \n";
        for (auto *Subscript : Range.Subscripts) {
          dbgs() << "  "; Subscript->print(dbgs()); dbgs() <<"\n";
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
        << ((*CurrItr)->isAddressOfVariable() ? "address of " : "")
        << "base " << (*CurrItr)->getBase()->getName() << "\n");
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
  mLI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  mIsSafeTypeCast =
    getAnalysis<GlobalOptionsImmutableWrapper>().getOptions().IsSafeTypeCast;
  auto &DL = F.getParent()->getDataLayout();
  mIndexTy = DL.getIndexType(Type::getInt8PtrTy(F.getContext()));
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: index type is ";
    mIndexTy->print(dbgs()); dbgs() << "\n");
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
  AU.addRequired<LoopInfoWrapperPass>();
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
    RawInfo[RawDelinearizeInfo::IsDelinearized] = ArrayInfo->isDelinearized();
  }
  return RawInfo;
}
