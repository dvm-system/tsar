#include "tsar_array_subscript_delinearize.h"
#include "tsar_pass.h"
#include "tsar_transformation.h"
#include "tsar_array_usage_matcher.h"
#include "tsar_utility.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/PassAnalysisSupport.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseSet.h>
#include "llvm/Analysis/InstructionSimplify.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/IR/Type.h>

#include <vector>
#include <utility>
#include <memory>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "array-subscript-delinearize"

char ArraySubscriptDelinearizePass::ID = 0;
INITIALIZE_PASS_BEGIN(ArraySubscriptDelinearizePass, "array-subscript-delinearize",
  "Array Subscript Delinearize", false, true)
  //INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(ArrayUsageMatcherImmutableWrapper)
INITIALIZE_PASS_END(ArraySubscriptDelinearizePass, "array-subscript-delinearize",
  "Array Subscript Delinearize", false, true)

STATISTIC(NumDelinearizedSubscripts, "Number of delinearized subscripts");

static cl::opt<unsigned> MaxSCEVCompareDepth(
  "scalar-evolution-max-scev-compare-depth-local-copy", cl::Hidden,
  cl::desc("Maximum depth of recursive SCEV complexity comparisons"),
  cl::init(32));

static cl::opt<unsigned> MaxValueCompareDepth(
  "scalar-evolution-max-value-compare-depth-local-copy", cl::Hidden,
  cl::desc("Maximum depth of recursive value complexity comparisons"),
  cl::init(2));

namespace tsar {
Array::iterator Array::begin() {
  return mElements.begin();
}

Array::iterator Array::end() {
  return mElements.end();
}

Array::const_iterator Array::cbegin() const {
  return mElements.cbegin();
}

Array::const_iterator Array::cend() const {
  return mElements.cend();
}

std::size_t Array::size() const {
  return mElements.size();
}

bool Array::empty() const {
  return mElements.empty();
}

void Array::pushBack(const Element &Access) {
  mElements.push_back(Access);
}

void Array::pushBack(Element &&Access) {
  mElements.push_back(std::move(Access));
}

//void Array::erase(int ElementIdx) {
//  mElements.erase(mElements.cbegin() + ElementIdx);
//}

llvm::Value *Array::getBase() const {
  return mBasePtr;
}

const Array::ExprList & Array::getDimensions() const {
  return mDims;
}

std::size_t Array::getDimensionsCount() const {
  return mDims.size();
}

//const Array::Element * Array::findElement(Value *Ptr) const {
//  for (auto Access : mElements) {
//    if (Access.Ptr == Ptr) {
//      return &Access;
//    }
//  }
//  return nullptr;
//}

Array::Element * Array::getElement(std::size_t Idx) {
  if (Idx >= 0 && Idx < mElements.size()) {
    return &mElements[Idx];
  }
  return nullptr;
}

void Array::emplaceBack(llvm::Value *Ptr, const ExprList &Subscript, bool isValid) {
  mElements.emplace_back(Ptr, Subscript, isValid);
}

void Array::emplaceBack(llvm::Value *Ptr, ExprList &&Subscript, bool isValid) {
  mElements.emplace_back(Ptr, std::move(Subscript), isValid);
}

void Array::clearDimensions() {
  mDims.clear();
}

void Array::resizeDimensions(std::size_t Size) {
  mDims.resize(Size);
}

void Array::setDimension(std::size_t DimIdx, const llvm::SCEV *Expr) {
  if (DimIdx >= 0 && DimIdx < mDims.size()) {
    mDims[DimIdx] = Expr;
  }
}

const SCEV * Array::getDimension(std::size_t DimIdx) {
  if (DimIdx >= 0 && DimIdx < mDims.size()) {
    return mDims[DimIdx];
  }
  return nullptr;
}

bool Array::isDimensionsEmpty() const {
  return mDims.empty();
}

bool Array::isValid() const {
  return mIsArrayValid;
}

void Array::setValid(bool Valid) {
  mIsArrayValid = Valid;
}

}

