//===-- Instrinsics.h - TSAR Intrinsic Function Handling --------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
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
#include "Intrinsics.gen"
#undef GET_INTRINSIC_ENUM_VALUES
  num_intrinsics
};

/// Returns the name for an intrinsic with no overloads.
llvm::StringRef getName(IntrinsicId Id);

/// Returns the function type for an intrinsic.
llvm::FunctionType *getType(llvm::LLVMContext &Ctx, IntrinsicId Id);

/// Creates or inserts an LLVM Function declaration for an intrinsic.
llvm::Function * getDeclaration(llvm::Module *M, IntrinsicId Id);
}
#endif//TSAR_INTRINSICS_H
