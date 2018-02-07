//===--- DIUnparser.cpp ------ Debug Info Unparser --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements unparser to print low-level LLVM IR in a more readable
// form, similar to high-level language.
//
//===----------------------------------------------------------------------===//

#include "DIUnparser.h"
#include "tsar_utility.h"
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Instructions.h>

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
      !isa<GEPOperator>(mLastAddressChange) &&
      !isa<LoadInst>(mLastAddressChange))
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
    if (isa<GEPOperator>(mLastAddressChange))
      Str.append({ '[','?',']' });
    else if (isa<LoadInst>(mLastAddressChange))
      Str.append({ '[','0',']' });
    mDIType = stripDIType(DIDTy->getBaseType());
  } else {
    llvm_unreachable("Unsupported debug type!");
  }
  mIsDITypeEnd = !isa<DICompositeType>(mDIType);
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
    DIVariable *DIVar;
    if (auto *Var = dyn_cast<const GlobalVariable>(Expr))
      DIVar = getMetadata(Var);
    else if (auto *AI = dyn_cast<const AllocaInst>(Expr))
      DIVar = getMetadata(AI);
    else
      return false;
    if (!DIVar)
      return false;
    mDIType = stripDIType(DIVar->getType());
    mIsDITypeEnd = !isa<DICompositeType>(mDIType);
    auto Name = DIVar->getName();
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
      if (!unparseToString(I.getOperand(), Str)) {
        Str.set_size(Size);
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
        for (int I = 0; I < DICTy->getElements().size(); ++I)
          Str.append({ '[','?',']' });
        DICTy = cast<DICompositeType>(DICTy->getBaseType());
      }
      assert((DICTy->getTag() == dwarf::DW_TAG_structure_type ||
        DICTy->getTag() == dwarf::DW_TAG_array_type) &&
        "It must be aggregate type!");
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
    if (!unparseToString(I.getOperand(), Str)) {
      Str.set_size(Size);
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
