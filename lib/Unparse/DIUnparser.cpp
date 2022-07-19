//===--- DIUnparser.cpp ------ Debug Info Unparser --------------*- C++ -*-===//
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
// This file implements unparser to print low-level LLVM IR in a more readable
// form, similar to high-level language.
//
//===----------------------------------------------------------------------===//

#include "tsar/Unparse/DIUnparser.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Debug.h>

using namespace tsar;
using namespace llvm;

bool DIUnparser::print(llvm::raw_ostream &OS) {
  SmallString<64> Str;
  return toString(Str) ? OS << Str, true : false;
}

LLVM_DUMP_METHOD bool DIUnparser::dump() {
  return print(dbgs());
}

void DIUnparser::endDITypeIfNeed(llvm::SmallVectorImpl<char> &Str) {
  if (Str.empty() || !mDIType || mIsDITypeEnd ||
      isa<AllocaInst>(mLastAddressChange))
    return;
  if (auto DICTy = dyn_cast<DICompositeType>(mDIType)) {
    // 1. There may be no 'getelementptr' instructions, this depends on
    // a starting point for unparse.
    // 2. It is necessary to check DICTy->getTag() because DICTy may be
    // a structure if unparsed memory location is a full structure instead
    // its member.
    if (isa<GEPOperator>(mLastAddressChange) &&
        DICTy->getTag() == dwarf::DW_TAG_array_type) {
      // We do not know how much dimensions should be specified because
      // a starting point for unparse is unknown. To underline this
      // [?]:1..N will be append to the result, where N is a number of
      // dimensions.
      Str.append({ '[','?',']' });
      auto Dims = DICTy->getElements().size();
      if (Dims > 1) {
        Str.append({':', '1', '.', '.'});
        auto DimsStr = std::to_string(DICTy->getElements().size());
        Str.append(DimsStr.begin(), DimsStr.end());
      }
      mDIType = stripDIType(DICTy->getBaseType());
    }
  } else if (auto DIDTy = dyn_cast<DIDerivedType>(mDIType)) {
    mDIType = stripDIType(DIDTy->getBaseType());
    // Type may be null in case of void *.
    auto DICTy = dyn_cast_or_null<DICompositeType>(mDIType);
    if (isa<GEPOperator>(mLastAddressChange)) {
      Str.append({ '[','?',']' });
      if (DICTy && DICTy->getTag() == dwarf::DW_TAG_array_type) {
        // This is something like a C99 array.
        Str.append({':', '1', '.', '.'});
        auto DimsStr = std::to_string(DICTy->getElements().size() + 1);
        Str.append(DimsStr.begin(), DimsStr.end());
        mDIType = stripDIType(DIDTy->getBaseType());
      }
    } else if (isa<LoadInst>(mLastAddressChange)) {
      Str.append({ '[','0',']' });
      if (DICTy && DICTy->getTag() == dwarf::DW_TAG_array_type) {
        // This is something like a C99 array.
        for (int I = 0; I < DICTy->getElements().size(); ++I)
          Str.append({ '[','0',']' });
        mDIType = stripDIType(DIDTy->getBaseType());
      }
    } else {
      // Process pointer which is stored in register, for example,
      // if the corresponding variable has been promoted.
      Str.append({ '[','0',']' });
    }
  } else {
    llvm_unreachable("Unsupported debug type!");
  }
    mIsDITypeEnd = !mDIType ||
      !isa<DICompositeType>(mDIType) && !isa<DIDerivedType>(mDIType);
}

bool DIUnparser::unparse(const Value *Expr, SmallVectorImpl<char> &Str) {
  assert(Expr && "Expression must not be null!");
  auto Result = true;
  if (auto Const = dyn_cast<ConstantInt>(Expr)) {
    Const->getValue().toString(Str, 10, true);
  } else if (auto Const = dyn_cast<ConstantFP>(Expr)) {
    Const->getValueAPF().toString(Str);
  } else if (auto GEP = dyn_cast<const GEPOperator>(Expr)) {
    Result = unparse(GEP, Str);
  } else if (auto LI = dyn_cast<const LoadInst>(Expr)) {
    if (!unparse(LI->getPointerOperand(), Str))
      return false;
    endDITypeIfNeed(Str);
    mIsDITypeEnd =
      !isa<DICompositeType>(mDIType) && !isa<DIDerivedType>(mDIType);
    mLastAddressChange = Expr;
  } else {
    mLastAddressChange = Expr;
    SmallVector<DIMemoryLocation, 2> DILocs;
    auto DILoc = findMetadata(Expr, DILocs, mDT);
    /// TODO (kaniandr@gmail.com): if some aggregate variable has been promoted
    /// than its metadata may contain not empty DIExpression. So, we should
    /// unparse this expression.
    if (!DILoc || DILoc->Expr && DILoc->Expr->getNumElements() > 0 ||
        isStubType(DILoc->Var->getType()))
      return false;
    assert(DILoc->Var && "Variable must not be null!");
    mDIType = stripDIType(DILoc->Var->getType());
    mIsDITypeEnd =
      !isa<DICompositeType>(mDIType) && !isa<DIDerivedType>(mDIType);
    auto Name = DILoc->Var->getName();
    if (Name.empty())
      Name = "sapfor.null";
    Str.append(Name.begin(), Name.end());
  }
  return Result;
}

