//===----- IRUtils.h ---- Utils for exploring LLVM IR -----------*- C++ -*-===//
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
// This file defines helpful functions to access IR-level information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SUPPORT_IR_UTILS_H
#define TSAR_SUPPORT_IR_UTILS_H

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>
#include <llvm/ADT/SmallVector.h>

namespace tsar {
/// Returns argument with a specified number or nullptr.
llvm::Argument * getArgument(llvm::Function &F, std::size_t ArgNo);

/// Returns number of dimensions in a specified type or 0 if it is not an array.
inline unsigned dimensionsNum(const llvm::Type *Ty) {
  unsigned Dims = 0;
  for (; Ty->isArrayTy(); Ty = Ty->getArrayElementType(), ++Dims);
  return Dims;
}

/// Returns number of dimensions and elements in a specified type and type of
/// innermost array element. If `Ty` is not an array type this function returns
/// 0,1, Ty.
inline std::tuple<unsigned, uint64_t, llvm::Type *>
arraySize(llvm::Type *Ty) {
  assert(Ty && "Type must not be null!");
  unsigned Dims = 0;
  uint64_t NumElements = 1;
  for (; Ty->isArrayTy(); Ty = Ty->getArrayElementType(), ++Dims)
    NumElements *= llvm::cast<llvm::ArrayType>(Ty)->getArrayNumElements();
  return std::make_tuple(Dims, NumElements, Ty);
}

/// Return true if a specified type is a pointer type or contains sub-types
/// which are pointer types.
inline bool hasUnderlyingPointer(llvm::Type *Ty) {
  assert(Ty && "Type must not be null!");
  if (Ty->isPointerTy())
    return true;
  if (Ty->isArrayTy())
    return hasUnderlyingPointer(Ty->getArrayElementType());
  if (Ty->isVectorTy())
    return hasUnderlyingPointer(Ty->getScalarType());
  if (Ty->isStructTy())
    for (unsigned I = 0, EI = Ty->getStructNumElements(); I < EI; ++I)
      return hasUnderlyingPointer(Ty->getStructElementType(I));
  return false;
}

/// Try to reveal type of an opaque pointer.
llvm::Type *getPointerElementType(const llvm::Value &V);

/// Return true if a specified value points to the memory which is only
/// available inside a specific loop.
bool pointsToLocalMemory(const llvm::Value &V, const llvm::Loop &L);

namespace detail {
/// Applies a specified function object to each loop in a loop tree.
template<class Function>
void for_each_loop(llvm::LoopInfo::reverse_iterator ReverseI,
  llvm::LoopInfo::reverse_iterator ReverseEI,
  Function F) {
  for (; ReverseI != ReverseEI; ++ReverseI) {
    F(*ReverseI);
    for_each_loop((*ReverseI)->rbegin(), (*ReverseI)->rend(), F);
  }
}
}

/// Applies a specified function object to each loop in a loop tree.
template<class Function>
Function for_each_loop(const llvm::LoopInfo &LI, Function F) {
  detail::for_each_loop(LI.rbegin(), LI.rend(), F);
  return std::move(F);
}

/// Check if predicate is `true` for at least one instruction which uses
/// result of a specified value.
///
/// This function tries to find instruction which covers each user of a
/// specified value (a user may be a constant expression for example).
/// If corresponding instruction is not found then a user is passed to a
/// functor.
template<class FunctionT>
bool any_of_user_insts(llvm::Value &I, FunctionT F) {
  for (auto *U : I.users()) {
    if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
      if (F(UI))
        return true;
    } else if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(U)) {
      llvm::SmallVector<llvm::ConstantExpr *, 4> WorkList{CE};
      do {
        auto *Expr = WorkList.pop_back_val();
        for (auto *ExprU : Expr->users()) {
          if (auto ExprUseInst = llvm::dyn_cast<llvm::Instruction>(ExprU)) {
            if (F(ExprUseInst))
              return true;
          } else if (auto Expr = llvm::dyn_cast<llvm::ConstantExpr>(ExprU)) {
            WorkList.push_back(Expr);
          } else if (F(Expr)) {
            return true;
          }
        }
      } while (!WorkList.empty());
    } else if (F(U)) {
      return true;
    }
  }
  return false;
}

/// Apply a specified function to each instruction which uses a specified value.
///
/// This function tries to find instruction which covers each user of a
/// specified value (a user may be a constant expression for example).
/// If corresponding instruction is not found then a user is passed to a
/// functor.
template<class FunctionT>
void for_each_user_insts(llvm::Value &V, FunctionT F) {
  for (auto *U : V.users()) {
    if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
      F(UI);
    } else if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(U)) {
      llvm::SmallVector<llvm::ConstantExpr *, 4> WorkList{ CE };
      do {
        auto *Expr = WorkList.pop_back_val();
        for (auto *ExprU : Expr->users()) {
          if (auto ExprUseInst = llvm::dyn_cast<llvm::Instruction>(ExprU))
            F(ExprUseInst);
          else if (auto ExprUseExpr = llvm::dyn_cast<llvm::ConstantExpr>(ExprU))
            WorkList.push_back(ExprUseExpr);
          else
            F(Expr);
        }
      } while (!WorkList.empty());
    } else
      F(U);
  }
}

/// Return a block inside the loop that have successors outside of the loop if
/// there is no other blocks inside the loop that have reachable successors
/// outside of the loop.
inline llvm::BasicBlock* getValidExitingBlock(const llvm::Loop& L) {
  llvm::SmallVector<llvm::BasicBlock *, 4> ExitingBlocks;
  L.getExitingBlocks(ExitingBlocks);
  if (ExitingBlocks.size() == 1)
    return ExitingBlocks.front();
  unsigned ValidCount = 0;
  llvm::BasicBlock *ValidBB{nullptr};
  for (auto *BB : ExitingBlocks) {
    if (any_of(successors(BB), [&L](llvm::BasicBlock *SuccBB) {
          return !L.contains(SuccBB) &&
                 !llvm::isa<llvm::UnreachableInst>(SuccBB->front());
        })) {
      ValidBB = BB;
      ++ValidCount;
    }
  }
  if (ValidCount == 1)
    return ValidBB;
  return nullptr;
}
}

#endif//TSAR_SUPPORT_IR_UTILS_H
