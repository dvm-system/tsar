//===--- SCEVUtils.cpp --------- SCEV Utils ---------------------*- C++ -*-===//
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
// This file implements functions to evaluate SCEVs.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/SCEVUtils.h"
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Support/Debug.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "scev"

using namespace llvm;
using namespace tsar;

namespace {
static inline int sizeOfSCEV(const SCEV *S) {
  struct FindSCEVSize {
    int Size;
    FindSCEVSize() : Size(0) {}

    bool follow(const SCEV *S) {
      ++Size;
      // Keep looking at all operands of S.
      return true;
    }
    bool isDone() const {
      return false;
    }
  };

  FindSCEVSize F;
  SCEVTraversal<FindSCEVSize> ST(F);
  ST.visitAll(S);
  return F.Size;
}

/// This class extends a similar class from ScalarEvolution.cpp which is
/// defined in anonymous namespace and can not be used here.
struct SCEVDivision : public SCEVVisitor<SCEVDivision, void> {
  // Except in the trivial case described above, we do not know how to divide
  // Expr by Denominator for the following functions with empty implementation.
  void visitPtrToIntExpr(const SCEVPtrToIntExpr *Numerator) {}
  void visitSequentialUMinExpr(const SCEVSequentialUMinExpr *NUmberator) {}
  void visitUDivExpr(const SCEVUDivExpr *Numerator) {}
  void visitSMaxExpr(const SCEVSMaxExpr *Numerator) {}
  void visitUMaxExpr(const SCEVUMaxExpr *Numerator) {}
  void visitSMinExpr(const SCEVSMinExpr *Numerator) {}
  void visitUMinExpr(const SCEVUMinExpr *Numerator) {}
  void visitUnknown(const SCEVUnknown *Numerator) {}
  void visitCouldNotCompute(const SCEVCouldNotCompute *Numerator) {}

  void visitTruncateExpr(const SCEVTruncateExpr *Numerator) {
    if (IsSafeTypeCast)
      return;
    auto Tmp = divide(SE, Numerator->getOperand(), Denominator, IsSafeTypeCast);
    if (!isCannotDivide(Numerator->getOperand(), Tmp)) {
      Res.Quotient = SE.getTruncateOrNoop(Tmp.Quotient, Numerator->getType());
      Res.Remainder = SE.getTruncateOrNoop(Tmp.Remainder, Numerator->getType());
      Res.IsSafeTypeCast = false;
    }
  }

  void visitZeroExtendExpr(const SCEVZeroExtendExpr *Numerator) {
    if (IsSafeTypeCast)
      return;
    auto Tmp = divide(SE, Numerator->getOperand(), Denominator, IsSafeTypeCast);
    if (!isCannotDivide(Numerator->getOperand(), Tmp)) {
      auto NumeratorBW = SE.getTypeSizeInBits(Numerator->getType());
      Res.Quotient =
        SE.getTypeSizeInBits(Tmp.Quotient->getType()) < NumeratorBW ?
        SE.getZeroExtendExpr(Tmp.Quotient, Numerator->getType()) :
        Tmp.Quotient;
      Res.Remainder =
        SE.getTypeSizeInBits(Tmp.Remainder->getType()) < NumeratorBW ?
        SE.getZeroExtendExpr(Tmp.Remainder, Numerator->getType()) :
        Tmp.Remainder;
      Res.IsSafeTypeCast = false;
    }
  }

  void visitSignExtendExpr(const SCEVSignExtendExpr *Numerator) {
    if (IsSafeTypeCast)
      return;
    auto Tmp = divide(SE, Numerator->getOperand(), Denominator, IsSafeTypeCast);
    if (!isCannotDivide(Numerator->getOperand(), Tmp)) {
      auto NumeratorBW = SE.getTypeSizeInBits(Numerator->getType());
      Res.Quotient =
        SE.getTypeSizeInBits(Tmp.Quotient->getType()) < NumeratorBW ?
        SE.getSignExtendExpr(Tmp.Quotient, Numerator->getType()) :
        Tmp.Quotient;
      Res.Remainder =
        SE.getTypeSizeInBits(Tmp.Remainder->getType()) < NumeratorBW ?
        SE.getSignExtendExpr(Tmp.Remainder, Numerator->getType()) :
        Tmp.Remainder;
      Res.IsSafeTypeCast = false;
    }
  }

