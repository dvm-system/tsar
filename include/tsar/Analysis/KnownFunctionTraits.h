//===- KnownFunctionTraits.h - Known Function Handling ----------*- C++ -*-===//
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
// This file defines a set of functions which allow processing of some known
// functions, for example intrinsic and library functions. To define properties
// of these known functions IntrinsicTraits and LibFuncTraits templates are
// specialized.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_KNOWN_FUNCTION_TRAITS_H
#define TSAR_KNOWN_FUNCTION_TRAITS_H

#include <bcl/utility.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/IntrinsicsARM.h>

namespace tsar {
/// Returns 'true' if a specified intrinsic indicates some information about
/// memory objects only and does not access the memory.
inline bool isMemoryMarkerIntrinsic(llvm::Intrinsic::ID Id) noexcept {
  switch (Id) {
  case llvm::Intrinsic::lifetime_start: case llvm::Intrinsic::lifetime_end:
  case llvm::Intrinsic::invariant_start: case llvm::Intrinsic::invariant_end:
  case llvm::Intrinsic::sideeffect: case llvm::Intrinsic::assume:
  case llvm::Intrinsic::experimental_noalias_scope_decl:
    return true;
  }
  return false;
}

/// \brief This proposes traits of some known intrinsic functions.
///
/// This class should be specialized by different function ids.
template<llvm::Intrinsic::ID Id> struct IntrinsicTraits {
  template<class Function> static void foreachMemoryArgument(Function &&F) {}
};

/// \brief This proposes traits of some known library functions.
///
/// This class should be specialized by different function ids.
template<llvm::LibFunc Id> struct LibFuncTraits {
  template<class Function> static void foreachMemoryArgument(Function &&F) {}
};

/// This allows processing of arguments which accesses memory locations.
template<unsigned... Args> struct KnownFunctionMemoryArgument {
  /// Applies a specified function to each argument which accesses memory.
  template<class Function> static void foreachMemoryArgument(Function &&F) {
    bcl::staticForeach(std::forward<Function>(F), Args...);
  }
};

/// Applies a specified function to each argument (of intrinsic function `Id`)
/// which accesses memory.
template<llvm::Intrinsic::ID Id, class Function>
inline void foreachIntrinsicMemArg(Function &&F) {
  IntrinsicTraits<Id>::foreachMemoryArgument(std::forward<Function>(F));
}

/// Applies a specified function to each argument (of library function `Id`)
/// which accesses memory.
template<llvm::LibFunc Id, class Function>
inline void foreachLibFuncMemArg(Function &&F) {
  LibFuncTraits<Id>::foreachMemoryArgument(std::forward<Function>(F));
}

//===----------------------------------------------------------------------===//
// IntrinsicTraits specializations for LLVM intrinsics
//===----------------------------------------------------------------------===//
template<> struct IntrinsicTraits<llvm::Intrinsic::memset> :
  public KnownFunctionMemoryArgument<0> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::memcpy> :
  public KnownFunctionMemoryArgument<0, 1> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::memmove> :
  public KnownFunctionMemoryArgument<0, 1> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::lifetime_start> :
  public KnownFunctionMemoryArgument<1> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::lifetime_end> :
  public KnownFunctionMemoryArgument<1> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::invariant_start> :
  public KnownFunctionMemoryArgument<1> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::invariant_end> :
  public KnownFunctionMemoryArgument<2> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::arm_neon_vld1> :
  public KnownFunctionMemoryArgument<0> {};
template<> struct IntrinsicTraits<llvm::Intrinsic::arm_neon_vst1> :
  public KnownFunctionMemoryArgument<0> {};

/// Applies a specified function to each argument which accesses memory.
template<class Function>
void foreachIntrinsicMemArg(llvm::Intrinsic::ID Id, Function &&F) {
  using namespace llvm;
  switch (Id) {
  case Intrinsic::memset:
    foreachIntrinsicMemArg<Intrinsic::memset>(std::forward<Function>(F));
    return;
  case Intrinsic::memcpy:
    foreachIntrinsicMemArg<Intrinsic::memcpy>(std::forward<Function>(F));
    return;
  case Intrinsic::memmove:
    foreachIntrinsicMemArg<Intrinsic::memmove>(std::forward<Function>(F));
    return;
  case Intrinsic::lifetime_start:
    foreachIntrinsicMemArg<Intrinsic::lifetime_start>(std::forward<Function>(F));
    return;
  case Intrinsic::lifetime_end:
    foreachIntrinsicMemArg<Intrinsic::lifetime_end>(std::forward<Function>(F));
    return;
  case Intrinsic::invariant_start:
    foreachIntrinsicMemArg<Intrinsic::invariant_start>(std::forward<Function>(F));
    return;
  case Intrinsic::invariant_end:
    foreachIntrinsicMemArg<Intrinsic::invariant_end>(std::forward<Function>(F));
    return;
  case Intrinsic::arm_neon_vld1:
    foreachIntrinsicMemArg<Intrinsic::arm_neon_vld1>(std::forward<Function>(F));
    return;
  case Intrinsic::arm_neon_vst1:
    foreachIntrinsicMemArg<Intrinsic::arm_neon_vst1>(std::forward<Function>(F));
    return;
  }
}

/// Applies a specified function to each argument which accesses memory.
template<class Function>
void foreachIntrinsicMemArg(const llvm::IntrinsicInst &II, Function &&F) {
  foreachIntrinsicMemArg(II.getIntrinsicID(), std::forward<Function>(F));
}

//===----------------------------------------------------------------------===//
// LibFunc specializations for LLVM library functions
//===----------------------------------------------------------------------===//
template<> struct LibFuncTraits<llvm::LibFunc_memset_pattern16> :
  public KnownFunctionMemoryArgument<0, 1> {};

/// Applies a specified function to each argument which accesses memory.
template<class Function>
void foreachLibFuncMemArg(llvm::LibFunc Id, Function &&F) {
  using namespace llvm;
  switch (Id) {
  case LibFunc_memset_pattern16:
    foreachLibFuncMemArg<LibFunc_memset_pattern16>(std::forward<Function>(F));
    return;
  }
}

/// Applies a specified function to each argument which accesses memory.
template<class Function>
void foreachLibFuncMemArg(const llvm::Function &Func,
    const llvm::TargetLibraryInfo &TLI, Function &&F) {
  using namespace llvm;
  LibFunc Id;
  if (!TLI.getLibFunc(Func, Id) && !TLI.has(Id))
    return;
  return foreachLibFuncMemArg(Id, std::forward<Function>(F));
}
}
#endif//TSAR_KNOWN_FUNCTION_TRAITS_H