namespace {
std::pair<const SCEV *, const SCEV *> findCoefficientsInSCEVMulExpr(const SCEVMulExpr *MulExpr,
  ScalarEvolution &SE) {
  assert(MulExpr && "MulExpr must not be null");
  SmallVector<const SCEV*, 2> AMultipliers, BMultipliers;
  bool hasAddRec = false;

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
          AMultipliers.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Op->getType()));
          BMultipliers.push_back(SE.getTruncateExpr(AddRecStart, Op->getType()));
        }
        if (Op->getSCEVType() == scSignExtend) {
          AMultipliers.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Op->getType()));
          BMultipliers.push_back(SE.getSignExtendExpr(AddRecStart, Op->getType()));
        }
        if (Op->getSCEVType() == scZeroExtend) {
          AMultipliers.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Op->getType()));
          BMultipliers.push_back(SE.getZeroExtendExpr(AddRecStart, Op->getType()));
        }
      } else {
        AMultipliers.push_back(Op);
        BMultipliers.push_back(Op);
      }
      break;
    }
    case scAddRecExpr: {
      hasAddRec = true;

      auto *AddRec = cast<SCEVAddRecExpr>(Op);

      auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
      auto *AddRecStart = AddRec->getStart();

      AMultipliers.push_back(AddRecStepRecurrence);
      BMultipliers.push_back(AddRecStart);

      break;
    }
    default: {
      AMultipliers.push_back(Op);
      BMultipliers.push_back(Op);
      break;
    }
    }

  }
  if (!hasAddRec) {
    return std::make_pair(SE.getConstant(APInt(sizeof(int), 0, true)), MulExpr);
  }
  auto *ACoefficient = SE.getMulExpr(AMultipliers);
  auto *BCoefficient = SE.getMulExpr(BMultipliers);
  return std::make_pair(ACoefficient, BCoefficient);
}
}

namespace tsar {
std::pair<const SCEV *, const SCEV *> tsar::findCoefficientsInSCEV(const SCEV *Expr, ScalarEvolution &SE) {
  assert(Expr && "Expression must not be null");
  switch (Expr->getSCEVType()) {
  case scTruncate:
  case scZeroExtend:
  case scSignExtend: {
    return findCoefficientsInSCEV(cast<SCEVCastExpr>(Expr)->getOperand(), SE);
  }
  case scAddRecExpr: {
    auto *AddRec = cast<SCEVAddRecExpr>(Expr);

    auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
    if (auto *CastExpr = dyn_cast<SCEVCastExpr>(AddRecStepRecurrence)) {
      AddRecStepRecurrence = CastExpr->getOperand();
    }

    auto *AddRecStart = AddRec->getStart();
    if (auto *CastExpr = dyn_cast<SCEVCastExpr>(AddRecStart)) {
      AddRecStart = CastExpr->getOperand();
    }

    return std::make_pair(AddRecStepRecurrence, AddRecStart);
  }
  case scAddExpr:
  case scConstant:
  case scUnknown: {
    return std::make_pair(SE.getConstant(APInt(sizeof(int), 0, true)), Expr);
  }
  case scMulExpr: {
    return findCoefficientsInSCEVMulExpr(cast<SCEVMulExpr>(Expr), SE);
  }
  default: {
    return std::make_pair(SE.getConstant(APInt(sizeof(int), 0, true)),
      SE.getConstant(APInt(sizeof(int), 0, true)));
  }
  }
}
}

