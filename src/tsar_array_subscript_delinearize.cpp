#include "tsar_array_subscript_delinearize.h"
#include "DelinearizeJSON.h"
#include "KnownFunctionTraits.h"
#include "MemoryAccessUtils.h"
#include "tsar_query.h"
#include "tsar_utility.h"
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils/Local.h>
#include <bcl/Json.h>
#include <utility>
#include <cmath>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "delinearize"

char DelinearizationPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(DelinearizationPass, "delinearize",
  "Array Access Delinearizer", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(DelinearizationPass, "delinearize",
  "Array Access Delinearizer", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

STATISTIC(NumDelinearizedSubscripts, "Number of delinearized subscripts");

static cl::opt<unsigned> MaxSCEVCompareDepth(
  "scalar-evolution-max-scev-compare-depth-local-copy", cl::Hidden,
  cl::desc("Maximum depth of recursive SCEV complexity comparisons"),
  cl::init(32));

static cl::opt<unsigned> MaxValueCompareDepth(
  "scalar-evolution-max-value-compare-depth-local-copy", cl::Hidden,
  cl::desc("Maximum depth of recursive value complexity comparisons"),
  cl::init(2));

namespace {
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
    SmallVector<const SCEV *, 4> MulCoef;
    for (; OpI != OpEI; ++OpI) {
      visit(*OpI);
      if (L)
        break;
      MulCoef.push_back(*OpI);
    }
    if (L) {
      MulCoef.append(++OpI, OpEI);
      MulCoef.push_back(FreeTerm);
      FreeTerm = mSE->getMulExpr(MulCoef);
      MulCoef.back() = Coef;
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

  void visitConstant(const SCEVConstant *S) { FreeTerm = S; }
  void visitUDivExpr(const SCEVUDivExpr *S) { FreeTerm = S; }
  void visitSMaxExpr(const SCEVSMaxExpr *S) { FreeTerm = S; }
  void visitUMaxExpr(const SCEVUMaxExpr *S) { FreeTerm = S; }
  void visitUnknown(const SCEVUnknown *S) { FreeTerm = S; }
  void visitCouldNotCompute(const SCEVCouldNotCompute *S) { FreeTerm = S; }
};
}

std::pair<const SCEV *, bool> tsar::computeSCEVAddRec(
    const SCEV *Expr, llvm::ScalarEvolution &SE) {
  SCEVBionmialSearch Search(SE);
  Search.visit(Expr);
  bool IsSafe = true;
  if (Search.L) {
    Expr = SE.getAddRecExpr(
      Search.FreeTerm, Search.Coef, Search.L, SCEV::FlagAnyWrap);
    IsSafe = Search.IsSafeCast;
  }
  return std::make_pair(Expr, IsSafe);
}

std::pair<const Array *, const Array::Element *>
DelinearizeInfo::findElement(const Value *ElementPtr) const {
  auto Itr = mElements.find(ElementPtr);
  if (Itr != mElements.end()) {
    auto *TargetArray = Itr->getArray();
    auto *TargetElement = TargetArray->getElement(Itr->getElementIdx());
    return std::make_pair(TargetArray, TargetElement);
  }
  return std::make_pair(nullptr, nullptr);
}

void DelinearizeInfo::fillElementsMap() {
  mElements.clear();
  for (auto &ArrayEntry : mArrays) {
    int Idx = 0;
    for (auto Itr = ArrayEntry->begin(), ItrE = ArrayEntry->end();
         Itr != ItrE; ++Itr, ++Idx)
      mElements.try_emplace(Itr->Ptr, ArrayEntry, Idx);
  }
}

namespace {
static int
CompareValueComplexity(SmallSet<std::pair<Value *, Value *>, 8> &EqCache,
  const LoopInfo *const LI, Value *LV, Value *RV,
  unsigned Depth) {
  if (Depth > MaxValueCompareDepth || EqCache.count({ LV, RV }))
    return 0;

  // Order pointer values after integer values. This helps SCEVExpander form
  // GEPs.
  bool LIsPointer = LV->getType()->isPointerTy(),
    RIsPointer = RV->getType()->isPointerTy();
  if (LIsPointer != RIsPointer)
    return (int)LIsPointer - (int)RIsPointer;

  // Compare getValueID values.
  unsigned LID = LV->getValueID(), RID = RV->getValueID();
  if (LID != RID)
    return (int)LID - (int)RID;

  // Sort arguments by their position.
  if (const auto *LA = dyn_cast<Argument>(LV)) {
    const auto *RA = cast<Argument>(RV);
    unsigned LArgNo = LA->getArgNo(), RArgNo = RA->getArgNo();
    return (int)LArgNo - (int)RArgNo;
  }

  if (const auto *LGV = dyn_cast<GlobalValue>(LV)) {
    const auto *RGV = cast<GlobalValue>(RV);

    const auto IsGVNameSemantic = [&](const GlobalValue *GV) {
      auto LT = GV->getLinkage();
      return !(GlobalValue::isPrivateLinkage(LT) ||
        GlobalValue::isInternalLinkage(LT));
    };

    // Use the names to distinguish the two values, but only if the
    // names are semantically important.
    if (IsGVNameSemantic(LGV) && IsGVNameSemantic(RGV))
      return LGV->getName().compare(RGV->getName());
  }

  // For instructions, compare their loop depth, and their operand count.  This
  // is pretty loose.
  if (const auto *LInst = dyn_cast<Instruction>(LV)) {
    const auto *RInst = cast<Instruction>(RV);

    // Compare loop depths.
    const BasicBlock *LParent = LInst->getParent(),
      *RParent = RInst->getParent();
    if (LParent != RParent) {
      unsigned LDepth = LI->getLoopDepth(LParent),
        RDepth = LI->getLoopDepth(RParent);
      if (LDepth != RDepth)
        return (int)LDepth - (int)RDepth;
    }

    // Compare the number of operands.
    unsigned LNumOps = LInst->getNumOperands(),
      RNumOps = RInst->getNumOperands();
    if (LNumOps != RNumOps)
      return (int)LNumOps - (int)RNumOps;

    for (unsigned Idx : seq(0u, LNumOps)) {
      int Result =
        CompareValueComplexity(EqCache, LI, LInst->getOperand(Idx),
          RInst->getOperand(Idx), Depth + 1);
      if (Result != 0)
        return Result;
    }
  }

  EqCache.insert({ LV, RV });
  return 0;
}

static int CompareSCEVComplexity(
  SmallSet<std::pair<const SCEV *, const SCEV *>, 8> &EqCacheSCEV,
  const LoopInfo *const LI, const SCEV *LHS, const SCEV *RHS,
  unsigned Depth = 0) {
  // Fast-path: SCEVs are uniqued so we can do a quick equality check.
  if (LHS == RHS)
    return 0;

  // Primarily, sort the SCEVs by their getSCEVType().
  unsigned LType = LHS->getSCEVType(), RType = RHS->getSCEVType();
  if (LType != RType)
    return (int)LType - (int)RType;

  if (Depth > MaxSCEVCompareDepth || EqCacheSCEV.count({ LHS, RHS }))
    return 0;
  // Aside from the getSCEVType() ordering, the particular ordering
  // isn't very important except that it's beneficial to be consistent,
  // so that (a + b) and (b + a) don't end up as different expressions.
  switch (static_cast<SCEVTypes>(LType)) {
  case scUnknown: {
    const SCEVUnknown *LU = cast<SCEVUnknown>(LHS);
    const SCEVUnknown *RU = cast<SCEVUnknown>(RHS);

    SmallSet<std::pair<Value *, Value *>, 8> EqCache;
    int X = CompareValueComplexity(EqCache, LI, LU->getValue(), RU->getValue(),
      Depth + 1);
    if (X == 0)
      EqCacheSCEV.insert({ LHS, RHS });
    return X;
  }

  case scConstant: {
    const SCEVConstant *LC = cast<SCEVConstant>(LHS);
    const SCEVConstant *RC = cast<SCEVConstant>(RHS);

    // Compare constant values.
    const APInt &LA = LC->getAPInt();
    const APInt &RA = RC->getAPInt();
    unsigned LBitWidth = LA.getBitWidth(), RBitWidth = RA.getBitWidth();
    if (LBitWidth != RBitWidth)
      return (int)LBitWidth - (int)RBitWidth;
    return LA.ult(RA) ? -1 : 1;
  }

  case scAddRecExpr: {
    const SCEVAddRecExpr *LA = cast<SCEVAddRecExpr>(LHS);
    const SCEVAddRecExpr *RA = cast<SCEVAddRecExpr>(RHS);

    // Compare addrec loop depths.
    const Loop *LLoop = LA->getLoop(), *RLoop = RA->getLoop();
    if (LLoop != RLoop) {
      unsigned LDepth = LLoop->getLoopDepth(), RDepth = RLoop->getLoopDepth();
      if (LDepth != RDepth)
        return (int)LDepth - (int)RDepth;
    }

    // Addrec complexity grows with operand count.
    size_t LNumOps = LA->getNumOperands(), RNumOps = RA->getNumOperands();
    if (LNumOps != RNumOps)
      return (int)LNumOps - (int)RNumOps;

    // Lexicographically compare.
    for (unsigned i = 0; i != LNumOps; ++i) {
      int X = CompareSCEVComplexity(EqCacheSCEV, LI, LA->getOperand(i),
        RA->getOperand(i), Depth + 1);
      if (X != 0)
        return X;
    }
    EqCacheSCEV.insert({ LHS, RHS });
    return 0;
  }

  case scAddExpr:
  case scMulExpr:
  case scSMaxExpr:
  case scUMaxExpr: {
    const SCEVNAryExpr *LC = cast<SCEVNAryExpr>(LHS);
    const SCEVNAryExpr *RC = cast<SCEVNAryExpr>(RHS);

    // Lexicographically compare n-ary expressions.
    size_t LNumOps = LC->getNumOperands(), RNumOps = RC->getNumOperands();
    if (LNumOps != RNumOps)
      return (int)LNumOps - (int)RNumOps;

    for (unsigned i = 0; i != LNumOps; ++i) {
      if (i >= RNumOps)
        return 1;
      int X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getOperand(i),
        RC->getOperand(i), Depth + 1);
      if (X != 0)
        return X;
    }
    EqCacheSCEV.insert({ LHS, RHS });
    return 0;
  }

  case scUDivExpr: {
    const SCEVUDivExpr *LC = cast<SCEVUDivExpr>(LHS);
    const SCEVUDivExpr *RC = cast<SCEVUDivExpr>(RHS);

    // Lexicographically compare udiv expressions.
    int X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getLHS(), RC->getLHS(),
      Depth + 1);
    if (X != 0)
      return X;
    X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getRHS(), RC->getRHS(),
      Depth + 1);
    if (X == 0)
      EqCacheSCEV.insert({ LHS, RHS });
    return X;
  }

  case scTruncate:
  case scZeroExtend:
  case scSignExtend: {
    const SCEVCastExpr *LC = cast<SCEVCastExpr>(LHS);
    const SCEVCastExpr *RC = cast<SCEVCastExpr>(RHS);

    // Compare cast expressions by operand.
    int X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getOperand(),
      RC->getOperand(), Depth + 1);
    if (X == 0)
      EqCacheSCEV.insert({ LHS, RHS });
    return X;
  }

  case scCouldNotCompute:
    llvm_unreachable("Attempt to use a SCEVCouldNotCompute object!");
  }
  llvm_unreachable("Unknown SCEV kind!");
}

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

