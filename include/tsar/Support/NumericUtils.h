//===- NumericUtils.h - Utilities To Use Numeric Constants ------*- C++ -*-===//
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
// This file implements utility methods to work with different representations
// of numeric constant.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_NUMERIC_UTILS
#define TSAR_NUMERIC_UTILS

#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/LLVMContext.h>
#include <limits>

namespace tsar {
/// Returns true if cast from a specified value `Const` to a specified
/// type `IntT` leads to integer overflow or a source value is signed and
/// a target value is unsigned.
template<class IntT>
inline bool isOverflow(const llvm::APInt &Const, bool IsSigned) {
  return
    std::numeric_limits<IntT>::is_signed ?
      IsSigned ?
        !Const.isSignedIntN(sizeof(IntT) * CHAR_BIT - 1) :
        !Const.isIntN(sizeof(IntT) * CHAR_BIT - 1) :
      IsSigned ?
        true : // it is not possible to represent signed as unsigned
        !Const.isIntN(sizeof(IntT) * CHAR_BIT);
}

/// Cast a specified value `Const` to a specified type `IntT`, returns `true`
/// on success and `false` if integer overflow has been occurred.
template<class IntT>
inline bool castAPInt(const llvm::APInt &Const, bool IsSigned, IntT &Out) {
  if (isOverflow<IntT>(Const, IsSigned))
    return false;
  if (IsSigned)
    Out = static_cast<IntT>(Const.getSExtValue());
  else
    Out = static_cast<IntT>(Const.getZExtValue());
  return true;
}

/// Cast a specified value `Expr` to a specified type `IntT`,
/// returns `false` if a value not a constant or integer overflow occurs.
template<class IntT>
inline bool castSCEV(const llvm::SCEV *Expr, bool IsSigned,  IntT &Out) {
  assert(Expr && "Expression must not be null!");
  if (auto Const = llvm::dyn_cast<llvm::SCEVConstant>(Expr))
    return castAPInt(Const->getAPInt(), IsSigned, Out);
  return false;
}
}
#endif//TSAR_NUMERIC_UTILS
