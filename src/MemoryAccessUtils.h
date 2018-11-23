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
#include <bcl/trait.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <utility>

namespace tsar {
/// Flags indicating assurance in memory access.
enum class AccessInfo : uint8_t { No, May, Must };

/// \brief Applies a specified function to each memory location accessed in a
/// specified instruction.
///
/// The function `Func` must have the following prototype:
/// `void Func(llvm::Instruction &I, llvm::MemoryLocation &&Loc, unsigned OpIdx,
///            AccessInfo IsRead, AccessInfo IsWrite)`
/// The function `UnknownFunc` evaluate accesses to unknown memory which occurs
/// for example in function calls. It must have the following prototype:
/// `void UnknownFunc(llvm::Instruction &I,
///                   AccessInfo IsRead, AccessInfo IsWrite).
/// Note, that alias analysis is not used to determine access type.
template<class FuncTy, class UnknownFuncTy>
void for_each_memory(llvm::Instruction &I, llvm::TargetLibraryInfo &TLI,
    FuncTy &&Func, UnknownFuncTy &&UnknownFunc) {
  using llvm::CallSite;
  using llvm::cast;
  using llvm::Function;
  using llvm::Instruction;
  using llvm::IntrinsicInst;
  using llvm::MemoryLocation;
  using llvm::StoreInst;
  auto traverseActualParams = [&TLI, &Func, &UnknownFunc](CallSite CS) {
    auto Callee =
      llvm::dyn_cast<Function>(CS.getCalledValue()->stripPointerCasts());
    llvm::LibFunc LibId;
    if (auto II = llvm::dyn_cast<IntrinsicInst>(CS.getInstruction())) {
      bool IsMarker = isMemoryMarkerIntrinsic(II->getIntrinsicID());
      foreachIntrinsicMemArg(*II, [IsMarker, &CS, &TLI, &Func](unsigned Idx) {
        Func(*CS.getInstruction(), MemoryLocation::getForArgument(CS, Idx, TLI),
          Idx,
          (CS.doesNotReadMemory() || IsMarker) ? AccessInfo::No : AccessInfo::May,
          (CS.onlyReadsMemory() || IsMarker) ? AccessInfo::No : AccessInfo::May);
      });
    } else if (Callee && TLI.getLibFunc(*Callee, LibId)) {
      foreachLibFuncMemArg(LibId, [&CS, &TLI, &Func](unsigned Idx) {
        Func(*CS.getInstruction(), MemoryLocation::getForArgument(CS, Idx, TLI),
          Idx, CS.doesNotReadMemory() ? AccessInfo::No : AccessInfo::May,
          CS.onlyReadsMemory() ? AccessInfo::No : AccessInfo::May);
      });
    } else {
      for (unsigned Idx = 0; Idx < CS.arg_size(); ++Idx) {
        assert(CS.getArgument(Idx)->getType() &&
          "All actual parameters must be typed!");
        if (!CS.getArgument(Idx)->getType()->isPointerTy())
          continue;
        Func(*CS.getInstruction(), MemoryLocation::getForArgument(CS, Idx, TLI),
         Idx, CS.doesNotReadMemory() ? AccessInfo::No : AccessInfo::May,
         CS.onlyReadsMemory() ? AccessInfo::No : AccessInfo::May);
      }
    }
    if (!CS.onlyAccessesArgMemory())
      UnknownFunc(*CS.getInstruction(),
        CS.doesNotReadMemory() ? AccessInfo::No : AccessInfo::May,
        CS.onlyReadsMemory() ? AccessInfo::No : AccessInfo::May);
  };
  switch (I.getOpcode()) {
  default:
    if (!I.mayReadOrWriteMemory())
      return;
    UnknownFunc(I,
      I.mayReadFromMemory() ? AccessInfo::May : AccessInfo::No,
      I.mayWriteToMemory() ? AccessInfo::May : AccessInfo::No);
    break;
  case Instruction::Load: case Instruction::VAArg:
  case Instruction::AtomicRMW: case Instruction::AtomicCmpXchg:
    assert(MemoryLocation::get(&I).Ptr == I.getOperand(0) &&
      "Operand with a specified number must be a specified memory location!");
    Func(I, MemoryLocation::get(&I), 0,
      I.mayReadFromMemory() ? AccessInfo::Must : AccessInfo::No,
      I.mayWriteToMemory() ? AccessInfo::Must : AccessInfo::No);
    break;
  case Instruction::Store:
    assert(MemoryLocation::get(&I).Ptr == I.getOperand(1) &&
      "Operand with a specified number must be a specified memory location!");
    Func(I, MemoryLocation::get(cast<StoreInst>(&I)), 1,
      AccessInfo::No, AccessInfo::Must);
    break;
  case Instruction::Call: case Instruction::Invoke:
    traverseActualParams(CallSite(&I)); break;
  }
}

/// \brief Applies a specified function to each memory location accessed in a
/// specified function.
///
/// The function `Func` must have the following prototype:
/// `void Func(llvm::Instruction &I, llvm::MemoryLocation &&Loc, unsigned OpIdx,
///            AccessInfo IsRead, AccessInfo IsWrite)`
/// The function `UnknownFunc` evaluate accesses to unknown memory which occurs
/// for example in function calls. It must have the following prototype:
/// `void UnknownFunc(llvm::Instruction &I,
///                   AccessInfo IsRead, AccessInfo IsWrite).
/// Note, that alias analysis is not used to determine access type.
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
/// `void Func(llvm::Instruction &I, llvm::MemoryLocation &&Loc, unsigned OpIdx,
///            AccessInfo IsRead, AccessInfo IsWrite)`
/// The function `UnknownFunc` evaluate accesses to unknown memory which occurs
/// for example in function calls. It must have the following prototype:
/// `void UnknownFunc(llvm::Instruction &I,
///                   AccessInfo IsRead, AccessInfo IsWrite).
/// Note, that alias analysis is not used to determine access type.
template<class FuncTy, class UnknownFuncTy>
void for_each_memory(llvm::BasicBlock &BB, llvm::TargetLibraryInfo &TLI,
    FuncTy &&Func, UnknownFuncTy &&UnknownFunc) {
  for (auto &I : BB)
    for_each_memory(I, TLI, std::forward<FuncTy>(Func),
      std::forward<UnknownFuncTy>(UnknownFunc));
}
}
#endif//TSAR_ESTIMATE_MEMORY_UTILS_H
