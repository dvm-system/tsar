//===--- tsar_df_location.cpp - Data Flow Framework ----- ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methdos declared in tsar_df_location.h
//
//===----------------------------------------------------------------------===//

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include "tsar_df_location.h"
#include "tsar_utility.h"

using namespace llvm;

namespace tsar {
bool LocationDFValue::intersect(const LocationDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (with.mKind == KIND_FULL)
    return false;
  if (mKind == KIND_FULL) {
    *this = with;
    return true;
  }
  LocationSet PrevAllocas;
  mLocations.swap(PrevAllocas);
  for (Value *Loc : PrevAllocas) {
    if (with.mLocations.count(Loc))
      mLocations.insert(Loc);
  }
  return mLocations.size() != PrevAllocas.size();
}

bool LocationDFValue::merge(const LocationDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (mKind == KIND_FULL)
    return false;
  if (with.mKind == KIND_FULL) {
    mLocations.clear();
    mKind = KIND_FULL;
    return true;
  }
  bool isChanged = false;
  for (Value *AI : with.mLocations)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    isChanged = mLocations.insert(AI) || isChanged;
#else
    isChanged = mLocations.insert(AI).second || isChanged;
#endif
  return isChanged;
}

bool LocationDFValue::operator==(const LocationDFValue &RHS) const {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(RHS.mKind != INVALID_KIND && "Collection is corrupted!");
  if (this == &RHS || mKind == KIND_FULL && RHS.mKind == KIND_FULL)
    return true;
  if (mKind != RHS.mKind)
    return false;
  return mLocations == RHS.mLocations;
}

std::string locationToSource(Value *Loc) {
  bool NB;
  DITypeRef DITy;
  return detail::locationToSource(Loc, DITy, NB);
}

namespace detail {
std::string locationToSource(
    Value *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  if (LoadInst *LI = dyn_cast<LoadInst>(Loc))
    return locationToSource(LI, DITy, NeadBracket);
  if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(Loc))
    return locationToSource(GEPI, DITy, NeadBracket);
  NeadBracket = false;
  DIVariable *DIVar;
  if (GlobalVariable *Var = dyn_cast<GlobalVariable>(Loc))
    DIVar = getMetadata(Var);
  else if (AllocaInst *AI = dyn_cast<AllocaInst>(Loc))
    DIVar = getMetadata(AI);
  else
    return "";
  DITy = DIVar->getType();
  return DIVar->getName();
}

std::string locationToSource(
    LoadInst *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  NeadBracket = false;
  Value *Ptr = Loc->getPointerOperand();
  bool NB;
  auto Str = locationToSource(Ptr, DITy, NB);
  if (Str.empty())
    return Str;
  assert(isa<DIDerivedType>(DITy) &&
    "Type of loadable location is not a pointer!");
  DITy = cast<DIDerivedType>(DITy)->getBaseType();
  return "*" + (NB ? "(" + Str + ")" : Str);
}

std::string locationToSource(
    GetElementPtrInst *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  NeadBracket = false;
  Type *Ty = Loc->getSourceElementType();
  if (!isa<StructType>(Ty))
    return "";
  StructType *StructTy = cast<StructType>(Ty);
  Value *Ptr = Loc->getPointerOperand();
  bool NB;
  auto Str = locationToSource(Ptr, DITy, NB);
  if (Str.empty() || !isa<DICompositeType>(DITy))
    return "";
  if (NB || Str.front() == '*')
    Str = "(" + Str + ")";
  Use *Idx = Loc->idx_begin(), *EIdx = Loc->idx_end();
  assert(isa<ConstantInt>(Idx) && cast<ConstantInt>(Idx)->isZero() &&
    "First index in getelemenptr for structure is not a zero value!");
  for (++Idx;;) {
    uint64_t ElNum = 0;
    for (auto El : cast<DICompositeType>(DITy)->getElements())
      if (ElNum++ == cast<ConstantInt>(Idx)->getZExtValue()) {
        Str += "." + cast<DIType>(El)->getName().str();
        // Each element of a composite type is a derived type.
        // For example, let us consider the following C-code:
        // struct S {int X;};
        // struct S Z;
        // There are following heararche of meta information for type of Z:
        // DICompositeType with the S name and one element which is
        //   DIDerivedType with the X name and base type
        //     DIBaseType with the int name.
        DITy = DITypeRef(cast<DIDerivedType>(El)->getBaseType());
        break;
      }
    if (++Idx == EIdx)
      break;
    if (!isa<DICompositeType>(DITy))
      return "";
  }
  return Str;
}
}

Value * findLocationBase(Value *Loc) {
  assert(Loc && "Location must not be null!");
  assert((isa<AllocaInst>(Loc) || isa<GlobalVariable>(Loc) ||
    isa<LoadInst>(Loc) || isa<GetElementPtrInst>(Loc)) &&
    "Unsupported type of location!");
  auto Src = locationToSource(Loc);
  if (!Src.empty())
    return Loc;
  if (LoadInst *LI = dyn_cast<LoadInst>(Loc))
    Loc = LI->getPointerOperand();
  else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(Loc))
    Loc = GEPI->getPointerOperand();
  else
    return Loc;
  return findLocationBase(Loc);
}
}