namespace llvm {
const tsar::Array * DelinearizeInfo::findArray(llvm::Value *BasePtr) const {
  auto ResultIt = mArrays.find_as(BasePtr);
  if (ResultIt != mArrays.end()) {
    return &(*ResultIt);
  }
  return nullptr;
}

std::pair<Array *, Array::Element *> DelinearizeInfo::findElement(Value *Ptr) {
  auto ArrayElementPairIter = mElements.find(Ptr);
  if (ArrayElementPairIter != mElements.end()) {
    Array *TargetArray = ArrayElementPairIter->second.first;
    Array::Element *TargetElement = TargetArray->getElement(
      ArrayElementPairIter->second.second);
    return std::make_pair(TargetArray, TargetElement);
  }

  return(std::make_pair(nullptr, nullptr));
}

void DelinearizeInfo::fillElementsMap() {
  mElements.clear();
  for (auto &CurrentArray : mArrays) {
    int i = 0;
    for (auto CurrentElementIt = CurrentArray.begin();
      CurrentElementIt != CurrentArray.end();
      ++CurrentElementIt, ++i) {
      mElements.insert(std::make_pair(CurrentElementIt->Ptr,
        std::make_pair(&CurrentArray, i)));
    }
  }
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
    unsigned LNumOps = LA->getNumOperands(), RNumOps = RA->getNumOperands();
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
    unsigned LNumOps = LC->getNumOperands(), RNumOps = RC->getNumOperands();
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

Value *GetUnderlyingObjectWithDbgCheck(Value *V, const DataLayout &DL,
  unsigned MaxLookup) {
  if (!V->getType()->isPointerTy())
    return V;
  for (unsigned Count = 0; MaxLookup == 0 || Count < MaxLookup; ++Count) {
    DbgValueList DbgValues;
    FindAllocaDbgValues(DbgValues, V);
    if (!DbgValues.empty()) {
      return V;
    }
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
      V = GEP->getPointerOperand();
    } else if (Operator::getOpcode(V) == Instruction::BitCast ||
      Operator::getOpcode(V) == Instruction::AddrSpaceCast) {
      V = cast<Operator>(V)->getOperand(0);
    } else if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V)) {
      if (GA->isInterposable())
        return V;
      V = GA->getAliasee();
    } else {
      if (auto CS = CallSite(V))
        if (Value *RV = CS.getReturnedArgOperand()) {
          V = RV;
          continue;
        }

      // See if InstructionSimplify knows any relevant tricks.
      if (Instruction *I = dyn_cast<Instruction>(V))
        // TODO: Acquire a DominatorTree and AssumptionCache and use them.
        if (Value *Simplified = SimplifyInstruction(I, DL, nullptr)) {
          V = Simplified;
          continue;
        }

      return V;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  }
  return V;
}

const DIVariable* findVariableDbg(Value* V) {
  assert(V && "Value must not be null");
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    return getMetadata(GV);
  }

  if (auto *AI = dyn_cast<AllocaInst>(V)) {
    return getMetadata(AI);
  }

  //TODO replace by a new function from tsar_ulility
  if (auto *DDI = FindAllocaDbgDeclare(V)) {
    return DDI->getVariable();
  } 

  DbgValueList DbgValues;
  FindAllocaDbgValues(DbgValues, V);
  if (!DbgValues.empty()) {
    return DbgValues[0]->getVariable();
  }

  return nullptr;
}

Value *findRootArray(Instruction *I) {
  Value *PointerOp;
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    PointerOp = SI->getPointerOperand();
  }
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    PointerOp = LI->getPointerOperand();
  }

  if (auto *Gep = dyn_cast<GetElementPtrInst>(PointerOp)) {
    Value * RootArr = (GetUnderlyingObjectWithDbgCheck(Gep, Gep->getModule()->getDataLayout(), 0));
    //For function argument GetUnderlyingObject returns LoadInst with
    //RootArr in operands
    if (auto *LI = dyn_cast<LoadInst>(RootArr)) {
      RootArr = LI->getPointerOperand();
    }
    DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] RootArr " << *RootArr << "\n";
    );
    return RootArr;
  } 
  
  if (auto *GV = dyn_cast<GlobalVariable>(PointerOp)) {
    DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] RootArrGV " << *GV << "\n";
    );
    return GV;
  }

  return nullptr;
}