struct SCEVDivision : public SCEVVisitor<SCEVDivision, void> {
public:
  // Computes the Quotient and Remainder of the division of Numerator by
  // Denominator.
  static void divide(ScalarEvolution &SE, const SCEV *Numerator,
    const SCEV *Denominator, const SCEV **Quotient,
    const SCEV **Remainder) {
    assert(Numerator && Denominator && "Uninitialized SCEV");

    SCEVDivision D(SE, Numerator, Denominator);

    // Check for the trivial case here to avoid having to check for it in the
    // rest of the code.
    if (Numerator == Denominator) {
      *Quotient = D.One;
      *Remainder = D.Zero;
      return;
    }

    if (Numerator->isZero()) {
      *Quotient = D.Zero;
      *Remainder = D.Zero;
      return;
    }

    // A simple case when N/1. The quotient is N.
    if (Denominator->isOne()) {
      *Quotient = Numerator;
      *Remainder = D.Zero;
      return;
    }

    // Split the Denominator when it is a product.
    if (const SCEVMulExpr *T = dyn_cast<SCEVMulExpr>(Denominator)) {
      const SCEV *Q, *R;
      *Quotient = Numerator;
      for (const SCEV *Op : T->operands()) {
        divide(SE, *Quotient, Op, &Q, &R);
        *Quotient = Q;

        // Bail out when the Numerator is not divisible by one of the terms of
        // the Denominator.
        if (!R->isZero()) {
          *Quotient = D.Zero;
          *Remainder = Numerator;
          return;
        }
      }
      *Remainder = D.Zero;
      return;
    }

    D.visit(Numerator);
    *Quotient = D.Quotient;
    *Remainder = D.Remainder;
  }