bool DIUnparser::unparse(const GEPOperator *GEP, SmallVectorImpl<char> &Str) {
  assert(GEP && "Location must not be null!");
  auto Result = unparse(GEP->getPointerOperand(), Str);
  bool WasGEPChain = isa<GEPOperator>(mLastAddressChange);
  mLastAddressChange = GEP;
  if (!Result)
    return false;
  if (GEP->getNumOperands() < 3) {
    // TODO (kaniandr@gmail.com): Pointers must be defer unparsed.
    // If `mIsDITypeEnd = true` it means that current `mDIType` is not treated
    // as array. It may be a scalar type or a pointer without load instruction.
    // This case is not evaluated, since it seems too rare.
    // For example, `int N; &N[0]` lead to such situation.
    if (mIsDITypeEnd)
      return false;
    assert(!mIsDITypeEnd && "Pointers must be defer unparsed!");
    // It is not known how to match first operands and dimensions, so
    // it will be omitted now. It will be replaced with [?] when number
    // of defer dimensions will become clear.
    return true;
  }
  auto I = gep_type_begin(GEP), EI = gep_type_end(GEP);
  if (auto DIDTy = dyn_cast<DIDerivedType>(mDIType)) {
    if (DIDTy->getTag() != dwarf::DW_TAG_pointer_type)
      return false;
    assert(!mIsDITypeEnd && "Pointers must be defer unparsed!");
    Str.push_back('[');
    // If there was `getelementptr` instructions before this than it is not
    // simple to calculate offset and [?] will be used.
    if (!WasGEPChain) {
      auto Size = Str.size();
      if (!unparseToString(Str, I.getOperand(), mDT)) {
        Str.resize(Size);
        Str.push_back('?');
      }
    } else {
      Str.push_back('?');
    }
    Str.push_back(']');
    mDIType = stripDIType(DIDTy->getBaseType());
  }
  for (++I; I != EI; ++I) {
    if (I.isStruct()) {
      auto OpC = cast<ConstantInt>(I.getOperand());
      auto Idx = OpC->getZExtValue();
      auto DICTy = cast<DICompositeType>(mDIType);
      // If current debug type is array it means that it has not be entirely
      // unparsed and its string representation should be appended with [?]
      // for each dimension.
      if (DICTy->getTag() == dwarf::DW_TAG_array_type) {
        assert(!mIsDITypeEnd && "Array must be defer unparsed!");
        if (isa<DICompositeType>(DICTy->getBaseType())) {
          for (int I = 0; I < DICTy->getElements().size(); ++I)
            Str.append({ '[','?',']' });
          DICTy = cast<DICompositeType>(DICTy->getBaseType());
        }
      }
      // TODO (kaniandr@gmail.com): LLVM IR for Fortran programs uses structure
      // of array of bytes to store pool of data which may have different types.
      if (DICTy->getTag() != dwarf::DW_TAG_structure_type &&
          DICTy->getTag() != dwarf::DW_TAG_class_type)
        return false;
      auto El = cast<DIDerivedType>(DICTy->getElements()[Idx]);
      Str.push_back('.');
      Str.append(El->getName().begin(), El->getName().end());
      mDIType = stripDIType(El->getBaseType());
      mIsDITypeEnd = !isa<DICompositeType>(mDIType);
      continue;
    }
    auto DICTy = cast<DICompositeType>(mDIType);
    assert(DICTy->getTag() == dwarf::DW_TAG_array_type &&
      "Current debug type must be an array!");
    /// Replace all defer dimensions with [?].
    if (!mIsDITypeEnd)
      for (unsigned J = 0, EJ = DICTy->getElements().size()
        - dimensionsNum(I.getIndexedType()) - 1; J < EJ; ++J)
        Str.append({ '[','?',']' });
    Str.push_back('[');
    auto Size = Str.size();
    if (!unparseToString(Str, I.getOperand(), mDT)) {
      Str.resize(Size);
      Str.push_back('?');
    }
    Str.push_back(']');
    // Current indexed type may be an array for multidimensional arrays or
    // a type of an array element.
    if (!isa<ArrayType>(I.getIndexedType())) {
      mDIType = stripDIType(DICTy->getBaseType());
      mIsDITypeEnd = !isa<DICompositeType>(mDIType);
    } else {
      mIsDITypeEnd = true;
    }
  }
  return true;
}
