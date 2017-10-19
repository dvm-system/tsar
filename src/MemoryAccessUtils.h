//===- MemoryAccessUtils.h - Utils For Exploring Memory Accesses -- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines useful functions to explore accesses to memory locations
// from instructions, basic blocks and functions.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_ESTIMATE_MEMORY_UTILS_H
#define TSAR_ESTIMATE_MEMORY_UTILS_H

#include "KnownFunctionTraits.h"
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <utility>

namespace tsar {
/// \brief Applies a specified function to each memory location accessed in a
/// specified instruction.
///
/// The function `Func` must have the following prototype:
/// `void Func(llvm::Instruction &I, llvm::MemoryLocation &&Loc,
///            bool IsRead, bool IsWrite)`
/// - `IsRead` means that a location `Loc` is read,
/// - `IsWrite` means that it is written.
/// The function `UnknownFunc` evaluate accesses to unknown memory which occurs
/// for example in function calls. It must have the following prototype:
/// `void UnknownFunc(llvm::Instruction &I, bool IsRead, bool IsWrite).
template<class FuncTy, class UnknownFuncTy>
void for_each_memory(llvm::Instruction &I, llvm::TargetLibraryInfo &TLI,
    FuncTy &&Func, UnknownFuncTy &&UnknownFunc) {
  auto traverseActualParams = [&TLI, &Func, &UnknownFunc](
      ImmutableCallSite CS) {
    LibFunc::Func LibId;
    if (auto II = dyn_cast<IntrinsicInst>(CS.getInstruction())) {
      foreachIntrinsicMemArg(*II, [&CS, &TLI, &Func](unsigned Idx) {
        Func(*CS.getInstruction(), MemoryLocation::getForArgument(CS, Idx, TLI),
          !CS.doesNotReadMemory(), !CS.onlyReadsMemory());
      });
    } else if (TLI.getLibFunc(*CS.getCalledFunction(), LibId)) {
      foreachLibFuncMemArg(LibId, [&CS, &TLI, &Func](unsigned Idx) {
        Func(*CS.getInstruction(), MemoryLocation::getForArgument(CS, Idx, TLI),
          !CS.doesNotReadMemory(), !CS.onlyReadsMemory());
      });
    } else {
      for (unsigned Idx = 0; Idx < CS.arg_size(); ++Idx) {
        assert(CS.getArgument(Idx)->getType() &&
          "All actual parameters must be typed!");
        if (!CS.getArgument(Idx)->getType()->isPointerTy())
          continue;
        Func(*CS.getInstruction(), MemoryLocation::getForArgument(CS, Idx, TLI),
         !CS.doesNotReadMemory(), !CS.onlyReadsMemory());
      }
    }
    if (!CS.onlyAccessesArgMemory())
      UnknownFunc(*CS.getInstruction(),
        !CS.doesNotReadMemory(), !CS.onlyReadsMemory());
  };
  switch (I.getOpcode()) {
  case Instruction::Load: case Instruction::Store: case Instruction::VAArg:
  case Instruction::AtomicRMW: case Instruction::AtomicCmpXchg:
    Func(I, MemoryLocation::get(&I),
      I.mayReadFromMemory(), I.mayWriteToMemory());
    break;
  case Instruction::Call:
    traverseActualParams(cast<CallInst>(&I)); break;
  case Instruction::Invoke:
    traverseActualParams(cast<InvokeInst>(&I)); break;
  }
}

/// \brief Applies a specified function to each memory location accessed in a
/// specified function.
///
/// The function `Func` must have the following prototype:
/// `void Func(llvm::Instruction &I, llvm::MemoryLocation &&Loc,
///            bool IsRead, bool IsWrite)`
/// - `IsRead` means that a location `Loc` is read,
/// - `IsWrite` means that it is written.
/// The function `UnknownFunc` evaluate accesses to unknown memory which occurs
/// for example in function calls. It must have the following prototype:
/// `void UnknownFunc(llvm::Instruction &I, bool IsRead, bool IsWrite).
template<class FuncTy, class UnknownFuncTy>
void for_each_memory(llvm::Function &F, llvm::TargetLibraryInfo &TLI,
    FuncTy &&Func, UnknownFuncTy &&UnknownFunc) {
  for (auto &I : make_range(inst_begin(F), inst_end(F)))
    for_each_memory(I, TLI, std::forward<FuncTy>(Func),
      std::forward<UnknownFuncTy>(UnknownFunc));
}

///\brief Applies a specified function to each memory location accessed in a
/// specified basic block.
///
/// The function `Func` must have the following prototype:
/// `void Func(llvm::Instruction &I, llvm::MemoryLocation &&Loc,
///            bool IsRead, bool IsWrite)`
/// - `IsRead` means that a location `Loc` is read,
/// - `IsWrite` means that it is written.
/// The function `UnknownFunc` evaluate accesses to unknown memory which occurs
/// for example in function calls. It must have the following prototype:
/// `void UnknownFunc(llvm::Instruction &I, bool IsRead, bool IsWrite).
template<class FuncTy, class UnknownFuncTy>
void for_each_memory(llvm::BasicBlock &BB, llvm::TargetLibraryInfo &TLI,
    FuncTy &&Func, UnknownFuncTy &&UnknownFunc) {
  for (auto &I : BB)
    for_each_memory(I, TLI, std::forward<FuncTy>(Func),
      std::forward<UnknownFuncTy>(UnknownFunc));
}
}
#endif//TSAR_ESTIMATE_MEMORY_UTILS_H
