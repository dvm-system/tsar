//===-- Instrinsics.h - TSAR Intrinsic Function Handling --------*- C++ -*-===//
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
// This file defines a enum of intrinsic identifiers supported by TSAR. Some
// helpful methods to process these intrinsics are also defined. The TSAR
// intrinsic functions is a normal functions (not LLVM intrinsics) which are
// used for analyzer to obtain some special information about a program at
// runtime.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_INTRINSICS_H
#define TSAR_INTRINSICS_H

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DerivedTypes.h>

namespace llvm {
class Function;
class FunctionType;
class Module;
class LLVMContext;
}

namespace tsar {
/// This namespace contains an enum with a value for every intrinsic/builtin
/// function known by TSAR.
enum class IntrinsicId : unsigned {
  not_intrinsic = 0,
#define GET_INTRINSIC_ENUM_VALUES
#include "tsar/Analysis/Intrinsics.gen"
#undef GET_INTRINSIC_ENUM_VALUES
  num_intrinsics
};

/// Returns the name for an intrinsic with no overloads.
llvm::StringRef getName(IntrinsicId Id);

/// Returns the function type for an intrinsic.
llvm::FunctionType *getType(llvm::LLVMContext &Ctx, IntrinsicId Id);

/// Creates or inserts an LLVM Function declaration for an intrinsic.
llvm::FunctionCallee getDeclaration(llvm::Module *M, IntrinsicId Id);

/// Find tsar function by name. Return false if not found.
bool getTsarLibFunc(llvm::StringRef funcName, IntrinsicId &Id);
}
#endif//TSAR_INTRINSICS_H
