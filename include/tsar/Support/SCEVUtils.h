//===--- SCEVUtils.h ----------- SCEV Utils ---------------------*- C++ -*-===//
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
// This file declares functions to evaluate SCEVs.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SCEV_UTILS_H
#define TSAR_SCEV_UTILS_H

namespace llvm {
class ScalarEvolution;
class SCEV;
}

namespace tsar {
struct SCEVDivisionResult {
  const llvm::SCEV *Quotient;
  const llvm::SCEV *Remainder;
  bool IsSafeTypeCast;
};

/// Computes the Quotient and Remainder of the division of Numerator by
/// Denominator. If IsSafeTypeCast is `false` then type casts in SCEVs are
/// ignored and divison may be incorrect.
///
/// If some type casts have been ignored `IsSafeTypeCast` member of return
/// value is set to `false`.
SCEVDivisionResult divide(llvm::ScalarEvolution &SE, const llvm::SCEV *Numerator,
  const llvm::SCEV *Denominator, bool IsSafeTypeCast = true);
}

#endif//TSAR_SCEV_UTILS_H