  // Except in the trivial case described above, we do not know how to divide
  // Expr by Denominator for the following functions with empty implementation.
  void visitTruncateExpr(const SCEVTruncateExpr *Numerator) {
    const SCEV *CurrentNumerator, *CurrentDenominator, *Q, *R;
    if (auto *CastDenominator = dyn_cast<SCEVCastExpr>(Denominator)) {
      if (Numerator->getOperand()->getType()
        == CastDenominator->getOperand()->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = CastDenominator->getOperand();
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    } else {
      if (Numerator->getOperand()->getType() == Denominator->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = Denominator;
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    }
    if (CurrentNumerator && CurrentDenominator) {
      divide(SE, CurrentNumerator, CurrentDenominator, &Q, &R);
      Quotient = SE.getTruncateExpr(Q, Numerator->getType());
      Remainder = SE.getTruncateExpr(R, Numerator->getType());
    }
  }

  void visitZeroExtendExpr(const SCEVZeroExtendExpr *Numerator) {
    const SCEV *CurrentNumerator, *CurrentDenominator, *Q, *R;
    if (auto *CastDenominator = dyn_cast<SCEVCastExpr>(Denominator)) {
      if (Numerator->getOperand()->getType()
        == CastDenominator->getOperand()->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = CastDenominator->getOperand();
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    } else {
      if (Numerator->getOperand()->getType() == Denominator->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = Denominator;
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    }
    if (CurrentNumerator && CurrentDenominator) {
      divide(SE, CurrentNumerator, CurrentDenominator, &Q, &R);
      Quotient = SE.getZeroExtendExpr(Q, Numerator->getType());
      Remainder = SE.getZeroExtendExpr(R, Numerator->getType());
    }
  }

  void visitSignExtendExpr(const SCEVSignExtendExpr *Numerator) {
    const SCEV *CurrentNumerator, *CurrentDenominator, *Q, *R;
    if (auto *CastDenominator = dyn_cast<SCEVCastExpr>(Denominator)) {
      if (Numerator->getOperand()->getType()
        == CastDenominator->getOperand()->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = CastDenominator->getOperand();
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    } else {
      if (Numerator->getOperand()->getType() == Denominator->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = Denominator;
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    }
    if (CurrentNumerator && CurrentDenominator) {
      divide(SE, CurrentNumerator, CurrentDenominator, &Q, &R);
      Quotient = SE.getSignExtendExpr(Q, Numerator->getType());
      Remainder = SE.getSignExtendExpr(R, Numerator->getType());
    }
  }

  void visitUDivExpr(const SCEVUDivExpr *Numerator) {}
  void visitSMaxExpr(const SCEVSMaxExpr *Numerator) {}
  void visitUMaxExpr(const SCEVUMaxExpr *Numerator) {}
  void visitUnknown(const SCEVUnknown *Numerator) {}
  void visitCouldNotCompute(const SCEVCouldNotCompute *Numerator) {}

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
      Quotient = SE.getConstant(QuotientVal);
      Remainder = SE.getConstant(RemainderVal);
      return;
    }
  }

  void visitAddRecExpr(const SCEVAddRecExpr *Numerator) {
    const SCEV *StartQ, *StartR, *StepQ, *StepR;
    if (!Numerator->isAffine())
      return cannotDivide(Numerator);
    divide(SE, Numerator->getStart(), Denominator, &StartQ, &StartR);
    divide(SE, Numerator->getStepRecurrence(SE), Denominator, &StepQ, &StepR);
    // Bail out if the types do not match.
    Type *Ty = Denominator->getType();
    if (Ty != StartQ->getType() || Ty != StartR->getType() ||
      Ty != StepQ->getType() || Ty != StepR->getType())
      return cannotDivide(Numerator);
    Quotient = SE.getAddRecExpr(StartQ, StepQ, Numerator->getLoop(),
      Numerator->getNoWrapFlags());
    Remainder = SE.getAddRecExpr(StartR, StepR, Numerator->getLoop(),
      Numerator->getNoWrapFlags());
  }

  void visitAddExpr(const SCEVAddExpr *Numerator) {
    SmallVector<const SCEV *, 2> Qs, Rs;
    Type *Ty = Denominator->getType();

    for (const SCEV *Op : Numerator->operands()) {
      const SCEV *Q, *R;
      divide(SE, Op, Denominator, &Q, &R);

      // Bail out if types do not match.
      if (Ty != Q->getType() || Ty != R->getType())
        return cannotDivide(Numerator);

      Qs.push_back(Q);
      Rs.push_back(R);
    }

    if (Qs.size() == 1) {
      Quotient = Qs[0];
      Remainder = Rs[0];
      return;
    }

    Quotient = SE.getAddExpr(Qs);
    Remainder = SE.getAddExpr(Rs);
  }

  void visitMulExpr(const SCEVMulExpr *Numerator) {
    SmallVector<const SCEV *, 2> Qs;
    Type *Ty = Denominator->getType();

    bool FoundDenominatorTerm = false;
    for (const SCEV *Op : Numerator->operands()) {
      // Bail out if types do not match.
      if (Ty != Op->getType())
        return cannotDivide(Numerator);

      if (FoundDenominatorTerm) {
        Qs.push_back(Op);
        continue;
      }

      // Check whether Denominator divides one of the product operands.
      const SCEV *Q, *R;
      divide(SE, Op, Denominator, &Q, &R);
      if (!R->isZero()) {
        Qs.push_back(Op);
        continue;
      }

      // Bail out if types do not match.
      if (Ty != Q->getType())
        return cannotDivide(Numerator);

      FoundDenominatorTerm = true;
      Qs.push_back(Q);
    }

    if (FoundDenominatorTerm) {
      Remainder = Zero;
      if (Qs.size() == 1)
        Quotient = Qs[0];
      else
        Quotient = SE.getMulExpr(Qs);
      return;
    }

    if (!isa<SCEVUnknown>(Denominator))
      return cannotDivide(Numerator);

    // The Remainder is obtained by replacing Denominator by 0 in Numerator.
    ValueToValueMap RewriteMap;
    RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] =
      cast<SCEVConstant>(Zero)->getValue();
    Remainder = SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap, true);

    if (Remainder->isZero()) {
      // The Quotient is obtained by replacing Denominator by 1 in Numerator.
      RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] =
        cast<SCEVConstant>(One)->getValue();
      Quotient =
        SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap, true);
      return;
    }

    // Quotient is (Numerator - Remainder) divided by Denominator.
    const SCEV *Q, *R;
    const SCEV *Diff = SE.getMinusSCEV(Numerator, Remainder);
    // This SCEV does not seem to simplify: fail the division here.
    if (sizeOfSCEV(Diff) > sizeOfSCEV(Numerator))
      return cannotDivide(Numerator);
    divide(SE, Diff, Denominator, &Q, &R);
    if (R != Zero)
      return cannotDivide(Numerator);
    Quotient = Q;
  }

private:
  SCEVDivision(ScalarEvolution &S, const SCEV *Numerator,
    const SCEV *Denominator)
    : SE(S), Denominator(Denominator) {
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
    Quotient = Zero;
    Remainder = Numerator;
  }

