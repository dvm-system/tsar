//===- DIMemoryLocation.cpp - Debug Level Memory Location -------*- C++ -*-===//
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
// This file provides utility analysis objects describing memory locations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include "tsar/Support/MetadataUtils.h"
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
  auto Fragment = Expr->getFragmentInfo();
  if (Fragment.hasValue())
    return Fragment->SizeInBits != 0;
  return !hasDeref();
}

LocationSize DIMemoryLocation::getSize() const {
  assert(isValid() && "Debug memory location is invalid!");
  auto Fragment = Expr->getFragmentInfo();
  if (Fragment.hasValue())
    return Fragment->SizeInBits == 0
               ? !AfterPointer ? LocationSize::beforeOrAfterPointer()
                               : LocationSize::afterPointer()
               : LocationSize::precise((Fragment->SizeInBits + 7) / 8);
  if (hasDeref())
    return AfterPointer ? LocationSize::afterPointer()
                        : LocationSize::beforeOrAfterPointer();
  if (auto Ty = stripDIType(Var->getType())) {
    // There is no dereference and size of type is known, so try to determine
    // size. We should check that the last offset does not lead to out of range
    // memory access.
    SmallVector<uint64_t, 1> Offsets;
    SmallBitVector SignMask;
    getOffsets(Offsets, SignMask);
    bool IsPositiveOffset = SignMask.test(0);
    uint64_t TySize = (Ty->getSizeInBits() + 7) / 8;
    if (IsPositiveOffset && TySize > Offsets.back())
      return LocationSize::precise(TySize - Offsets.back());
  }
  // Return UnknownSize in case of out of range memory access.
  return AfterPointer ? LocationSize::afterPointer()
                      : LocationSize::beforeOrAfterPointer();
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
  uint64_t Offset = 0;
  for (auto I = Expr->expr_op_begin(), E = Expr->expr_op_end(); I != E; ++I) {
    switch (I->getOp()) {
    default:
      llvm_unreachable("Unsupported kind of DWARF expression!");
    case dwarf::DW_OP_deref: push(); break;
    case dwarf::DW_OP_constu: Offset = I->getArg(0); break;
    case dwarf::DW_OP_plus_uconst: PositiveOffset += I->getArg(0); break;
    case dwarf::DW_OP_plus: PositiveOffset += Offset; break;
    case dwarf::DW_OP_minus: NegativeOffset += Offset; break;
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

DIMemoryLocation DIMemoryLocation::get(DbgVariableIntrinsic *Inst) {
  assert(Inst && "Instruction must not be null!");
  auto *Expr = Inst->getExpression();
  assert(Expr && Expr->isValid() && "Expression must be valid!");
  if (auto Frag = Expr->getFragmentInfo())
    Expr = DIExpression::get(Expr->getContext(),
      { dwarf::DW_OP_LLVM_fragment, Frag->OffsetInBits, Frag->SizeInBits });
  else
    Expr = DIExpression::get(Expr->getContext(), {});
  auto *Var = cast_or_null<DIVariable>(Inst->getVariable());
  auto DbgLoc = Inst->getDebugLoc();
  auto *Location = !Var || DbgLoc && DbgLoc.getLine() != 0 ? DbgLoc.get() :
      DILocation::get(Var->getContext(), Var->getLine(), 0, Var->getScope());
  DIMemoryLocation DILoc{Var, Expr, Location};
  if (!DILoc.getSize().mayBeBeforePointer())
    DILoc.AfterPointer = true;
  return DILoc;
}