  void visitConstant(const SCEVConstant *Numerator) {
    if (const SCEVConstant *D = dyn_cast<SCEVConstant>(Denominator)) {
      APInt NumeratorVal = Numerator->getAPInt();
      APInt DenominatorVal = D->getAPInt();
      auto NumeratorBW = NumeratorVal.getBitWidth();
      auto DenominatorBW = DenominatorVal.getBitWidth();

      if (NumeratorBW > DenominatorBW)
        DenominatorVal = DenominatorVal.sext(NumeratorBW);
      else if (NumeratorBW < DenominatorBW)
        NumeratorVal = NumeratorVal.sext(DenominatorBW);

      APInt QuotientVal(NumeratorVal.getBitWidth(), 0);
      APInt RemainderVal(NumeratorVal.getBitWidth(), 0);
      APInt::sdivrem(NumeratorVal, DenominatorVal, QuotientVal, RemainderVal);
      Res.Quotient = SE.getConstant(QuotientVal);
      Res.Remainder = SE.getConstant(RemainderVal);
    }
  }

  void visitAddRecExpr(const SCEVAddRecExpr *Numerator) {
    const SCEV *StartQ, *StartR, *StepQ, *StepR;
    if (!Numerator->isAffine())
      return;
    auto StartRes =
      divide(SE, Numerator->getStart(), Denominator, IsSafeTypeCast);
    auto StepRes =
      divide(SE, Numerator->getStepRecurrence(SE), Denominator, IsSafeTypeCast);
    // Bail out if the types do not match.
    if (StartRes.Quotient->getType() != StepRes.Quotient->getType() ||
        StartRes.Remainder->getType() != StepRes.Remainder->getType())
      return;
    if (IsSafeTypeCast) {
      Type *Ty = Denominator->getType();
      if (Ty != StartRes.Quotient->getType() ||
          Ty != StartRes.Remainder->getType())
        return;
    }
    Res.Quotient = SE.getAddRecExpr(StartRes.Quotient, StepRes.Quotient,
      Numerator->getLoop(), Numerator->getNoWrapFlags());
    Res.Remainder = SE.getAddRecExpr(StartRes.Remainder, StepRes.Remainder,
      Numerator->getLoop(), Numerator->getNoWrapFlags());
    Res.IsSafeTypeCast &= StartRes.IsSafeTypeCast & StepRes.IsSafeTypeCast;
  }

  void visitAddExpr(const SCEVAddExpr *Numerator) {
    SmallVector<const SCEV *, 2> Qs, Rs;
    bool IsSafeTC = true;
    auto divideOp = [this, &IsSafeTC, &Qs, &Rs](const SCEV *Op) {
      auto Tmp = divide(SE, Op, Denominator, IsSafeTypeCast);
      IsSafeTC &= Tmp.IsSafeTypeCast;
      Type *Ty = Denominator->getType();
      if (Ty != Tmp.Quotient->getType() || Ty != Tmp.Remainder->getType()) {
        // Bail out if types do not match.
        if (IsSafeTypeCast)
          return false;
        else
          IsSafeTC = false;
      }
      Qs.push_back(Tmp.Quotient);
      Rs.push_back(Tmp.Remainder);
      return true;
    };
    auto OpI = Numerator->op_begin(), OpEI = Numerator->op_end();
    if (!divideOp(*OpI))
      return;
    auto QTy = Qs.front()->getType();
    auto RTy = Rs.front()->getType();
    for (const SCEV *Op : make_range(++OpI, OpEI)) {
      if (!divideOp(Op))
        return;
      // Bail out if types do not match.
      if (QTy != Qs.back()->getType() || RTy != Rs.back()->getType())
        return;
    }
    Res.IsSafeTypeCast &= IsSafeTC;
    if (Qs.size() == 1) {
      Res.Quotient = Qs[0];
      Res.Remainder = Rs[0];
      return;
    }
    Res.Quotient = SE.getAddExpr(Qs);
    Res.Remainder = SE.getAddExpr(Rs);
  }

