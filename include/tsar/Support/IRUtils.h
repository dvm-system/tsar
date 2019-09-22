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
#include <llvm/IR/Type.h>

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
}

#endif//TSAR_SUPPORT_IR_UTILS_H
