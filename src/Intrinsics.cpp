//===-- Instrinsics.cpp - TSAR Intrinsic Function Handling ------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements functions from Intrinsics.h which allow to process
// TSAR intrinsics.
//
//===----------------------------------------------------------------------===//

#include "Intrinsics.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <tuple>

using namespace llvm;
using namespace tsar;

/// Table of string intrinsic names indexed by enum value.
static const char * const IntrinsicNameTable[] = {
  "not_intrinsic",
#define GET_INTRINSIC_NAME_TABLE
#include "Intrinsics.gen"
#undef GET_INTRINSIC_NAME_TABLE
};

/// Kinds of types which can be used in an intrinsic prototype.
static enum TypeKind : unsigned {
#define GET_INTRINSIC_TYPE_KINDS
#include "Intrinsics.gen"
#undef GET_INTRINSIC_TYPE_KINDS
};

namespace {
/// This literal type contains offsets for a prototype in table of prototypes
/// (see PrototypeOffsetTable and PrototypeTable for details).
struct PrototypeDescriptor {
  unsigned Start;
  unsigned End;
};
}
///\brief Table of offsets in prototype table indexed by enum value.
///
/// Each intrinsic has a prototype which is described by a table of prototypes.
/// Each description record has start and end points which are stored in this
/// table. So start point of a prototype for an intrinsic Id can be accessed
/// in a following way `PrototypeTable[PrototypeOffsetTable[Id].first]`.
static constexpr PrototypeDescriptor PrototypeOffsetTable[] = {
#define PROTOTYPE(Start,End) {Start, End},
  PROTOTYPE(0,0) // there is no prototype for `tsar_not_intrinsic`
#define GET_INTRINSIC_PROTOTYPE_OFFSET_TABLE
#include "Intrinsics.gen"
#undef GET_INTRINSIC_PROTOTYPE_OFFSET_TABLE
#undef PROTOTYPE
};

/// Table of intrinsic prototypes indexed by records in prototype offset table.
static constexpr TypeKind PrototypeTable[] = {
#define GET_INTRINSIC_PROTOTYPE_TABLE
#include "Intrinsics.gen"
#undef GET_INTRINSIC_PROTOTYPE_TABLE
};

/// Builds LLVM type for a type which is described in PrototypeTable and is
/// started at a specified position.
static Type * DecodeType(LLVMContext &Ctx, unsigned &Start) {
  switch (PrototypeTable[Start]) {
  case Void: ++Start; return Type::getVoidTy(Ctx);
  case Any:  ++Start; return Type::getInt8Ty(Ctx);
  case Size: ++Start; return Type::getInt64Ty(Ctx);
  case Pointer: return PointerType::getUnqual(DecodeType(Ctx, ++Start));
  default:
    llvm_unreachable("Unknown kind of intrinsic parameter type!");
    return nullptr;
  }
}

namespace tsar {
StringRef getName(IntrinsicId Id) {
  assert(Id < IntrinsicId::num_intrinsics && Id > IntrinsicId::not_intrinsic &&
    "Invalid intrinsic ID!");
  return IntrinsicNameTable[static_cast<unsigned>(Id)];
}

FunctionType *getType(LLVMContext &Ctx, IntrinsicId Id) {
  auto Offset = PrototypeOffsetTable[static_cast<unsigned>(Id)];
  Type *ResultTy = DecodeType(Ctx, Offset.Start);
  SmallVector<Type *, 8> ArgsTys;
  while (Offset.Start < Offset.End)
    ArgsTys.push_back(DecodeType(Ctx, Offset.Start));
  return FunctionType::get(ResultTy, ArgsTys, false);
}

llvm::Function * getDeclaration(Module *M, IntrinsicId Id) {
  return cast<Function>(
    M->getOrInsertFunction(getName(Id), getType(M->getContext(), Id)));
}
}