void findArrayDimesionsFromDbgInfo(Value *RootArr,
  SmallVectorImpl<int> &Dimensions) {
  assert(RootArr && "RootArray must not be null");

  Dimensions.clear();

  auto *VarDbg = findVariableDbg(RootArr);
  if (!VarDbg)
    return;

  auto VarDbgType = VarDbg->getType();
  if (auto *GVE = dyn_cast<DIGlobalVariableExpression>(VarDbgType)) {
    //GVE->
  }

  DINodeArray TypeElements = nullptr;
  bool IsFirstDimPointer = false;
  if (auto *ArrayCompositeType = dyn_cast<DICompositeType>(VarDbgType)) {
    if (ArrayCompositeType->getTag() == dwarf::DW_TAG_array_type) {
      TypeElements = ArrayCompositeType->getElements();
    }
  } else if (auto *DerivedType = dyn_cast<DIDerivedType>(VarDbgType)) {
    if (DerivedType->getTag() == dwarf::DW_TAG_pointer_type ||
      DerivedType->getTag() == dwarf::DW_TAG_array_type) {
      if (DerivedType->getTag() == dwarf::DW_TAG_pointer_type) {
        IsFirstDimPointer = true;
      }

      if (auto *InnerCompositeType =
        dyn_cast<DICompositeType>(DerivedType->getBaseType())) {
        if (InnerCompositeType->getTag() == dwarf::DW_TAG_array_type) {
          TypeElements = InnerCompositeType->getElements();
        }
      } else {
        //TypeElements = DerivedType->getBaseType();
      }
    }
  }

  DEBUG(
    if (IsFirstDimPointer && TypeElements) {
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Array dimensions count: "
        << TypeElements.size() + 1 << "\n\t"
        << RootArr->getName() << "[]";
    } else {
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Array dimensions count: "
      << TypeElements.size() << "\n\t"
      << RootArr->getName();
    }
  );

  if (IsFirstDimPointer) {
    Dimensions.push_back(-1);
  }

  if (!TypeElements) {
    return;
  }

  for (unsigned int i = 0; i < TypeElements.size(); ++i) {
    if (auto *DimSize = dyn_cast<DISubrange>(TypeElements[i])) {
      Dimensions.push_back(DimSize->getCount());
      DEBUG(
        dbgs() << "[";
      if (DimSize->getCount() > 0)
        dbgs() << DimSize->getCount();
      dbgs() << "]";
      );
    }
  }
  DEBUG(
    dbgs() << "\n";
  );
}

GetElementPtrInst *findGepOperand(Instruction *I) {
  assert(I && "Instruction must not be null");
  dbgs() << "Find Gep operand start\n";
  for (unsigned int i = 0; i < I->getNumOperands(); ++i) {
    I->getOperand(i)->dump();
    if (auto *Gep = dyn_cast<GetElementPtrInst>(I->getOperand(i))) {
      dbgs() << "Find Gep operand finish with gep\n";
      return Gep;
    }
  }
  dbgs() << "Find Gep operand finish\n";
  return nullptr;
}

void findGeps(Instruction *I, SmallVectorImpl<GetElementPtrInst *> &Geps) {
  assert(I && "Instruction must not be null");
  auto *CurrentInst = I;
  do {
    CurrentInst->dump();
    auto *Gep = findGepOperand(CurrentInst);
    if (Gep) {
      Geps.push_back(Gep);
      CurrentInst = Gep;
    } else {
      CurrentInst = nullptr;
      for (unsigned int i = 0; i < I->getNumOperands(); ++i) {
        if (auto *LI = dyn_cast<LoadInst>(I->getOperand(i))) {
          dbgs() << "Found LI";
          LI->dump();
          if (auto* InnerGep = findGepOperand(LI)) {
            dbgs() << "Found InnerGep";
            InnerGep->dump();
            Geps.push_back(InnerGep);
            CurrentInst = InnerGep;
            break;
          } else {
            dbgs() << "Dont found InnerGep";
          }
        }
      }
    }
  } while (CurrentInst);
  std::reverse(Geps.begin(), Geps.end());
  DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]GEPS size: " << Geps.size() << "\n";
  );
}