  ScalarEvolution &SE;
  const SCEV *Denominator, *Quotient, *Remainder, *Zero, *One;
};

template<class GEPItrT>
void extractSubscriptsFromGEPs(
    const GEPItrT &GEPBeginItr, const GEPItrT &GEPEndItr,
    SmallVectorImpl<Value *> &Idxs) {
  for (auto *GEP : make_range(GEPBeginItr, GEPEndItr)) {
    unsigned NumOperands = GEP->getNumOperands();
    if (NumOperands == 2) {
      Idxs.push_back(GEP->getOperand(1));
    } else {
      if (auto *SecondOp = dyn_cast<Constant>(GEP->getOperand(1))) {
        if (!SecondOp->isZeroValue())
          Idxs.push_back(GEP->getOperand(1));
      } else {
        Idxs.push_back(GEP->getOperand(1));
      }
      for (unsigned I = 2; I < NumOperands; ++I) {
        Idxs.push_back(GEP->getOperand(I));
      }
    }
  }
}

void countPrimeNumbers(uint64_t Bound, std::vector<uint64_t> &Primes) {
  //Implements Sieve of Atkin with cache

  enum {
    PRIMES_CACHE_SIZE = 60
  };

  static uint64_t CachedPrimes[PRIMES_CACHE_SIZE] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59,
    61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131,
    137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197,
    199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271,
    277, 281
  };

  Primes.clear();

  if (Bound <= CachedPrimes[PRIMES_CACHE_SIZE - 1]) {
    for (int i = 0; i < PRIMES_CACHE_SIZE; i++) {
      if (CachedPrimes[i] <= Bound) {
        Primes.push_back(CachedPrimes[i]);
      } else {
        break;
      }
    }
    return;
  }

  std::vector<bool> IsPrime;
  IsPrime.resize(Bound + 1);
  for (int i = 0; i <= Bound; i++) {
    IsPrime[i] = false;
  }
  IsPrime[2] = true;
  IsPrime[3] = true;
  uint64_t BoundSqrt = (int)sqrt(Bound);

  uint64_t x2 = 0, y2, n;
  for (int i = 1; i <= BoundSqrt; i++) {
    x2 += 2 * i - 1;
    y2 = 0;
    for (int j = 1; j <= BoundSqrt; j++) {
      y2 += 2 * j - 1;
      n = 4 * x2 + y2;
      if ((n <= Bound) && (n % 12 == 1 || n % 12 == 5)) {
        IsPrime[n] = !IsPrime[n];
      }

      n -= x2;
      if ((n <= Bound) && (n % 12 == 7)) {
        IsPrime[n] = !IsPrime[n];
      }

      n -= 2 * y2;
      if ((i > j) && (n <= Bound) && (n % 12 == 11)) {
        IsPrime[n] = !IsPrime[n];
      }
    }
  }

  for (int i = 5; i <= BoundSqrt; i++) {
    if (IsPrime[i]) {
      n = i * i;
      for (uint64_t j = n; j <= Bound; j += n) {
        IsPrime[j] = false;
      }
    }
  }

  Primes.push_back(2);
  Primes.push_back(3);
  Primes.push_back(5);
  for (int i = 6; i <= Bound; i++) {
    if ((IsPrime[i]) && (i % 3) && (i % 5)) {
      Primes.push_back(i);
    }
  }
}

void countConstantMultipliers(const SCEVConstant *Const,
  ScalarEvolution &SE, SmallVectorImpl<const SCEV *> &Multipliers) {
  uint64_t ConstValue = Const->getAPInt().getLimitedValue();
  assert(ConstValue && "Constant value is zero");

  if (ConstValue < 0) {
    Multipliers.push_back(SE.getConstant(Const->getType(), -1, true));
    ConstValue *= -1;
  }
  if (ConstValue == 1) {
    Multipliers.push_back(SE.getConstant(Const->getType(), 1, false));
    return;
  }
  std::vector<uint64_t> Primes;
  countPrimeNumbers(ConstValue, Primes);
  size_t i = Primes.size() - 1;
  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Constant Multipliers:\n";
  );
  while (ConstValue > 1) {
    if (ConstValue % Primes[i] == 0) {
      Multipliers.push_back(SE.getConstant(Const->getType(), Primes[i], false));
      LLVM_DEBUG(
        dbgs() << "\t";
        Multipliers[Multipliers.size() - 1]->dump();
      );
      ConstValue /= Primes[i];
    } else {
      i--;
    }
  }
}

