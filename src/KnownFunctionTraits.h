//===- KnownFunctionTraits.h - Known Function Handling ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

namespace tsar {
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
  public KnownFunctionMemoryArgument<0, 1> {};
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
