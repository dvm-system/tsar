//===--- SourceUnparser.cpp --- Source Info Unparser ------------*- C++ -*-===//
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
// This file implements unparser to print metadata objects as a constructs of
// an appropriate high-level language.
//
//===----------------------------------------------------------------------===//

#include "tsar/Unparse/SourceUnparser.h"
#include "tsar/Support/MetadataUtils.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/ADT/SmallBitVector.h>
#include <llvm/Support/raw_ostream.h>

using namespace tsar;
using namespace llvm;

bool SourceUnparserImp::unparseAsScalarTy(uint64_t Offset, bool IsPositive) {
  if (Offset == 0)
    return true;
  if (!mIsAddress) {
    updatePriority(TOKEN_ADDRESS, TOKEN_ADDRESS);
    mReversePrefix.push_back(TOKEN_ADDRESS);
    mIsAddress = true;
  }
  updatePriority(TOKEN_CAST_TO_ADDRESS, IsPositive ? TOKEN_ADD : TOKEN_SUB);
  mReversePrefix.push_back(TOKEN_CAST_TO_ADDRESS);
  mCurrType = nullptr;
  mSuffix.push_back(IsPositive ? TOKEN_ADD : TOKEN_SUB);
  mSuffix.push_back(TOKEN_UCONST);
  mUConsts.push_back(Offset);
  return true;
}

bool SourceUnparserImp::unparseAsStructureTy(uint64_t Offset, bool IsPositive) {
  assert(mCurrType && "Currently evaluated type must not be null!");
  auto DICTy = cast<DICompositeType>(mCurrType);
  auto TySize = getSize(mCurrType);
  if (DICTy->getElements().size() == 0 || !IsPositive || TySize <= Offset)
    return unparseAsScalarTy(Offset, IsPositive);
  DIDerivedType *CurrEl = nullptr;
  for (auto El: DICTy->getElements()) {
    if (El->getTag() != dwarf::DW_TAG_member)
      continue;
    auto DITy = cast<DIDerivedType>(stripDIType(cast<DIType>(El)));
    //if (!DITy || DITy->getTag() != dwarf::DW_TAG_member)
    //  continue;
    auto ElOffset = DITy->getOffsetInBits() / 8;
    // It is necessary to use both checks (== and >) to accurately evaluate
    // structures with bit fields. In this case the first field will be
    // used. To avoid usage of subsequent bit fields instead of it (==) is
    // necessary.
    if (ElOffset > Offset) {
      break;
    } else if (ElOffset == Offset) {
      CurrEl = DITy;
      break;
    }
    CurrEl = DITy;
  }
  // A composite type may have elements but does not have members in case of C++.
  if (!CurrEl)
    return unparseAsScalarTy(Offset, IsPositive);
  assert(Offset >= CurrEl->getOffsetInBits() / 8 &&
    "Too large offset of a structure filed!");
  if (mIsAddress) {
    updatePriority(TOKEN_DEREF, TOKEN_DEREF);
    mReversePrefix.push_back(TOKEN_DEREF);
    mIsAddress = false;
  }
  updatePriority(TOKEN_FIELD, TOKEN_FIELD);
  mSuffix.append({ TOKEN_FIELD,TOKEN_IDENTIFIER });
  mIdentifiers.push_back(CurrEl);
  mCurrType = stripDIType(CurrEl->getBaseType());
  Offset -= CurrEl->getOffsetInBits() / 8;
  return unparse(Offset, true);
}

bool SourceUnparserImp::unparseAsUnionTy(uint64_t Offset, bool IsPositive) {
  return unparseAsScalarTy(Offset, IsPositive);
}

