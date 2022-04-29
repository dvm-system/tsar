//===-- Instrinsics.cpp - TSAR Intrinsic Function Handling ------*- C++ -*-===//
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
// This file implements functions from Intrinsics.h which allow to process
// TSAR intrinsics.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Intrinsics.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <tuple>

using namespace tsar;

using llvm::FunctionType;
using llvm::LLVMContext;
using llvm::Module;
using llvm::PointerType;
using llvm::StringRef;
using llvm::Type;

/// Table of string intrinsic names indexed by enum value.
static const char * const IntrinsicNameTable[] = {
  "not_intrinsic",
#define GET_INTRINSIC_NAME_TABLE
#include "tsar/Analysis/Intrinsics.gen"
#undef GET_INTRINSIC_NAME_TABLE
};

namespace {
/// Kinds of types which can be used in an intrinsic prototype.
enum TypeKind : unsigned {
#define GET_INTRINSIC_TYPE_KINDS
#include "tsar/Analysis/Intrinsics.gen"
#undef GET_INTRINSIC_TYPE_KINDS
};

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
#include "tsar/Analysis/Intrinsics.gen"
#undef GET_INTRINSIC_PROTOTYPE_OFFSET_TABLE
#undef PROTOTYPE
};

/// Table of intrinsic prototypes indexed by records in prototype offset table.
static constexpr TypeKind PrototypeTable[] = {
#define GET_INTRINSIC_PROTOTYPE_TABLE
#include "tsar/Analysis/Intrinsics.gen"
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
  llvm::SmallVector<Type *, 8> ArgsTys;
  while (Offset.Start < Offset.End)
    ArgsTys.push_back(DecodeType(Ctx, Offset.Start));
  return FunctionType::get(ResultTy, ArgsTys, false);
}

llvm::FunctionCallee getDeclaration(llvm::Module *M, IntrinsicId Id) {
  return M->getOrInsertFunction(getName(Id), getType(M->getContext(), Id));
}

bool getTsarLibFunc(StringRef funcName, IntrinsicId &Id) {
  const char* const *Start = &IntrinsicNameTable[0];
  const char* const *End =
    &IntrinsicNameTable[(unsigned)IntrinsicId::num_intrinsics];
  if (funcName.empty())
    return false;
  const char* const *I = std::find(Start, End, funcName);
  if (I != End) {
    Id = (IntrinsicId)(I - Start);
    return true;
  }
  return false;
}
}