const SCEV* findGCD(SmallVectorImpl<const SCEV *> &Expressions,
  ScalarEvolution &SE) {
  assert(!Expressions.empty() && "GCD Expressions size must not be zero");

  SmallVector<const SCEV *, 3> Terms;

  //Release AddRec Expressions, multipliers are in step and start expressions
  for (auto *Expr : Expressions) {
    switch (Expr->getSCEVType()) {
    case scTruncate:
    case scZeroExtend:
    case scSignExtend: {
      auto *CastExpr = cast<SCEVCastExpr>(Expr);
      auto *InnerOp = CastExpr->getOperand();
      switch (InnerOp->getSCEVType()) {
      case scAddRecExpr: {
        auto *AddRec = cast<SCEVAddRecExpr>(InnerOp);
        auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
        auto *AddRecStart = AddRec->getStart();
        if (Expr->getSCEVType() == scTruncate) {
          Terms.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getTruncateExpr(AddRecStart, Expr->getType()));
        }
        if (Expr->getSCEVType() == scSignExtend) {
          Terms.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getSignExtendExpr(AddRecStart, Expr->getType()));
        }
        if (Expr->getSCEVType() == scZeroExtend) {
          Terms.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getZeroExtendExpr(AddRecStart, Expr->getType()));
        }
        break;
      }
      case scUnknown:
      case scAddExpr:
      case scMulExpr: {
        Terms.push_back(Expr);
        break;
      }
      }
      break;
    }
    case scConstant:
    case scUnknown:
    case scAddExpr: {
      Terms.push_back(Expr);
      break;
    }
    case scMulExpr: {
      auto *MulExpr = cast<SCEVMulExpr>(Expr);
      bool hasAddRec = false;
      SmallVector<const SCEV *, 3> StepMultipliers, StartMultipliers;
      for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
        auto *Op = MulExpr->getOperand(i);
        switch (Op->getSCEVType()) {
        case scTruncate:
        case scZeroExtend:
        case scSignExtend: {
          auto *InnerOp = cast<SCEVCastExpr>(Op)->getOperand();

          if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(InnerOp)) {
            hasAddRec = true;
            auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
            auto *AddRecStart = AddRec->getStart();
            if (Op->getSCEVType() == scTruncate) {
              StepMultipliers.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getTruncateExpr(AddRecStart, Op->getType()));
              }
            }
            if (Op->getSCEVType() == scSignExtend) {
              StepMultipliers.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getSignExtendExpr(AddRecStart, Op->getType()));
              }
            }
            if (Op->getSCEVType() == scZeroExtend) {
              StepMultipliers.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getZeroExtendExpr(AddRecStart, Op->getType()));
              }
            }
          } else if (auto *InnerMulExpr = dyn_cast<SCEVMulExpr>(InnerOp)) {
            StepMultipliers.push_back(Op);
            StartMultipliers.push_back(Op);
          } else if (InnerOp->getSCEVType() == scUnknown ||
            InnerOp->getSCEVType() == scAddExpr) {
            StepMultipliers.push_back(Op);
            StartMultipliers.push_back(Op);
          }
         break;
        }
        case scAddRecExpr: {
          auto *AddRec = cast<SCEVAddRecExpr>(Op);
          hasAddRec = true;
          auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
          auto *AddRecStart = AddRec->getStart();
          StepMultipliers.push_back(AddRecStepRecurrence);
          if (!AddRecStart->isZero()) {
            StartMultipliers.push_back(AddRecStart);
          }
          break;
        }
        case scUnknown:
        case scAddExpr:
        case scConstant: {
          StepMultipliers.push_back(Op);
          StartMultipliers.push_back(Op);
          break;
        }
        }
      }
      if (hasAddRec && !StartMultipliers.empty()) {
        SmallVector<const SCEV *, 2> AddRecInnerExpressions = {
          SE.getMulExpr(StartMultipliers),
          SE.getMulExpr(StepMultipliers)
        };
        Terms.push_back(findGCD(AddRecInnerExpressions, SE));
      } else if (!StepMultipliers.empty()) {
        Terms.push_back(SE.getMulExpr(StepMultipliers));
      }
     break;
    }
    case scAddRecExpr: {
      auto *AddRec = cast<SCEVAddRecExpr>(Expr);
      auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
      /*SCEVConstant *AddRecStartConstantMultiplier = nullptr;
      SCEVConstant *AddRecStepConstantMultiplier = nullptr;*/

      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStepRecurrence)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr ||
            Op->getSCEVType() == scConstant) {
            Multipliers.push_back(Op);
          }
          //if (auto *Const = dyn_cast<SCEVConstant>(Op)) {

          //}
        }
        AddRecStepRecurrence = SE.getMulExpr(Multipliers);
      }

      auto *AddRecStart = AddRec->getStart();
      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStart)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr ||
            Op->getSCEVType() == scConstant) {
            Multipliers.push_back(Op);
          }
        }
        AddRecStart = SE.getMulExpr(Multipliers);
      }
      SmallVector<const SCEV *, 2> AddRecInnerExpressions = {AddRecStart, AddRecStepRecurrence};
      //AddRecInnerExpressions.push_back();
      Terms.push_back(findGCD(AddRecInnerExpressions, SE));
      /*Terms.push_back(AddRecStepRecurrence);
      Terms.push_back(AddRecStart);*/
      break;
    }
    }
  }

  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] GCD Terms:\n";
    for (auto *Term : Terms) {
      dbgs() << "\t";
      Term->dump();
    });

  if (Terms.empty()) {
    return SE.getConstant(Expressions[0]->getType(), 1, true);
  }

  SmallVector<const SCEV *, 3> Dividers;

  const SCEV* OpeningSCEV = nullptr;

  //Finding not zero SCEV in Terms
  for (auto *Term : Terms) {
    if (!Term->isZero()) {
      OpeningSCEV = Term;
      break;
    }
  }
  if (!OpeningSCEV) {
    return SE.getConstant(Expressions[0]->getType(), 0, true);
  }

  //Start from multipliers of first SCEV, then exclude them step by step
  if (auto *Mul = dyn_cast<SCEVMulExpr>(OpeningSCEV)) {
    for (int i = 0; i < Mul->getNumOperands(); ++i) {
      if (auto *Const = dyn_cast<SCEVConstant>(Mul->getOperand(i))) {
        SmallVector<const SCEV *, 3> ConstMultipliers;
        countConstantMultipliers(Const, SE, ConstMultipliers);
        Dividers.append(ConstMultipliers.begin(), ConstMultipliers.end());
      } else {
        Dividers.push_back(Mul->getOperand(i));
      }
    }
  } else {
    if (auto *Const = dyn_cast<SCEVConstant>(OpeningSCEV)) {
      SmallVector<const SCEV *, 3> ConstMultipliers;
      countConstantMultipliers(Const, SE, ConstMultipliers);
      Dividers.append(ConstMultipliers.begin(), ConstMultipliers.end());
    } else {
      Dividers.push_back(OpeningSCEV);
    }
  }

  for (int i = 1; i < Terms.size(); ++i) {
    auto *CurrentTerm = Terms[i];
    SmallVector<const SCEV *, 3> ActualStepDividers;

    for (auto *Divider : Dividers) {
      const SCEV *Q, *R;
      SCEVDivision::divide(SE, CurrentTerm, Divider, &Q, &R);
      if (R->isZero()) {
        ActualStepDividers.push_back(Divider);
        CurrentTerm = Q;
        if (ActualStepDividers.size() == Dividers.size()) {
          break;
        }
      }
    }

    Dividers = ActualStepDividers;
    if (Dividers.size() == 0) {
      return SE.getConstant(Expressions[0]->getType(), 1, true);
    }
  }

  if (Dividers.size() == 1) {
    return Dividers[0];
  } else {
    return SE.getMulExpr(Dividers);
  }

}