  void visitMulExpr(const SCEVMulExpr *Numerator) {
    SmallVector<const SCEV *, 2> Qs;
    bool IsSafeTC = true;
    bool FoundDenominatorTerm = false;
    auto divideOp = [this, &FoundDenominatorTerm, &IsSafeTC, &Qs](
        const SCEV *Op) {
      auto *Ty = Denominator->getType();
      if (Ty != Op->getType()) {
        // Bail out if types do not match.
        if (IsSafeTypeCast)
          return false;
        else
          IsSafeTC = false;
      }
      if (FoundDenominatorTerm) {
        Qs.push_back(Op);
        return true;
      }
      // Check whether Denominator divides one of the product operands.
      auto Tmp = divide(SE, Op, Denominator, IsSafeTypeCast);
      if (!Tmp.Remainder->isZero()) {
        Qs.push_back(Op);
        return true;
      }
      if (Ty != Tmp.Quotient->getType()) {
        // Bail out if types do not match.
        if (IsSafeTypeCast)
          return false;
        else
          IsSafeTC = false;
      }
      FoundDenominatorTerm = true;
      Qs.push_back(Tmp.Quotient);
      return true;
    };
    auto OpI = Numerator->op_begin(), OpEI = Numerator->op_end();
    if (!divideOp(*OpI))
      return;
    auto QTy = Qs.front()->getType();
    for (const SCEV *Op : make_range(++OpI, OpEI)) {
      if (!divideOp(Op))
        return;
      // Bail out if types do not match.
      if (QTy != Qs.back()->getType())
        return;
    }
    if (FoundDenominatorTerm) {
      Res.IsSafeTypeCast &= IsSafeTC;
      Res.Remainder = Zero;
      if (Qs.size() == 1)
        Res.Quotient = Qs[0];
      else
        Res.Quotient = SE.getMulExpr(Qs);
      return;
    }
    if (!isa<SCEVUnknown>(Denominator))
      return;
    // The Remainder is obtained by replacing Denominator by 0 in Numerator.
    ValueToSCEVMapTy RewriteMap;
    RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] = Zero;
    Res.Remainder =
      SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap);
    if (Res.Remainder->isZero()) {
      // The Quotient is obtained by replacing Denominator by 1 in Numerator.
      RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] = One;
      Res.Quotient =
        SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap);
      return;
    }
    if (Numerator->getType() != Res.Remainder->getType())
      return;
    // Quotient is (Numerator - Remainder) divided by Denominator.
    const SCEV *Diff = SE.getMinusSCEV(Numerator, Res.Remainder);
    // This SCEV does not seem to simplify: fail the division here.
    if (sizeOfSCEV(Diff) > sizeOfSCEV(Numerator))
      return;
    auto Tmp = divide(SE, Diff, Denominator, IsSafeTypeCast);
    if (Tmp.Remainder != Zero)
      return;
    Res.Quotient = Tmp.Quotient;
    Res.IsSafeTypeCast = Tmp.IsSafeTypeCast;
  }

  SCEVDivision(ScalarEvolution &SE, const SCEV *Numerator,
    const SCEV *Denominator, bool IsSafeTypeCast)
    : SE(SE), Denominator(Denominator), IsSafeTypeCast(IsSafeTypeCast) {
    Zero = SE.getZero(Denominator->getType());
    One = SE.getOne(Denominator->getType());

    // We generally do not know how to divide Expr by Denominator. We
    // initialize the division to a "cannot divide" state to simplify the rest
    // of the code.
    cannotDivide(Numerator);

  }

  // Convenience function for giving up on the division. We set the quotient to
  // be equal to zero and the remainder to be equal to the numerator.
  void cannotDivide(const SCEV *Numerator) {
    Res.Quotient = Zero;
    Res.Remainder = Numerator;
    Res.IsSafeTypeCast = true;
  }

  bool isCannotDivide(const SCEV *Numerator, const SCEVDivisionResult &Result) {
    return Result.Quotient == Zero && Result.Remainder == Numerator &&
      Result.IsSafeTypeCast;
  }

  ScalarEvolution &SE;
  const SCEV *Denominator, *Zero, *One;
  bool IsSafeTypeCast;
  SCEVDivisionResult Res;
};