void findLLVMIdxs(Instruction *I, SmallVectorImpl<GetElementPtrInst *> &Geps, 
  SmallVectorImpl<Value *> &Idxs) {
  assert(I && "Instruction must not be null");
  assert(!Geps.empty() && "Geps size must not be zero");
  Idxs.clear();
  for (auto &Gep : Geps) {
    int numOperands = Gep->getNumOperands();
    if (numOperands == 2) {
      Idxs.push_back(Gep->getOperand(1));
    } else {
      if (auto *SecondOp = dyn_cast<ConstantData>(Gep->getOperand(1))) {
        if (!SecondOp->isZeroValue()) {
          Idxs.push_back(Gep->getOperand(1));
        }
      }
      for (int i = 2; i < numOperands; ++i) {
        Idxs.push_back(Gep->getOperand(i));
      }
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
              StartMultipliers.push_back(SE.getTruncateExpr(AddRecStart, Op->getType()));
            }
            if (Op->getSCEVType() == scSignExtend) {
              StepMultipliers.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Op->getType()));
              StartMultipliers.push_back(SE.getSignExtendExpr(AddRecStart, Op->getType()));
            }
            if (Op->getSCEVType() == scZeroExtend) {
              StepMultipliers.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Op->getType()));
              StartMultipliers.push_back(SE.getZeroExtendExpr(AddRecStart, Op->getType()));
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
          StartMultipliers.push_back(AddRecStart);
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
      if (!StepMultipliers.empty()) {
        Terms.push_back(SE.getMulExpr(StepMultipliers));
      }
      if (hasAddRec) {
        if (!StartMultipliers.empty()) {
          Terms.push_back(SE.getMulExpr(StartMultipliers));
        }
      }
     break;
    }
    case scAddRecExpr: {
      auto *AddRec = cast<SCEVAddRecExpr>(Expr);
      auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStepRecurrence)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr) {
            Multipliers.push_back(Op);
          }
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
            Op->getSCEVType() == scAddExpr) {
            Multipliers.push_back(Op);
          }
        }
        AddRecStart = SE.getMulExpr(Multipliers);
      }

      Terms.push_back(AddRecStepRecurrence);
      Terms.push_back(AddRecStart);
      break;
    }
    }
  }

  DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]GCD Terms:\n";
    for (auto *Term : Terms) {
      dbgs() << "\t";
      Term->dump();
    });

  if (Terms.empty()) {
    return SE.getConstant(Expressions[0]->getType(), 1, true);
  }

  SmallVector<const SCEV *, 3> Dividers;

  const SCEV* OpeningSCEV;

  //Finding not zero SCEV in Terms
  for (auto *Term : Terms) {
    if (!Term->isZero()) {
      OpeningSCEV = Term;
      break;
    }
    return SE.getConstant(Expressions[0]->getType(), 1, true);
  }

  //Start from multipliers of first SCEV, then exclude them step by step
  if (auto *Mul = dyn_cast<SCEVMulExpr>(OpeningSCEV)) {
    for (int i = 0; i < Mul->getNumOperands(); ++i) {
      Dividers.push_back(Mul->getOperand(i));
    }
  } else {
    Dividers.push_back(OpeningSCEV);
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

void collectArrays(DelinearizeInfo::ArraySet &AnalyzedArrays, Function &F,
  ScalarEvolution &SE) {

  for (auto &BB :  F) {
    for (auto &I : BB) {
      if (isa<StoreInst>(I) || isa<LoadInst>(I)) {

        Value *RootArr = findRootArray(&I);
        if (!RootArr) {
          continue;
        }

        SmallVector<GetElementPtrInst *, 3> Geps;
        findGeps(&I, Geps);

        if (Geps.empty()) {
          continue;
        }

        SmallVector<Value *, 3> Idxs;
        findLLVMIdxs(&I, Geps, Idxs);

        if (Idxs.empty()) {
          continue;
        }

        Array::ExprList Subscripts;
        for (auto *Idx : Idxs) {
          auto *IdxSCEV = SE.getSCEV(Idx);
          Subscripts.push_back(IdxSCEV);
        }

        DEBUG(
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Inst: ";
          I.dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Idxs: " << Idxs.size() << "\n";
          for (auto *Idx : Idxs) {
            dbgs() << "\t";
            Idx->dump();
            auto *IdxSCEV = SE.getSCEV(Idx);
            dbgs() << "\t\t";
            IdxSCEV->dump();
          }
        );

        Array *CurrentArray;
        auto ArrayEntryIt = AnalyzedArrays.find_as(RootArr);
        if (ArrayEntryIt != AnalyzedArrays.end()) {
          CurrentArray = &(*ArrayEntryIt);
        } else  {
          auto InsertResult = AnalyzedArrays.insert(Array(RootArr));
          if (!InsertResult.second) {
            continue;
          }
          CurrentArray = &(*InsertResult.first);
        }
        
        CurrentArray->emplaceBack(Geps[0], std::move(Subscripts));
      }
    }
  }
}

void fillArrayDimensionsSizes(Array *CurrentArray, ScalarEvolution &SE) {
  assert(CurrentArray && "Current Array must not be null");
  assert(!CurrentArray->empty() && "Acesses size must not be zero");

  SmallVector<int, 3> Dimensions;
  findArrayDimesionsFromDbgInfo(CurrentArray->getBase(), Dimensions);
  //TODO Если есть отлдадочная информация, то она в приоритете, если нет отладочной, 
  //а в инструкциях не совпадает количество измерений, то забраковывать массив
  //Проанализировать разыменование не всех измерений, что возвращает GEP
  //Если уверенно найдены измерения, то отбрасывать инструкцию, если неуверенно, то весь массив
  
  if (Dimensions.empty()) {
    int DimensionsCount = CurrentArray->getElement(0)->Subscript.size();

    for (int i = 1; i < CurrentArray->size(); ++i) {
      if (CurrentArray->getElement(0)->Subscript.size() != DimensionsCount) {
        CurrentArray->setValid(false);
        DEBUG(
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Array " <<
            CurrentArray->getBase()->getName() << " is unrealible\n";
        );
        return;
      }
    }

    Dimensions.resize(DimensionsCount);
    for (int i = 0; i < DimensionsCount; ++i) {
      Dimensions[i] = -1;
    }
  }

  //Fill const dimensions sizes
  CurrentArray->clearDimensions();
  CurrentArray->resizeDimensions(Dimensions.size());

  Type *Ty = CurrentArray->getElement(0)->Subscript[0]->getType();

  int LastConstDimension = Dimensions.size();
  //Find last (from left to right) dimension with constant size, extreme left is always unknown
  for (int i = Dimensions.size() - 1; i > 0; --i) {
    if (Dimensions[i] > 0) {
      LastConstDimension = i;
      CurrentArray->setDimension(i, SE.getConstant(Ty, Dimensions[i], true));
    } else {
      break;
    }
  }

  int FirstUnknownDimension = LastConstDimension - 1;


  //TODO Think about debug info
  if (Dimensions[0] > 0) {
    CurrentArray->setDimension(0, SE.getConstant(Ty, Dimensions[0], true));
    --LastConstDimension;
  } else {
    CurrentArray->setDimension(0, SE.getCouldNotCompute());
  }

  DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Filling array const dims from " <<
      LastConstDimension << " to " << Dimensions.size() - 1 << "\n";
  );

  if (!FirstUnknownDimension) {
    return;
  }

  DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Start filling\n";
  );

  auto *PrevioslyDimsSizesProduct = SE.getConstant(Ty, 1, true);

  for (int i = FirstUnknownDimension - 1; i >= 0; --i) {
    DEBUG(
      dbgs() << "i: " << i << "\n";
    );
    const SCEV *Q, *R, *DimSize;

    //Find divider (dimension size), switch by constant or variable
    if (Dimensions[i + 1] > 0) {
      DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Const dim size\n";
      );
      DimSize = SE.getConstant(Ty, Dimensions[i + 1], true);
    } else {
      DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Var dim size\n";
      );
      SmallVector<const SCEV *, 3> Expressions;
      for (auto CurrentElementIt = CurrentArray->begin();
        CurrentElementIt != CurrentArray->end(); ++CurrentElementIt) {
        if (CurrentElementIt->Subscript.size() != Dimensions.size()) {
          CurrentElementIt->isValid = false;
        } else {
          for (int j = i; j >= 0; --j) {
            Expressions.push_back(CurrentElementIt->Subscript[j]);
          }
        }
      }
      DimSize = findGCD(Expressions, SE);

      SCEVDivision::divide(SE, DimSize, PrevioslyDimsSizesProduct, &Q, &R);
      if (R->isZero()) {
        DimSize = Q;
      } else {
        DimSize = SE.getConstant(Ty, 1, true);
        CurrentArray->setValid(false);
        break;
      }
    }

    CurrentArray->setDimension(i + 1, DimSize);

    PrevioslyDimsSizesProduct = SE.getMulExpr(PrevioslyDimsSizesProduct, DimSize);
  }
}