#ifdef LLVM_DEBUG
void delinearizationLog(const DelinearizeInfo &Info, ScalarEvolution &SE,
    raw_ostream  &OS) {
  for (auto &ArrayInfo : Info.getArrays()) {
    OS << "[DELINEARIZE]: results for array ";
    ArrayInfo->getBase()->print(OS, true);
    OS << "\n";
    OS << "  number of dimensions: " << ArrayInfo->getNumberOfDims() << "\n";
    for (std::size_t I = 0, EI = ArrayInfo->getNumberOfDims(); I < EI; ++I) {
      OS << "    " << I << ": ";
      ArrayInfo->getDimSize(I)->print(OS);
      OS << "\n";
    }
    OS << "  accesses:\n";
    for (auto &El : *ArrayInfo) {
      OS << "    address: ";
      El.Ptr->print(OS, true);
      OS << "\n";
      for (auto *S : El.Subscripts) {
        OS << "      SCEV: ";
        S->print(OS);
        OS << "\n";
        auto Info = computeSCEVAddRec(S, SE);
        const SCEV *Coef, *ConstTerm;
        if (auto AddRec = dyn_cast<SCEVAddRecExpr>(Info.first)) {
          Coef = AddRec->getStepRecurrence(SE);
          ConstTerm = AddRec->getStart();
        } else {
          Coef = nullptr;
          ConstTerm = Info.first;
        }
        OS << "      a: ";
        if (Coef)
          Coef->print(OS);
        else
          OS << "null";
        OS << "\n";
        OS << "      b: ";
        if (ConstTerm)
          ConstTerm->print(OS);
        else
          OS << "null";
        OS << "\n";
        if (!Info.second)
          OS << "      unsafe cast\n";
      }
    }
  }
}
#endif
}

void DelinearizationPass::cleanSubscripts(Array &ArrayInfo) {
  assert(ArrayInfo.isDelinearized() && "Array must be delinearized!");
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: simplify subscripts for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto LastConstDim = ArrayInfo.getNumberOfDims();
  for (LastConstDim; LastConstDim > 0; --LastConstDim)
    if (!isa<SCEVConstant>(ArrayInfo.getDimSize(LastConstDim - 1)))
      break;
  if (LastConstDim == 0)
    return;
  auto *PrevDimSizesProduct = mSE->getConstant(mIndexTy, 1);
  for (auto DimIdx = LastConstDim - 1; DimIdx > 0; --DimIdx) {
    assert(ArrayInfo.isKnownDimSize(DimIdx) &&
      "Non-first unknown dimension in delinearized array!");
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct,
      mSE->getTruncateOrZeroExtend(ArrayInfo.getDimSize(DimIdx), mIndexTy));
    for (auto &Range: ArrayInfo) {
      auto *Subscript = Range.Subscripts[DimIdx - 1];
      const SCEV *Q = nullptr, *R = nullptr;
      SCEVDivision::divide(*mSE, Subscript, PrevDimSizesProduct, &Q, &R);
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: subscript " << DimIdx - 1 << " ";
        Subscript->dump();
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        PrevDimSizesProduct->dump();
        dbgs() << "[DELINEARIZE]: quotient "; Q->dump();
        dbgs() << "[DELINEARIZE]: remainder "; R->dump());
      if (!R->isZero()) {
        Range.Traits &= ~Range.IsValid;
        break;
      }
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: set subscript to "; Q->dump());
      Range.Subscripts[DimIdx - 1] = Q;
    }
  }
}

void DelinearizationPass::fillArrayDimensionsSizes(
    SmallVectorImpl<int64_t> &DimSizes, Array &ArrayInfo) {
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute sizes of dimensions for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto NumberOfDims = ArrayInfo.getNumberOfDims();
  auto LastUnknownDim = NumberOfDims;
  if (NumberOfDims == 0) {
    auto RangeItr = ArrayInfo.begin(), RangeItrE = ArrayInfo.end();
    for (; RangeItr != RangeItrE; ++RangeItr) {
      if (RangeItr->isElement() && RangeItr->isValid()) {
        NumberOfDims = RangeItr->Subscripts.size();
        break;
      }
    }
    if (RangeItr == RangeItrE) {
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: no valid element found\n";
        dbgs() << "[DELINEARIZE]: unable to determine number of"
        " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
      return;
    }
    for (++RangeItr; RangeItr != RangeItrE; ++RangeItr)
      if (RangeItr->isElement() && RangeItr->isValid())
        if (NumberOfDims != RangeItr->Subscripts.size()) {
          LLVM_DEBUG(
            dbgs() << "[DELINEARIZE]: unable to determine number of"
            " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
          return;
        }
    assert(NumberOfDims > 0 && "Scalar variable is treated as array?");
    DimSizes.resize(NumberOfDims, -1);
    ArrayInfo.setNumberOfDims(NumberOfDims);
    ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = NumberOfDims - 1;
    LLVM_DEBUG(
      dbgs() << "[DELINEARIZE]: extract number of dimensions from subscripts: "
             << NumberOfDims << "\n");
  } else {
    auto LastConstDim = DimSizes.size();
    for (auto I = DimSizes.size(); I > 0; --I) {
      if (DimSizes[I - 1] < 0)
        break;
      LastConstDim = I - 1;
      ArrayInfo.setDimSize(I - 1, mSE->getConstant(mIndexTy, DimSizes[I - 1]));
    }
    if (LastConstDim == 0) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: all dimensions have constant sizes\n");
      return;
    }
    if (DimSizes[0] > 0)
      ArrayInfo.setDimSize(0, mSE->getConstant(mIndexTy, DimSizes[0]));
    else
      ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = LastConstDim - 1;
  }
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute non-constant dimension sizes\n");
  auto *PrevDimSizesProduct = mSE->getConstant(mIndexTy, 1);
  auto DimIdx = LastUnknownDim;
  for (; DimIdx > 0; --DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process dimension " << DimIdx << "\n");
    const SCEV *Q, *R, *DimSize;
    if (DimSizes[DimIdx] > 0) {
      DimSize = mSE->getConstant(mIndexTy, DimSizes[DimIdx]);
    } else {
      SmallVector<const SCEV *, 3> Expressions;
      for (auto &Range: ArrayInfo) {
        if (!Range.isElement())
          continue;
        assert(Range.Subscripts.size() == NumberOfDims &&
          "Number of dimensions is inconsistent with number of subscripts!");
        for (auto J = DimIdx; J > 0; --J) {
          Expressions.push_back(Range.Subscripts[J - 1]);
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: use for GCD computation: ";
            Expressions.back()->dump());
        }
      }
      DimSize = findGCD(Expressions, *mSE);
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: GCD: "; DimSize->dump());
      SCEVDivision::divide(*mSE, DimSize, PrevDimSizesProduct, &Q, &R);
      DimSize = Q;
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        PrevDimSizesProduct->dump();
        dbgs() << "[DELINEARIZE]: quotient "; Q->dump();
        dbgs() << "[DELINEARIZE]: remainder "; R->dump());
    }
    if (DimSize->isZero()) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: could not compute dimension size\n");
      DimSize = mSE->getCouldNotCompute();
      ArrayInfo.setDimSize(DimIdx, DimSize);
      for (auto J : seq(decltype(DimIdx)(1), DimIdx)) {
        if (DimSizes[J] > 0)
          ArrayInfo.setDimSize(J, mSE->getConstant(mIndexTy, DimSizes[J]));
        else
          ArrayInfo.setDimSize(J, DimSize);
      }
      break;
    }
    ArrayInfo.setDimSize(DimIdx, DimSize);
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: dimension size is "; DimSize->dump());
    DimSize = mSE->getTruncateOrZeroExtend(DimSize, mIndexTy);
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct, DimSize);
  }
  if (DimIdx == 0)
    ArrayInfo.setDelinearized();
}