/// Traverse a SCEV and simplifies it to a binomial if possible. The result is
/// `Coef * Count + FreeTerm`, where `Count` is and induction variable for L.
/// `IsSafeCast` will be set to `false` if some unsafe casts are necessary for
/// simplification.
struct SCEVBionmialSearch : public SCEVVisitor<SCEVBionmialSearch, void> {
  ScalarEvolution *mSE = nullptr;
  const SCEV * Coef = nullptr;
  const SCEV * FreeTerm = nullptr;
  const Loop * L = nullptr;
  bool IsSafeCast = true;

  SCEVBionmialSearch(ScalarEvolution &SE) : mSE(&SE) {}

  void visitTruncateExpr(const SCEVTruncateExpr *S) {
    IsSafeCast = false;
    visit(S->getOperand());
    if (Coef)
      Coef = mSE->getTruncateExpr(Coef, S->getType());
    if (FreeTerm)
      FreeTerm = mSE->getTruncateExpr(FreeTerm, S->getType());
  }

  void visitSignExtendExpr(const SCEVSignExtendExpr *S) {
    IsSafeCast = false;
    visit(S->getOperand());
    if (Coef)
      Coef = mSE->getSignExtendExpr(Coef, S->getType());
    if (FreeTerm)
      FreeTerm = mSE->getSignExtendExpr(FreeTerm, S->getType());
  }

  void visitZeroExtendExpr(const SCEVZeroExtendExpr *S) {
    IsSafeCast = false;
    visit(S->getOperand());
    if (Coef)
      Coef = mSE->getZeroExtendExpr(Coef, S->getType());
    if (FreeTerm)
      FreeTerm = mSE->getZeroExtendExpr(FreeTerm, S->getType());
  }

  void visitAddRecExpr(const SCEVAddRecExpr *S) {
    L = S->getLoop();
    Coef = S->getStepRecurrence(*mSE);
    FreeTerm = S->getStart();
  }

  void visitMulExpr(const SCEVMulExpr *S) {
    assert(!L && "Loop must not be set yet!");
    auto OpI = S->op_begin(), OpEI = S->op_end();
    SmallVector<const SCEV *, 4> MulFreeTerm;
    for (; OpI != OpEI; ++OpI) {
      visit(*OpI);
      if (L)
        break;
      MulFreeTerm.push_back(*OpI);
    }
    if (L) {
      MulFreeTerm.append(++OpI, OpEI);
      auto MulCoef = MulFreeTerm;
      MulFreeTerm.push_back(FreeTerm);
      // Note, that getMulExpr() may change order of SCEVs in it's parameter.
      FreeTerm = mSE->getMulExpr(MulFreeTerm);
      MulCoef.push_back(Coef);
      Coef = mSE->getMulExpr(MulCoef);
    } else {
      FreeTerm = S;
    }
  }

  void visitAddExpr(const SCEVAddExpr *S) {
    assert(!L && "Loop must not be set yet!");
    auto OpI = S->op_begin(), OpEI = S->op_end();
    SmallVector<const SCEV *, 4> Terms;
    for (; OpI != OpEI; ++OpI) {
      visit(*OpI);
      if (L)
        break;
      Terms.push_back(*OpI);
    }
    if (L) {
      Terms.append(++OpI, OpEI);
      Terms.push_back(FreeTerm);
      FreeTerm = mSE->getAddExpr(Terms);
    } else {
      FreeTerm = S;
    }
  }

  void visitPtrToIntExpr(const SCEVPtrToIntExpr *S) { FreeTerm = S; }
  void visitSequentialUMinExpr(const SCEVSequentialUMinExpr *S) {
    FreeTerm = S;
  }
  void visitConstant(const SCEVConstant *S) { FreeTerm = S; }
  void visitUDivExpr(const SCEVUDivExpr *S) { FreeTerm = S; }
  void visitSMaxExpr(const SCEVSMaxExpr *S) { FreeTerm = S; }
  void visitUMaxExpr(const SCEVUMaxExpr *S) { FreeTerm = S; }
  void visitSMinExpr(const SCEVSMinExpr *S) { FreeTerm = S; }
  void visitUMinExpr(const SCEVUMinExpr *S) { FreeTerm = S; }
  void visitUnknown(const SCEVUnknown *S) { FreeTerm = S; }
  void visitCouldNotCompute(const SCEVCouldNotCompute *S) { FreeTerm = S; }
};

