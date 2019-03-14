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
  void visitUDivExpr(const SCEVUDivExpr *Numerator) {}
  void visitSMaxExpr(const SCEVSMaxExpr *Numerator) {}
  void visitUMaxExpr(const SCEVUMaxExpr *Numerator) {}
  void visitUnknown(const SCEVUnknown *Numerator) {}
  void visitCouldNotCompute(const SCEVCouldNotCompute *Numerator) {}

  void visitTruncateExpr(const SCEVTruncateExpr *Numerator) {
    if (IsSafeTypeCast)
      return;
    auto Tmp = divide(SE, Numerator->getOperand(), Denominator, IsSafeTypeCast);
    if (!isCannotDivide(Numerator->getOperand(), Tmp)) {
      Res.Quotient = SE.getTruncateExpr(Tmp.Quotient, Numerator->getType());
      Res.Remainder = SE.getTruncateExpr(Tmp.Remainder, Numerator->getType());
      Res.IsSafeTypeCast = false;
    }
  }

  void visitZeroExtendExpr(const SCEVZeroExtendExpr *Numerator) {
    if (IsSafeTypeCast)
      return;
    auto Tmp = divide(SE, Numerator->getOperand(), Denominator, IsSafeTypeCast);
    if (!isCannotDivide(Numerator->getOperand(), Tmp)) {
      Res.Quotient = SE.getZeroExtendExpr(Tmp.Quotient, Numerator->getType());
      Res.Remainder = SE.getZeroExtendExpr(Tmp.Remainder, Numerator->getType());
      Res.IsSafeTypeCast = false;
    }
  }

  void visitSignExtendExpr(const SCEVSignExtendExpr *Numerator) {
    if (IsSafeTypeCast)
      return;
    auto Tmp = divide(SE, Numerator->getOperand(), Denominator, IsSafeTypeCast);
    if (!isCannotDivide(Numerator->getOperand(), Tmp)) {
      Res.Quotient = SE.getSignExtendExpr(Tmp.Quotient, Numerator->getType());
      Res.Remainder = SE.getSignExtendExpr(Tmp.Remainder, Numerator->getType());
      Res.IsSafeTypeCast = false;
    }
  }

  void visitConstant(const SCEVConstant *Numerator) {
    if (const SCEVConstant *D = dyn_cast<SCEVConstant>(Denominator)) {
      APInt NumeratorVal = Numerator->getAPInt();
      APInt DenominatorVal = D->getAPInt();
      uint32_t NumeratorBW = NumeratorVal.getBitWidth();
      uint32_t DenominatorBW = DenominatorVal.getBitWidth();

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
    ValueToValueMap RewriteMap;
    RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] =
      cast<SCEVConstant>(Zero)->getValue();
    Res.Remainder =
      SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap, true);
    if (Res.Remainder->isZero()) {
      // The Quotient is obtained by replacing Denominator by 1 in Numerator.
      RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] =
        cast<SCEVConstant>(One)->getValue();
      Res.Quotient =
        SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap, true);
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
}