void cleanSubscripts(Array *CurrentArray, ScalarEvolution &SE) {
  assert(CurrentArray && "Current Array must not be null");
  assert(!CurrentArray->empty() && "Acesses size must not be zero");

  int LastConstDimension = CurrentArray->getDimensionsCount();
  //find last (from left to right) dimension with constant size, extreme left is always unknown
  for (int i = CurrentArray->getDimensionsCount() - 1; i > 0; --i) {
    if (isa<SCEVConstant>(CurrentArray->getDimension(i))) {
      LastConstDimension = i;
    } else {
      break;
    }
  }

  int FirstUnknownDimension = LastConstDimension - 1;

  const SCEV *Q, *R;

  Type *Ty = CurrentArray->getElement(0)->Subscript[0]->getType();
  auto *PrivioslyDimsSizesProduct = SE.getConstant(Ty, 1, true);

  for (int i = FirstUnknownDimension - 1; i >= 0; --i) {
    PrivioslyDimsSizesProduct = SE.getMulExpr(PrivioslyDimsSizesProduct, 
      CurrentArray->getDimension(i + 1));
    for (auto CurrentElementIt = CurrentArray->begin();
      CurrentElementIt != CurrentArray->end();
      ++CurrentElementIt) {
      auto *CurrentSCEV = CurrentElementIt->Subscript[i];
        SCEVDivision::divide(SE, CurrentSCEV, PrivioslyDimsSizesProduct, &Q, &R);
        DEBUG(
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] SCEV: ";
          CurrentSCEV->dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Divider: ";
          PrivioslyDimsSizesProduct->dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] R: ";
          R->dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Q: ";
          Q->dump();
        );
        if (R->isZero()) {
          CurrentSCEV = Q;
        } else {
          assert(false && "Cant divide access");
        }
      DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Set ";
        CurrentSCEV->dump();
      );
      CurrentElementIt->Subscript[i] = CurrentSCEV;
    }
  }
}