bool SourceUnparserImp::unparseAsPointerTy(uint64_t Offset, bool IsPositive) {
  assert(mCurrType && "Currently evaluated type must not be null!");
  if (!mIsAddress)
    return unparseAsScalarTy(Offset, IsPositive);
  auto DIDTy = cast<DIDerivedType>(mCurrType);
  mCurrType = stripDIType(DIDTy->getBaseType());
  auto TySize = mCurrType ? getSize(mCurrType) : 0;
  if (TySize == 0)
    return unparseAsScalarTy(Offset, IsPositive);
  auto ElIdx = Offset / TySize;
  updatePriority(TOKEN_SUBSCRIPT_BEGIN, TOKEN_SUBSCRIPT_END);
  mSuffix.push_back(TOKEN_SUBSCRIPT_BEGIN);
  if (ElIdx == 0 && mLoc.Template) {
    mSuffix.push_back(TOKEN_UNKNOWN);
  } else {
    if (!IsPositive)
      mSuffix.push_back(TOKEN_SUB);
    mSuffix.push_back(TOKEN_UCONST);
    mUConsts.push_back(ElIdx);
    Offset -= ElIdx * TySize;
  }
  mSuffix.push_back(TOKEN_SUBSCRIPT_END);
  // Subscript expression (for example []) has been used, it is not address now.
  mIsAddress = false;
  return unparse(Offset, IsPositive);

}

void SourceUnparserImp::addUnknownSubscripts(unsigned Dims) {
  updatePriority(TOKEN_SUBSCRIPT_BEGIN, TOKEN_SUBSCRIPT_END);
  mSuffix.push_back(TOKEN_SUBSCRIPT_BEGIN);
  for (unsigned I = 0; I < Dims; ++I)
    mSuffix.append({ TOKEN_UNKNOWN, TOKEN_SEPARATOR });
  mSuffix.push_back(TOKEN_SUBSCRIPT_END);
}

bool SourceUnparserImp::unparseAsArrayTy(uint64_t Offset, bool IsPositive) {
  assert(mCurrType && "Currently evaluated type must not be null!");
  auto DICTy = cast<DICompositeType>(mCurrType);
  auto TySize = getSize(mCurrType);
  mCurrType = stripDIType(DICTy->getBaseType());
  auto ElSize = mCurrType ? getSize(mCurrType) : 0;
  mIsAddress = true;
  if (DICTy->getElements().size() == 0 || !IsPositive || TySize <= Offset ||
      ElSize == 0)
    return unparseAsScalarTy(Offset, IsPositive);
  SmallVector<std::pair<int64_t, int64_t>, 8> Dims;
  auto pushDimSize = [this, DICTy, Offset, IsPositive, &Dims](unsigned Dim) {
    auto DInfo = cast<DISubrange>(DICTy->getElements()[Dim]);
    auto LowerBound = DInfo->getLowerBound();
    auto Count = DInfo->getCount();
    if (!Count || Count.is<DIVariable *>() ||
        Count.get<ConstantInt *>()->isMinusOne() ||
        LowerBound && LowerBound.is<DIVariable *>() ||
        LowerBound && LowerBound.is<DIExpression *>())
      if (mLoc.Template) {
        addUnknownSubscripts(DICTy->getElements().size());
        return std::make_pair(false, true);
      } else {
        return std::make_pair(false, unparseAsScalarTy(Offset, IsPositive));
      }
    auto L = LowerBound ? LowerBound.get<ConstantInt *>()->getSExtValue() : 0;
    auto C = Count.get<ConstantInt *>()->getSExtValue();
    Dims.push_back(std::make_pair(L, C));
    return std::make_pair(true, true);
  };
  if (mIsForwardDim)
    for (unsigned I = 0, E = DICTy->getElements().size(); I != E; ++I) {
      auto Tmp = pushDimSize(I);
      if (!Tmp.first)
        return Tmp.second;
    }
  else
    for (unsigned I = DICTy->getElements().size() - 1, E = 0; I != E; --I) {
      auto Tmp = pushDimSize(I);
      if (!Tmp.first)
        return Tmp.second;
    }
  mIsAddress = false;
  auto ElIdx = Offset / ElSize;
  Offset -= ElIdx * ElSize;
  updatePriority(TOKEN_SUBSCRIPT_BEGIN, TOKEN_SUBSCRIPT_END);
  mSuffix.push_back(TOKEN_SUBSCRIPT_BEGIN);
  auto addDim = [this, &Dims, &ElIdx](unsigned Dim, unsigned DimE) {
    unsigned Coeff = 1;
    for (unsigned I = Dim + 1; I < DimE; Coeff *= Dims[I].second, ++I);
    auto DimOffset = ElIdx / Coeff;
    if (Dims[Dim].first < 0 && DimOffset < std::abs(Dims[Dim].first)) {
      mSuffix.push_back(TOKEN_SUB);
      DimOffset = std::abs(Dims[Dim].first) - DimOffset;
    } else {
      DimOffset += Dims[Dim].first;
    }
    if (DimOffset == 0 && mLoc.Template) {
      mSuffix.push_back(TOKEN_UNKNOWN);
    } else {
      mSuffix.push_back(TOKEN_UCONST);
      mUConsts.push_back(DimOffset);
    }
    ElIdx = ElIdx % Coeff;
  };
  addDim(0, Dims.size());
  for (unsigned Dim = 1, DimE = Dims.size(); Dim < DimE; ++Dim) {
    mSuffix.push_back(TOKEN_SEPARATOR);
    addDim(Dim, DimE);
  }
  mSuffix.push_back(TOKEN_SUBSCRIPT_END);
  return unparse(Offset, IsPositive);
}

