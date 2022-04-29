//===- MemoryAccessUtils.h - Utils For Exploring Memory Accesses -- C++ -*-===//
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
// This file defines useful functions to explore accesses to memory locations
// from instructions, basic blocks and functions.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_ESTIMATE_MEMORY_UTILS_H
#define TSAR_ESTIMATE_MEMORY_UTILS_H

#include "tsar/Analysis/KnownFunctionTraits.h"
#include <bcl/trait.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
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
  using llvm::CallBase;
  using llvm::cast;
  using llvm::Function;
  using llvm::Instruction;
  using llvm::IntrinsicInst;
  using llvm::MemoryLocation;
  using llvm::StoreInst;
  auto *F = I.getFunction();
  auto isValidPtr = [F](const llvm::Value *Ptr) {
    if (llvm::isa<llvm::UndefValue>(Ptr))
      return false;
    if (const auto *CPN = llvm::dyn_cast<llvm::ConstantPointerNull>(Ptr))
      if (!llvm::NullPointerIsDefined(F, CPN->getType()->getAddressSpace()))
        return false;
    return true;
  };
  auto traverseActualParams =
      [&TLI, &Func, &UnknownFunc, &isValidPtr](CallBase *Call) {
    auto Callee =
      llvm::dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
    llvm::LibFunc LibId;
    bool IsMarker{false};
    if (auto II = llvm::dyn_cast<IntrinsicInst>(Call)) {
      IsMarker = isMemoryMarkerIntrinsic(II->getIntrinsicID());
      foreachIntrinsicMemArg(*II,
          [IsMarker, Call, &TLI, &Func, &isValidPtr](unsigned Idx) {
        auto Loc = MemoryLocation::getForArgument(Call, Idx, TLI);
        if (!isValidPtr(Loc.Ptr))
          return;
        Func(*Call, std::move(Loc), Idx,
          (Call->doesNotAccessMemory() || IsMarker)
             ? AccessInfo::No : AccessInfo::May,
          (Call->onlyReadsMemory() || IsMarker)
             ? AccessInfo::No : AccessInfo::May);
      });
    } else if (Callee && TLI.getLibFunc(*Callee, LibId)) {
      foreachLibFuncMemArg(LibId,
          [Call, &TLI, &Func, &isValidPtr](unsigned Idx) {
        auto Loc = MemoryLocation::getForArgument(Call, Idx, TLI);
        if (!isValidPtr(Loc.Ptr))
          return;
        Func(*Call, std::move(Loc), Idx,
          Call->doesNotAccessMemory() ? AccessInfo::No : AccessInfo::May,
          Call->onlyReadsMemory() ? AccessInfo::No : AccessInfo::May);
      });
    } else {
      for (unsigned Idx = 0; Idx < Call->arg_size(); ++Idx) {
        assert(Call->getArgOperand(Idx)->getType() &&
          "All actual parameters must be typed!");
        if (!Call->getArgOperand(Idx)->getType()->isPointerTy())
          continue;
        auto Loc = MemoryLocation::getForArgument(Call, Idx, TLI);
        if (!isValidPtr(Loc.Ptr))
          continue;
        Func(*Call, std::move(Loc), Idx,
         Call->doesNotAccessMemory() ? AccessInfo::No : AccessInfo::May,
         Call->onlyReadsMemory() ? AccessInfo::No : AccessInfo::May);
      }
    }
    if (!IsMarker && !Call->onlyAccessesArgMemory() &&
        Call->mayReadOrWriteMemory())
      UnknownFunc(*Call,
                  Call->doesNotAccessMemory() ? AccessInfo::No : AccessInfo::May,
                  Call->onlyReadsMemory() ? AccessInfo::No : AccessInfo::May);
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
  {
    auto Loc = MemoryLocation::get(&I);
    assert(Loc.Ptr == I.getOperand(0) &&
      "Operand with a specified number must be a specified memory location!");
    if (!isValidPtr(Loc.Ptr))
      return;
    Func(I, std::move(Loc), 0,
      I.mayReadFromMemory() ? AccessInfo::Must : AccessInfo::No,
      I.mayWriteToMemory() ? AccessInfo::Must : AccessInfo::No);
    break;
  }
  case Instruction::Store:
  {
    auto Loc = MemoryLocation::get(&I);
    assert(Loc.Ptr == I.getOperand(1) &&
      "Operand with a specified number must be a specified memory location!");
    if (!isValidPtr(Loc.Ptr))
      return;
    Func(I, MemoryLocation::get(cast<StoreInst>(&I)), 1,
      AccessInfo::No, AccessInfo::Must);
    break;
  }
  case Instruction::Call: case Instruction::Invoke:
    traverseActualParams(cast<CallBase>(&I)); break;
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