void findSubscripts(DelinearizeInfo::ArraySet &AnalyzedArrays, Function &F, 
  ScalarEvolution &SE) {
  collectArrays(AnalyzedArrays, F, SE);

  for (auto &CurrentArray : AnalyzedArrays) {
    fillArrayDimensionsSizes(&CurrentArray, SE);
    if (!CurrentArray.isDimensionsEmpty()) {
      cleanSubscripts(&CurrentArray, SE);
    }
  }
}

}

namespace llvm {
bool ArraySubscriptDelinearizePass::runOnFunction(Function &F) {
  DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] In function " << F.getName() << "\n";
  F.dump();
  );

  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &AM = getAnalysis<ArrayUsageMatcherImmutableWrapper>().get();

  DelinearizeInfo::ArraySet AnalyzedArrays;

  findSubscripts(AnalyzedArrays, F, SE);

  mDelinearizeInfo = DelinearizeInfo(AnalyzedArrays);
  mDelinearizeInfo.fillElementsMap();

  DEBUG(
    for (auto &ArrayEntry : AnalyzedArrays) {
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Array ";
      ArrayEntry.getBase()->dump();

      dbgs() << "Dims:" << ArrayEntry.getDimensionsCount() << "\n";
      for (auto *DimSize : ArrayEntry.getDimensions()) {
        dbgs() << "\t";
        DimSize->dump();
      }
      dbgs() << "\n";

      dbgs() << "Accesses:\n";
      for (auto ElementEntryIt = ArrayEntry.begin();
        ElementEntryIt != ArrayEntry.end();
        ++ElementEntryIt) {
        dbgs() << "\tValue: ";
        ElementEntryIt->Ptr->dump();
        for (auto *S : ElementEntryIt->Subscript) {
          dbgs() << "\tSCEV: ";
          S->dump();

          auto Idxs = findCoefficientsInSCEV(S, SE);
          if (Idxs.first) {
            dbgs() << "\ta: ";
            Idxs.first->dump();
          } else {
            dbgs() << "\ta: null";
          }
          if (Idxs.second) {
            dbgs() << "\tb: ";
            Idxs.second->dump();
          } else {
            dbgs() << "\tb: null";
          }
        }
        dbgs() << "\n";
      }
    });
  return false;
}

void ArraySubscriptDelinearizePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ArrayUsageMatcherImmutableWrapper>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass * createArraySubscriptDelinearizePass() {
  return new ArraySubscriptDelinearizePass;
}
}