bool SourceUnparserImp::unparseDeref() {
  if (mIsAddress) {
    updatePriority(TOKEN_DEREF, TOKEN_DEREF);
    mReversePrefix.push_back(TOKEN_DEREF);
    mIsAddress = false;
  } else {
    // Do not lower type hear because the pointer types will be evaluated
    // later in a separate method.
    if (mCurrType && mCurrType->getTag() != dwarf::DW_TAG_pointer_type)
      mCurrType = nullptr;
    mIsAddress = true;
  }
  return true;
}

bool SourceUnparserImp::unparse(uint64_t Offset, bool IsPositive) {
  if (mCurrType)
    switch (mCurrType->getTag()) {
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
      return unparseAsStructureTy(Offset, IsPositive);
    case dwarf::DW_TAG_array_type:
      return unparseAsArrayTy(Offset, IsPositive);
    case dwarf::DW_TAG_pointer_type:
      return unparseAsPointerTy(Offset, IsPositive);
    case dwarf::DW_TAG_union_type:
      return unparseAsUnionTy(Offset, IsPositive);
    }
  return unparseAsScalarTy(Offset, IsPositive);
}

bool SourceUnparserImp::unparse() {
  clear();
  mIdentifiers.push_back(mLoc.Var);
  mLastOpPriority = getPriority(TOKEN_IDENTIFIER);
  SmallVector<uint64_t, 4> Offsets;
  SmallBitVector SignMask;
  mLoc.getOffsets(Offsets, SignMask);
  assert(!Offsets.empty() && "Number of offsets must not be null!");
  mCurrType = stripDIType(mLoc.Var->getType());
  for (unsigned OffsetIdx = 0, E = Offsets.size() - 1;
       OffsetIdx < E; ++OffsetIdx) {
    if (!unparse(Offsets[OffsetIdx], SignMask.test(OffsetIdx)) ||
        !unparseDeref())
      return false;
  }
  if (!mIsMinimal || !mCurrType || Offsets.back() != 0 ||
        (mLoc.getSize().hasValue() &&
         mLoc.getSize().getValue() < getSize(mCurrType)))
    if (!unparse(Offsets.back(), SignMask.test(Offsets.size() - 1)))
      return false;
  if (mIsAddress) {
    updatePriority(TOKEN_DEREF, TOKEN_DEREF);
    mReversePrefix.push_back(TOKEN_DEREF);
  }
  return true;
}

StringRef SourceUnparserImp::getName(const MDNode &Identifier) const {
  if (auto *DIVar = dyn_cast<DIVariable>(&Identifier))
    return DIVar->getName();
  if (auto *DITy = dyn_cast<DIDerivedType>(&Identifier))
    return DITy->getName();
  return StringRef();
}