/// Strip all type casts if unsafe type cast is allowed.
inline const SCEV * stripCastIfNot(const SCEV *S, bool IsSafeTypeCast) {
  assert(S && "SCEV must not be null!");
  if (!IsSafeTypeCast)
    while (auto *Cast = dyn_cast<SCEVCastExpr>(S))
      S = Cast->getOperand();
  return S;
}
}

namespace tsar {
SCEVDivisionResult divide(ScalarEvolution &SE, const SCEV *Numerator,
    const SCEV *Denominator, bool IsSafeTypeCast) {
  assert(Numerator && Denominator && "Uninitialized SCEV");
  SCEVDivision D(SE, Numerator, Denominator, IsSafeTypeCast);
  // Check for the trivial case here to avoid having to check for it in the
  // rest of the code.
  if (Numerator == Denominator)
    return { D.One, D.Zero, true };
  if (Numerator->isZero())
    return { D.Zero, D.Zero, true };
  // A simple case when N/1. The quotient is N.
  if (Denominator->isOne())
    return { Numerator, D.Zero, true };
  // Strip of dominator casts if unsafe type cast is possible.
  bool IsSafeDominatorCast = true;
  if (!IsSafeTypeCast && isa<SCEVCastExpr>(Denominator)) {
    IsSafeDominatorCast = true;
    while (auto Cast = dyn_cast<SCEVCastExpr>(Denominator))
      Denominator = Cast->getOperand();
  }
  // Split the Denominator when it is a product.
  SCEVDivisionResult Res;
  if (const SCEVMulExpr *T = dyn_cast<SCEVMulExpr>(Denominator)) {
    Res.Quotient = Numerator;
    Res.IsSafeTypeCast = IsSafeDominatorCast;
    for (const SCEV *Op : T->operands()) {
      auto Tmp = divide(SE, Res.Quotient, Op, IsSafeTypeCast);
      // Bail out when the Numerator is not divisible by one of the terms of
      // the Denominator.
      if (!Tmp.Remainder->isZero())
        return { D.Zero, Numerator, true };
      Res.Quotient = Tmp.Quotient;
      Res.IsSafeTypeCast &= Tmp.IsSafeTypeCast;
    }
    Res.Remainder = D.Zero;
    return Res;
  }
  D.visit(Numerator);
  D.Res.IsSafeTypeCast &= IsSafeDominatorCast;
  return D.Res;
}

std::pair<const SCEV *, bool> computeSCEVAddRec(
    const SCEV *Expr, llvm::ScalarEvolution &SE) {
  SCEVBionmialSearch Search(SE);
  Search.visit(Expr);
  bool IsSafe = true;
  if (Search.L && SE.isLoopInvariant(Search.Coef, Search.L) &&
      SE.isLoopInvariant(Search.FreeTerm, Search.L)) {
    Expr = SE.getAddRecExpr(
      Search.FreeTerm, Search.Coef, Search.L, SCEV::FlagAnyWrap);
    IsSafe = Search.IsSafeCast;
  }
  return std::make_pair(Expr, IsSafe);
}

const SCEV* findGCD(ArrayRef<const SCEV *> Expressions,
    ScalarEvolution &SE, bool IsSafeTypeCast) {
  assert(!Expressions.empty() && "List of expressions must not be empty!");
  std::vector<const SCEV *> Terms;
  Terms.reserve(Expressions.size());
  for (auto *S : Expressions) {
    auto Info = computeSCEVAddRec(S, SE);
    stripCastIfNot(Info.first, IsSafeTypeCast);
    if (auto AddRec = dyn_cast<SCEVAddRecExpr>(Info.first)) {
      if (Info.second || !IsSafeTypeCast) {
        Terms.push_back(AddRec->getStart());
        Terms.push_back(AddRec->getStepRecurrence(SE));
      } else {
        Terms.push_back(S);
      }
    } else {
      Terms.push_back(S);
    }
  }
  // Remove duplicates.
  array_pod_sort(Terms.begin(), Terms.end());
  Terms.erase(std::unique(Terms.begin(), Terms.end()), Terms.end());
  Terms.erase(
    std::remove_if(Terms.begin(), Terms.end(),
      [](const SCEV *S) { return S->isZero(); }), Terms.end());
  // Put small terms first.
  llvm::sort(Terms.begin(), Terms.end(),
    [IsSafeTypeCast](const SCEV *LHS, const SCEV *RHS) {
    LHS = stripCastIfNot(LHS, IsSafeTypeCast);
    RHS = stripCastIfNot(RHS, IsSafeTypeCast);
    auto LHSSize = isa<SCEVMulExpr>(LHS) ?
      cast<SCEVMulExpr>(LHS)->getNumOperands() : 1;
    auto RHSSize = isa<SCEVMulExpr>(RHS) ?
      cast<SCEVMulExpr>(RHS)->getNumOperands() : 1;
    return LHSSize < RHSSize;
  });
  LLVM_DEBUG(
    dbgs() << "[GCD]: terms:\n";
    for (auto *T : Terms)
      dbgs() << "  " << *T << "\n");
  if (Terms.empty())
    return SE.getCouldNotCompute();
  if (Terms.size() < 2)
    return Terms.front()->isZero() ? SE.getCouldNotCompute() : Terms.front();
  SmallVector<const SCEV *, 4> Dividers;
  auto TermItr = Terms.begin(), TermItrE = Terms.end();
  if (auto *Mul =
      dyn_cast<SCEVMulExpr>(stripCastIfNot(*TermItr, IsSafeTypeCast))) {
   for (auto *Op : Mul->operands())
     Dividers.push_back(Op);
  } else {
    Dividers.push_back(*TermItr);
  }
  for (++TermItr; TermItr != TermItrE; ++TermItr) {
    SmallVector<const SCEV *, 4> NewDividers;
    auto *T = *TermItr;
    for (auto *D : Dividers) {
      auto Div = divide(SE, T, D, IsSafeTypeCast);
      if (!Div.Remainder->isZero())
        continue;
      NewDividers.push_back(D);
      T = Div.Quotient;
    }
    if (NewDividers.empty())
      return SE.getCouldNotCompute();
    if (Dividers.size() != NewDividers.size())
      std::swap(Dividers, NewDividers);
  }
  if (Dividers.size() == 1)
    return Dividers.front();
   return SE.getMulExpr(Dividers);
}

std::vector<std::size_t> countPrimeNumbers(std::size_t Bound) {
  std::vector<std::size_t> Primes;
  enum { PRIMES_CACHE_SIZE = 60 };
  static std::size_t CachedPrimes[PRIMES_CACHE_SIZE] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59,
    61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131,
    137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197,
    199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271,
    277, 281
  };
  if (Bound <= CachedPrimes[PRIMES_CACHE_SIZE - 1]) {
    for (unsigned I = 0; I < PRIMES_CACHE_SIZE; ++I) {
      if (CachedPrimes[I] <= Bound)
        Primes.push_back(CachedPrimes[I]);
      else
        break;
    }
    return Primes;
  }
  std::vector<bool> IsPrime;
  IsPrime.resize(Bound + 1);
  for (int i = 0; i <= Bound; i++)
    IsPrime[i] = false;
  IsPrime[2] = true;
  IsPrime[3] = true;
  std::size_t BoundSqrt = (std::size_t)std::sqrt(Bound);
  std::size_t X2 = 0, Y2, N;
  for (std::size_t I = 1; I <= BoundSqrt; ++I) {
    X2 += 2 * I - 1;
    Y2 = 0;
    for (std::size_t J = 1; J <= BoundSqrt; J++) {
      Y2 += 2 * J - 1;
      N = 4 * X2 + Y2;
      if ((N <= Bound) && (N % 12 == 1 || N % 12 == 5))
        IsPrime[N] = !IsPrime[N];
      N -= X2;
      if ((N <= Bound) && (N % 12 == 7))
        IsPrime[N] = !IsPrime[N];
      N -= 2 * Y2;
      if ((I > J) && (N <= Bound) && (N % 12 == 11))
        IsPrime[N] = !IsPrime[N];
    }
  }
  for (std::size_t I = 5; I <= BoundSqrt; ++I) {
    if (IsPrime[I]) {
      N = I * I;
      for (std::size_t J = N; J <= Bound; J += N)
        IsPrime[J] = false;
    }
  }
  Primes.push_back(2);
  Primes.push_back(3);
  Primes.push_back(5);
  for (std::size_t I = 6; I <= Bound; I++)
    if ((IsPrime[I]) && (I % 3) && (I % 5))
      Primes.push_back(I);
  return Primes;
}

}
