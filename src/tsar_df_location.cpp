//===--- tsar_df_location.cpp - Data Flow Framework ----- -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods declared in tsar_df_location.h
//
//===----------------------------------------------------------------------===//

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Debug.h>
#include "tsar_df_location.h"
#include "tsar_utility.h"
#include "tsar_dbg_output.h"

using namespace llvm;

namespace tsar {
std::pair<LocationSet::iterator, bool> LocationSet::insert(
  const llvm::MemoryLocation &Loc) {
  auto I = mLocations.find(Loc.Ptr);
  if (I == mLocations.end()) {
    MemoryLocation *NewLoc = new MemoryLocation(Loc);
    auto Pair = mLocations.insert(std::make_pair(Loc.Ptr, NewLoc));
    return std::make_pair(iterator(Pair.first), true);
  }
  bool isChanged = true;
  if (I->second->AATags != Loc.AATags)
    if (I->second->AATags == DenseMapInfo<AAMDNodes>::getEmptyKey())
      I->second->AATags = Loc.AATags;
    else
      I->second->AATags = DenseMapInfo<AAMDNodes>::getTombstoneKey();
  else
    isChanged = false;
  if (I->second->Size < Loc.Size) {
    I->second->Size = Loc.Size;
    isChanged = true;
  }
  return std::make_pair(iterator(I), isChanged);
}

bool LocationSet::intersect(const LocationSet &with) {
  if (this == &with)
    return false;
  MapTy PrevLocations;
  mLocations.swap(PrevLocations);
  bool isChanged = false;
  for (auto Pair : PrevLocations) {
    auto I = with.findContaining(*Pair.second);
    if (I != with.end()) {
      insert(*Pair.second);
      continue;
    }
    I = with.findCoveredBy(*Pair.second);
    if (I != with.end()) {
      insert(*I);
      isChanged = true;
    }
  }
  for (auto Pair : PrevLocations)
    delete Pair.second;
  return isChanged;
}

bool LocationSet::operator==(const LocationSet &RHS) const {
  if (this == &RHS)
    return true;
  if (mLocations.size() != RHS.mLocations.size())
    return false;
  for (auto Pair : mLocations) {
    auto I = RHS.mLocations.find(Pair.first);
    if (I == RHS.mLocations.end() ||
      I->second->Size != Pair.second->Size ||
      I->second->AATags != Pair.second->AATags)
      return false;
  }
  return true;
}

void LocationSet::print(raw_ostream &OS) const {
  for (auto Pair : mLocations) {
    printLocationSource(OS, Pair.second->Ptr);
    OS << " " << *Pair.second->Ptr << "\n";
  }
}

void LocationSet::dump() const { print(dbgs()); }

bool LocationDFValue::intersect(const LocationDFValue &with) {
  assert(mKind != INVALID_KIND && "Collection is corrupted!");
  assert(with.mKind != INVALID_KIND && "Collection is corrupted!");
  if (with.mKind == KIND_FULL)
    return false;
  if (mKind == KIND_FULL) {
    *this = with;
    return true;
  }
  return mLocations.intersect(with.mLocations);
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
  return mLocations.merge(with.mLocations);
}

void LocationDFValue::print(raw_ostream &OS) const {
  if (mKind == KIND_FULL)
    OS << "whole program memory\n";
  else
    mLocations.print(OS);
}

void LocationDFValue::dump() const { print(dbgs()); }

const Value * BaseLocationSet::stripPointer(const Value *Ptr) {
  assert(Ptr && "Pointer to memory location must not be null!");
  if (Operator::getOpcode(Ptr) == Instruction::BitCast ||
      Operator::getOpcode(Ptr) == Instruction::AddrSpaceCast ||
      Operator::getOpcode(Ptr) == Instruction::IntToPtr)
    return stripPointer(cast<Operator>(Ptr)->getOperand(0));
  if (auto LI = dyn_cast<const LoadInst>(Ptr))
    return stripPointer(LI->getPointerOperand());
  if (auto GEPI = dyn_cast<const GetElementPtrInst>(Ptr))
    return stripPointer(GEPI->getPointerOperand());
  assert((isa<const GlobalVariable>(Ptr) || isa<const AllocaInst>(Ptr)) &&
    "Unsupported type of pointer to memory location!");
  return Ptr;
}

void BaseLocationSet::stripToBase(MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  if (Operator::getOpcode(Loc.Ptr) == Instruction::BitCast ||
      Operator::getOpcode(Loc.Ptr) == Instruction::AddrSpaceCast ||
      Operator::getOpcode(Loc.Ptr) == Instruction::IntToPtr) {
    Loc.Ptr = cast<const Operator>(Loc.Ptr)->getOperand(0);
    return stripToBase(Loc);
  }
  if (auto GEPI = dyn_cast<const GetElementPtrInst>(Loc.Ptr)) {
    Type *Ty = GEPI->getSourceElementType();
    // TODO (kaniandr@gmail.com) : it is possible that sequence of
    // 'getelmentptr' instructions is represented as a single instruction.
    // If the result of it is a member of a structure this case must be
    // evaluated separately. It this moment only individual access to
    // members is supported: for struct STy {int X;}; it is
    // %X = getelementptr inbounds %struct.STy, %struct.STy* %S, i32 0, i32 0
    // Also fix it in isSameBase().
    if (!isa<StructType>(Ty) || GEPI->getNumIndices() != 2) {
      Loc.Ptr = GEPI->getPointerOperand();
      Loc.Size = MemoryLocation::UnknownSize;
      return stripToBase(Loc);
    }
  }
  if (!(isa<const GlobalVariable>(Loc.Ptr) || isa<const AllocaInst>(Loc.Ptr) ||
       isa<const GetElementPtrInst>(Loc.Ptr) || isa<const LoadInst>(Loc.Ptr))) {
    Loc.Ptr = nullptr;
    Loc.Size = MemoryLocation::UnknownSize;
  }
  return;
}

bool BaseLocationSet::isSameBase(
    const llvm::Value *BasePtr1, const llvm::Value *BasePtr2) {
  if (BasePtr1 == BasePtr2)
    return true;
  if (!BasePtr1 || !BasePtr2 ||
      BasePtr1->getValueID() != BasePtr2->getValueID())
    return false;
  if (auto LI = dyn_cast<const LoadInst>(BasePtr1))
    return isSameBase(LI->getPointerOperand(),
      cast<const LoadInst>(BasePtr2)->getPointerOperand());
  if (auto GEPI1 = dyn_cast<const GetElementPtrInst>(BasePtr1)) {
    auto GEPI2 = dyn_cast<const GetElementPtrInst>(BasePtr2);
    if (!isSameBase(GEPI1->getPointerOperand(), GEPI2->getPointerOperand()))
      return false;
    Type *Ty1 = GEPI1->getSourceElementType();
    Type *Ty2 = GEPI2->getSourceElementType();
    if (!isa<StructType>(Ty1) && !isa<StructType>(Ty2))
      return true;
    // TODO (kaniandr@gmail.com) : see stripToBase().
    if (Ty1 != Ty2 || GEPI1->getNumIndices() != 2 ||
        GEPI1->getNumIndices() != GEPI2->getNumIndices())
      return false;
    const Use *Idx1 = GEPI1->idx_begin();
    const Use *Idx2 = GEPI2->idx_begin();
    assert(isa<ConstantInt>(Idx1) && cast<ConstantInt>(Idx1)->isZero() &&
      "First index in getelemenptr for structure is not a zero value!");
    assert(isa<ConstantInt>(Idx2) && cast<ConstantInt>(Idx2)->isZero() &&
      "First index in getelemenptr for structure is not a zero value!");
    ++Idx1; ++Idx2;
    return cast<ConstantInt>(Idx1)->getZExtValue() ==
      cast<ConstantInt>(Idx2)->getZExtValue();
  }
  return false;
}

std::pair<BaseLocationSet::iterator, bool> BaseLocationSet::insert(
    const MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  LocationSet *LS;
  MemoryLocation Base(Loc);
  stripToBase(Base);
  const Value *StrippedPtr = Base.Ptr ? stripPointer(Base.Ptr) : nullptr;
  auto I = mBases.find(StrippedPtr);
  if (I != mBases.end()) {
    LS = I->second;
    for (MemoryLocation &Curr : *LS)
      if (isSameBase(Curr.Ptr, Base.Ptr)) {
        Base.Ptr = Curr.Ptr;
        break;
      }
  } else {
    LS = new LocationSet;
    mBases.insert(std::make_pair(StrippedPtr, LS));
  }
  auto Pair = LS->insert(Base);
  return std::make_pair(
    mBaseList.insert(Pair.first.operator->()).first,
    Pair.second);
}

BaseLocationSet::size_type BaseLocationSet::count(
    const MemoryLocation &Loc) const {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  MemoryLocation Base(Loc);
  stripToBase(Base);
  const Value *StrippedPtr = stripPointer(Base.Ptr);
  auto I = mBases.find(StrippedPtr);
  if (I != mBases.end()) {
    LocationSet *LS = I->second;
    for (MemoryLocation &Curr : *LS)
      if (isSameBase(Curr.Ptr, Base.Ptr)) {
        Base.Ptr = Curr.Ptr;
        return LS->contain(Base) ? 1 : 0;
      }
  }
  return 0;
}

std::string locationToSource(const Value *Loc) {
  bool NB;
  DITypeRef DITy;
  return detail::locationToSource(Loc, DITy, NB);
}

namespace detail {
std::string locationToSource(
    const Value *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  if (auto *LI = dyn_cast<const LoadInst>(Loc))
    return locationToSource(LI, DITy, NeadBracket);
  if (auto *GEPI = dyn_cast<const GetElementPtrInst>(Loc))
    return locationToSource(GEPI, DITy, NeadBracket);
  NeadBracket = false;
  DIVariable *DIVar;
  if (auto *Var = dyn_cast<const GlobalVariable>(Loc))
    DIVar = getMetadata(Var);
  else if (auto *AI = dyn_cast<const AllocaInst>(Loc))
    DIVar = getMetadata(AI);
  else
    return "";
  if (!DIVar)
    return "";
  DITy = DIVar->getType();
  return DIVar->getName();
}

std::string locationToSource(
    const LoadInst *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  NeadBracket = false;
  const Value *Ptr = Loc->getPointerOperand();
  bool NB;
  auto Str = locationToSource(Ptr, DITy, NB);
  if (Str.empty())
    return Str;
  // Type of location may not be a pointer if there is type case 'inttoptr':
  // %1 = load i32, i32* %x
  // %2 = inttoptr i32 % 1 to i8*
  // store i8 10, i8* %2
  // Type of %1 is not a pointer.
  if (isa<DIDerivedType>(DITy))
    DITy = cast<DIDerivedType>(DITy)->getBaseType();
  return "*" + (NB ? "(" + Str + ")" : Str);
}

std::string locationToSource(
    const GetElementPtrInst *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  NeadBracket = false;
  Type *Ty = Loc->getSourceElementType();
  if (!isa<StructType>(Ty) || Loc->getNumIndices() != 2)
    return "";
  StructType *StructTy = cast<StructType>(Ty);
  const Value *Ptr = Loc->getPointerOperand();
  bool NB;
  auto Str = locationToSource(Ptr, DITy, NB);
  if (Str.empty() || !isa<DICompositeType>(DITy))
    return "";
  if (NB || Str.front() == '*')
    Str = "(" + Str + ")";
  const Use *Idx = Loc->idx_begin(), *EIdx = Loc->idx_end();
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
        // There are following hierarchy of meta information for type of Z:
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
}

