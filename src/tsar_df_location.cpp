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
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
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

std::string locationToSource(const Value *Loc) {
  bool NB;
  DITypeRef DITy;
  return detail::locationToSource(Loc, DITy, NB);
}

namespace detail {
std::string locationToSource(
    const Value *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  if (Operator::getOpcode(Loc) == Instruction::BitCast ||
      Operator::getOpcode(Loc) == Instruction::AddrSpaceCast)
    Loc = cast<Operator>(Loc)->getOperand(0);
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
  assert(isa<DIDerivedType>(DITy) &&
    "Type of loadable location is not a pointer!");
  DITy = cast<DIDerivedType>(DITy)->getBaseType();
  return "*" + (NB ? "(" + Str + ")" : Str);
}

std::string locationToSource(
    const GetElementPtrInst *Loc, DITypeRef &DITy, bool &NeadBracket) {
  assert(Loc && "Location must not be null!");
  NeadBracket = false;
  Type *Ty = Loc->getSourceElementType();
  if (!isa<StructType>(Ty))
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