void DelinearizationPass::findArrayDimesionsFromDbgInfo(
    Value *BasePtr, SmallVectorImpl<int64_t> &Dimensions) {
  assert(BasePtr && "RootArray must not be null");
  SmallVector<DIMemoryLocation, 1> DILocs;
  auto DIM = findMetadata(BasePtr, DILocs, mDT);
  if (!DIM)
    return;
  assert(DIM->isValid() && "Debug memory location must be valid!");
  if (!DIM->Var->getType())
    return;
  auto VarDbgTy = DIM->Var->getType().resolve();
  DINodeArray ArrayDims = nullptr;
  bool IsFirstDimPointer = false;
  if (VarDbgTy->getTag() == dwarf::DW_TAG_array_type) {
    ArrayDims = cast<DICompositeType>(VarDbgTy)->getElements();
  } else if (VarDbgTy->getTag() == dwarf::DW_TAG_pointer_type) {
    IsFirstDimPointer = true;
    auto BaseTy = cast<DIDerivedType>(VarDbgTy)->getBaseType();
    if (BaseTy && BaseTy.resolve()->getTag() == dwarf::DW_TAG_array_type)
      ArrayDims = cast<DICompositeType>(BaseTy)->getElements();
  }
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZE]: number of array dimensions for "
           << BasePtr->getName() << " is ";
    dbgs() << (ArrayDims.size() + (IsFirstDimPointer ? 1 : 0)) << "\n");
  if (IsFirstDimPointer) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: first dimension is pointer\n");
    Dimensions.push_back(-1);
  }
  if (!ArrayDims)
    return;
  Dimensions.reserve(ArrayDims.size() + IsFirstDimPointer ? 1 : 0);
  for (unsigned int DimIdx = 0; DimIdx < ArrayDims.size(); ++DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: size of " << DimIdx << " dimension is ");
    int64_t DimSize = -1;
    if (auto *DIDim = dyn_cast<DISubrange>(ArrayDims[DimIdx])) {
      auto DIDimCount = DIDim->getCount();
      if (DIDimCount.is<ConstantInt*>()) {
        auto Count = DIDimCount.get<ConstantInt *>()->getValue();
        if (Count.getMinSignedBits() <= 64)
          DimSize = Count.getSExtValue();
        LLVM_DEBUG(dbgs() << DimSize << "\n");
      } else if (DIDimCount.is<DIVariable *>()) {
        LLVM_DEBUG(dbgs() << DIDimCount.get<DIVariable *>()->getName() << "\n");
      } else {
        LLVM_DEBUG( dbgs() << "unknown\n");
      }
    }
    Dimensions.push_back(DimSize);
  }
}

