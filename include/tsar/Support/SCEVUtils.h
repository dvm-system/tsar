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

#include <llvm/ADT/ArrayRef.h>
#include <vector>

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

/// Simplify a specified expression and transform it to AddRec expression if
/// possible.
///
/// This function uses unsafe type cast and may drop some truncate and extend
/// operations or chnage order of theses operations. In this case the second
/// returned value is `false`.
std::pair<const llvm::SCEV *, bool> computeSCEVAddRec(
  const llvm::SCEV *Expr, llvm::ScalarEvolution &SE);

/// Find GCD for specified expressions.
///
/// This function do not perform any simplification of expressions, except
/// the following ones.
/// (1) This splits AddRec expressions: A*I + B into two expressions A and B.
/// (2) Each factor of a Mul expression is evaluated separately and may be a GCD.
/// TODO (kaniandr@gmail.com): may be it is useful to perform factorization of
/// constants? However, calculation of prime numbers may degrade performance.
/// Note, that is seems that we also can not safely use Euclid algorithm.
const llvm::SCEV* findGCD(llvm::ArrayRef<const llvm::SCEV *> Expressions,
  llvm::ScalarEvolution &SE, bool IsSafeTypeCast = true);

/// Returns list of primes which is less or equal than a specified bound.
///
/// This function implements Sieve of Atkin with cache.
std::vector<std::size_t> countPrimeNumbers(std::size_t Bound);
}
#endif//TSAR_SCEV_UTILS_H
