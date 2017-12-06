//===- DIMemoryLocation.cpp - Debug Level Memory Location -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file provides utility analysis objects describing memory locations.
//
//===----------------------------------------------------------------------===//

#include "DIMemoryLocation.h"
#include <llvm/ADT/SmallBitVector.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/DebugInfoMetadata.h>

using namespace llvm;
using namespace tsar;

bool DIMemoryLocation::hasDeref() const {
  for (auto I = Expr->expr_op_begin(), E = Expr->expr_op_end(); I != E; ++I)
    if (I->getOp() == dwarf::DW_OP_deref)
      return true;
  return false;
}

bool DIMemoryLocation::startsWithDeref() const {
  assert(isValid() && "Debug memory location is invalid!");
  return Expr->startsWithDeref();
}

bool DIMemoryLocation::isSized() const {
  assert(isValid() && "Debug memory location is invalid!");
  return !hasDeref() || Expr->isFragment();
}

uint64_t DIMemoryLocation::getSize() const {
  assert(isValid() && "Debug memory location is invalid!");
  auto Fragment = Expr->getFragmentInfo();
  if (Fragment.hasValue())
    return (Fragment->SizeInBits + 7) / 8;
  if (hasDeref())
    return llvm::MemoryLocation::UnknownSize;
  if (auto Ty = Var->getType().resolve()) {
    // There is no dereference and size of type is known, so try to determine
    // size. We should check that last offset does not lead to out of range
    // memory access.
    SmallVector<uint64_t, 1> Offsets;
    SmallBitVector SignMask;
    getOffsets(Offsets, SignMask);
    bool IsPositiveOffset = SignMask.test(0);
    uint64_t TySize = (Ty->getSizeInBits() + 7) / 8;
    if (IsPositiveOffset && TySize > Offsets.back())
      return TySize - Offsets.back();
  }
  // Return UnknownSize in case of out of range memory access.
  return llvm::MemoryLocation::UnknownSize;
}
void DIMemoryLocation::getOffsets(
    SmallVectorImpl<uint64_t> &Offsets, SmallBitVector &SignMask) const {
  assert(isValid() && "Debug memory location is invalid!");
  SmallBitVector RevertSignMask;
  uint64_t PositiveOffset = 0;
  uint64_t NegativeOffset = 0;
  auto push = [&PositiveOffset, &NegativeOffset, &RevertSignMask, &Offsets]() {
    RevertSignMask.resize(Offsets.size() + 1);
    if (PositiveOffset >= NegativeOffset) {
      Offsets.push_back((PositiveOffset - NegativeOffset) / 8);
    } else {
      Offsets.push_back((NegativeOffset - PositiveOffset) / 8);
      RevertSignMask.set(Offsets.size() - 1);
    }
    NegativeOffset = PositiveOffset = 0;
  };
  for (auto I = Expr->expr_op_begin(), E = Expr->expr_op_end(); I != E; ++I) {
    switch (I->getOp()) {
    default:
      llvm_unreachable("Unsupported kind of DWARF expression!");
    case dwarf::DW_OP_deref: push(); break;
    case dwarf::DW_OP_plus: PositiveOffset += I->getArg(0); break;
    case dwarf::DW_OP_minus: NegativeOffset += I->getArg(0); break;
    case dwarf::DW_OP_LLVM_fragment:
      PositiveOffset += Expr->getFragmentInfo(I, E)->OffsetInBits; break;
    }
  }
  push();
  RevertSignMask.flip();
  SignMask = std::move(RevertSignMask);
}

bool DIMemoryLocation::isValid() const {
  return Var && Expr && Expr->isValid() && !Expr->isConstant();
}