void DelinearizationPass::collectArrays(Function &F, DimensionMap &DimsCache) {
  for (auto &I : instructions(F)) {
    auto processMemory = [this, &DimsCache](Instruction &I, MemoryLocation Loc,
        unsigned,  AccessInfo, AccessInfo) {
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          return;
      }
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process instruction "; I.dump());
      auto &DL = I.getModule()->getDataLayout();
      auto *BasePtr = const_cast<Value *>(Loc.Ptr);
      BasePtr = GetUnderlyingObjectWithMetadata(BasePtr, DL);
      if (auto *LI = dyn_cast<LoadInst>(BasePtr))
        BasePtr = LI->getPointerOperand();
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: strip to base " << *BasePtr << "\n");
      auto DimsInfo = DimsCache.try_emplace(BasePtr);
      auto DimsItr = DimsInfo.first;
      if (DimsInfo.second)
        findArrayDimesionsFromDbgInfo(BasePtr, DimsItr->second);
      auto NumberOfDims = DimsItr->second.size();
      SmallVector<GEPOperator *, 3> GEPs;
      auto *GEP = dyn_cast<GEPOperator>(const_cast<Value *>(Loc.Ptr));
      while (GEP && (NumberOfDims == 0 || GEPs.size() < NumberOfDims)) {
        GEPs.push_back(GEP);
        GEP = dyn_cast<GEPOperator>(GEP->getPointerOperand());
      }
      SmallVector<Value *, 3> SubscriptValues;
      extractSubscriptsFromGEPs(GEPs.rbegin(), GEPs.rend(), SubscriptValues);
      auto ArrayItr = mDelinearizeInfo.getArrays().find_as(BasePtr);
      if (ArrayItr == mDelinearizeInfo.getArrays().end()) {
        ArrayItr = mDelinearizeInfo.getArrays().insert(new Array(BasePtr)).first;
        (*ArrayItr)->setNumberOfDims(NumberOfDims);
      }
      assert((*ArrayItr)->getNumberOfDims() == NumberOfDims &&
        "Inconsistent number of dimensions!");
      auto &El = (*ArrayItr)->addElement(
        GEPs.empty() ? const_cast<Value *>(Loc.Ptr) : GEPs.back());
      // In some cases zero subscript is dropping out by optimization passes.
      // So, we add extra zero subscripts at the beginning of subscript list.
      // We add subscripts for instructions which access a single element,
      // for example, in case of call it is possible to pass a whole array
      // as a parameter (without GEPs).
      // TODO (kaniandr@gmail.com): we could add subscripts not only at the
      // beginning of the list, try to implement smart extra subscript insertion.
      if (isa<LoadInst>(I) || isa<StoreInst>(I) ||
          isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        El.Traits |= Array::Element::IsElement;
        if (SubscriptValues.size() < NumberOfDims) {
          (*ArrayItr)->setRangeRef();
          for (std::size_t Idx = 0, IdxE = NumberOfDims - SubscriptValues.size();
               Idx < IdxE; Idx++) {
            El.Subscripts.push_back(mSE->getZero(mIndexTy));
            LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add extra zero subscript\n");
          }
        } else {
          El.Traits |= Array::Element::IsValid;
        }
      } else {
        El.Traits |= Array::Element::IsValid;
      }
      if (!SubscriptValues.empty()) {
        (*ArrayItr)->setRangeRef();
        for (auto *V : SubscriptValues)
          El.Subscripts.push_back(mSE->getSCEV(V));
      }
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: number of dimensions "
               << NumberOfDims << "\n";
        dbgs() << "[DELINEARIZE]: number of subscripts "
               << El.Subscripts.size() << "\n";
        dbgs() << "[DELINEARIZE]: element is "
               << (El.IsValid ? "valid" : "invalid") << "\n";
        dbgs() << "[DELINEARIZE]: subscripts: \n";
        for (auto *Subscript : El.Subscripts) {
          dbgs() << "  "; Subscript->dump();
        }
      );
    };
    for_each_memory(I, *mTLI, processMemory,
      [](Instruction &, AccessInfo, AccessInfo) {});
  }
  // Now, we remove all object which is not arrays.
  for (auto Itr = mDelinearizeInfo.getArrays().begin(),
       ItrE = mDelinearizeInfo.getArrays().end(); Itr != ItrE;) {
    auto CurrItr = Itr++;
    if ((*CurrItr)->getNumberOfDims() == 0 && !(*CurrItr)->hasRangeRef()) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: not an array "
                        << (*CurrItr)->getBase()->getName() << "\n");
      DimsCache.erase((*CurrItr)->getBase());
      mDelinearizeInfo.getArrays().erase(CurrItr);
    }
  }
}

void DelinearizationPass::findSubscripts(Function &F) {
  DimensionMap DimsCache;
  collectArrays(F, DimsCache);
  for (auto *ArrayInfo : mDelinearizeInfo.getArrays()) {
    auto DimsItr = DimsCache.find(ArrayInfo->getBase());
    assert(DimsItr != DimsCache.end() &&
      "Cache of dimension sizes must be constructed!");
    fillArrayDimensionsSizes(DimsItr->second, *ArrayInfo);
    if (ArrayInfo->isDelinearized()) {
      cleanSubscripts(*ArrayInfo);
    } else {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize "
                        << DimsItr->first->getName() << "\n");
    }
  }
}

bool DelinearizationPass::runOnFunction(Function &F) {
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZE]: process function " << F.getName() << "\n");
  releaseMemory();
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &DL = F.getParent()->getDataLayout();
  mIndexTy = DL.getIndexType(Type::getInt8PtrTy(F.getContext()));
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: index type is "; mIndexTy->dump());
  findSubscripts(F);
  mDelinearizeInfo.fillElementsMap();
  LLVM_DEBUG(delinearizationLog(mDelinearizeInfo, *mSE, dbgs()));
  return false;
}

void DelinearizationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.setPreservesAll();
}

void DelinearizationPass::print(raw_ostream &OS, const Module *M) const {
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  RawDelinearizeInfo Info = tsar::toJSON(mDelinearizeInfo, SE);
  OS << json::Parser<RawDelinearizeInfo>::unparse(Info) << '\n';
}

FunctionPass * createDelinearizationPass() { return new DelinearizationPass; }

RawDelinearizeInfo tsar::toJSON(const DelinearizeInfo &Info, ScalarEvolution &SE) {
  RawDelinearizeInfo RawInfo;
  for (auto &ArrayInfo : Info.getArrays()) {
    std::string NameStr;
    raw_string_ostream NameOS(NameStr);
    NameOS.flush();
    ArrayInfo->getBase()->print(NameOS);
    std::vector<std::string> DimSizes(ArrayInfo->getNumberOfDims());
    for (std::size_t I = 0, EI = DimSizes.size(); I < EI; ++I) {
      raw_string_ostream DimSizeOS(DimSizes[I]);
      ArrayInfo->getDimSize(I)->print(DimSizeOS);
      DimSizeOS.flush();
    }
    std::vector<std::vector<std::vector<std::string>>> Accesses;
    for (auto &El : *ArrayInfo) {
      std::vector<std::vector<std::string>> Subscripts;
      for (auto *S : El.Subscripts) {
        Subscripts.emplace_back(2);
        auto &CoefStr = Subscripts.back().front();
        auto &ConstTermStr = Subscripts.back().back();
        auto Info = computeSCEVAddRec(S, SE);
        const SCEV *Coef, *ConstTerm;
        if (auto AddRec = dyn_cast<SCEVAddRecExpr>(Info.first)) {
          Coef = AddRec->getStepRecurrence(SE);
          ConstTerm = AddRec->getStart();
        } else {
          Coef = nullptr;
          ConstTerm = Info.first;
        }
        if (Coef) {
          raw_string_ostream CoefOS(CoefStr);
          Coef->print(CoefOS);
          CoefOS.flush();
        } else {
          CoefStr = "null";
        }
        if (ConstTerm) {
          raw_string_ostream ConstTermOS(ConstTermStr);
          ConstTerm->print(ConstTermOS);
          ConstTermOS.flush();
        } else {
          ConstTermStr = "null";
        }
      }
      Accesses.push_back(std::move(Subscripts));
    }
    RawInfo[RawDelinearizeInfo::Sizes].emplace(NameStr, std::move(DimSizes));
    RawInfo[RawDelinearizeInfo::Accesses].emplace(
      std::move(NameStr), std::move(Accesses));
  }
  return RawInfo;
}

