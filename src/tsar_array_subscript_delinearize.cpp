#include "tsar_array_subscript_delinearize.h"
#include "DelinearizeJSON.h"
#include "KnownFunctionTraits.h"
#include "MemoryAccessUtils.h"
#include "tsar_query.h"
#include "tsar_utility.h"
#include "tsar/Support/SCEVUtils.h"
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

inline const SCEV * stripCastIfNot(const SCEV *S, bool IsSafeTypeCast) {
  assert(S && "SCEV must not be null!");
  if (!IsSafeTypeCast)
    while (auto *Cast = dyn_cast<SCEVCastExpr>(S))
      S = Cast->getOperand();
  return S;
}

const SCEV* findGCD(ArrayRef<const SCEV *> Expressions,
  ScalarEvolution &SE, bool IsSafeTypeCast = true) {
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
          Coef = SE.getZero(Info.first->getType());
          ConstTerm = Info.first;
        }
        OS << "      a: ";
        Coef->print(OS);
        OS << "\n";
        OS << "      b: ";
        ConstTerm->print(OS);
        OS << "\n";
        if (!Info.second)
          OS << "      with unsafe cast\n";
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
  for (auto &Range : ArrayInfo) {
    assert(ArrayInfo.getNumberOfDims() - LastConstDim <= Range.Subscripts.size()
      && "Unknown subscripts in right dimensions with constant sizes!");
    // In some cases zero subscript is dropping out by optimization passes.
    // So, we try to add extra zero subscripts. We add subscripts for
    // instructions which access a single element, for example, in case of call
    // it is possible to pass a whole array as a parameter (without GEPs).
    auto ExtraZeroCount = Range.isElement() && !Range.isValid() ?
      ArrayInfo.getNumberOfDims() - Range.Subscripts.size() : 0;
    std::size_t DimIdx = 0, SubscriptIdx = 0;
    std::size_t DimIdxE = std::min(LastConstDim - 1, Range.Subscripts.size());
    SmallVector<const SCEV *, 4> NewSubscripts;
    for (;  DimIdx < DimIdxE; ++DimIdx) {
      auto *Subscript = Range.Subscripts[SubscriptIdx++];
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: subscript " << DimIdx << ": "
                        << *Subscript << "\n");
      for (std::size_t I = DimIdx + 1; I < LastConstDim; ++I) {
        auto Div = divide(*mSE, Subscript, ArrayInfo.getDimSize(I), false);
        if (Div.Remainder->isZero()) {
          Subscript = Div.Quotient;
        } else if (ExtraZeroCount > 0) {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add extra zero subscript\n");
          Subscript = mSE->getZero(mIndexTy);
          --ExtraZeroCount;
          // We insert zero subscript before current subscript, so we should
          // reprocess it.
          --SubscriptIdx;
          break;
        } else {
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize\n");
          Subscript = nullptr;
          Range.Traits &= ~Range.IsValid;
          break;
        }
      }
      if (!Subscript)
        break;
      NewSubscripts.push_back(Subscript);
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: set to " << *Subscript << "\n");
    }
    if (DimIdx < DimIdxE)
      continue;
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add " << ExtraZeroCount
                      << " extra zero subscripts to " << SubscriptIdx << "\n");
    for (std::size_t I = 0; I < ExtraZeroCount; ++I)
      NewSubscripts.push_back(mSE->getZero(mIndexTy));
    // Add subscripts for constant dimensions.
    for (auto EI = Range.Subscripts.size(); SubscriptIdx < EI; ++SubscriptIdx)
      NewSubscripts.push_back(Range.Subscripts[SubscriptIdx]);
    std::swap(Range.Subscripts, NewSubscripts);
    assert(Range.Subscripts.size() == ArrayInfo.getNumberOfDims() &&
      "Unable to delinearize element access!");
    Range.Traits |= Range.IsValid;
  }
}

void DelinearizationPass::fillArrayDimensionsSizes(Array &ArrayInfo) {
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute sizes of dimensions for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto NumberOfDims = ArrayInfo.getNumberOfDims();
  auto LastUnknownDim = NumberOfDims;
  if (NumberOfDims == 0) {
    for (auto &Range : ArrayInfo)
      if (Range.isElement() && Range.isValid())
        NumberOfDims = std::max(Range.Subscripts.size(), NumberOfDims);
    if (NumberOfDims == 0) {
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: no valid element found\n";
        dbgs() << "[DELINEARIZE]: unable to determine number of"
        " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
      return;
    }
    for (auto &Range : ArrayInfo) {
      if (Range.isElement() && Range.isValid() &&
          NumberOfDims != Range.Subscripts.size())
        Range.Traits &= ~Range.IsValid;
    }
    ArrayInfo.setNumberOfDims(NumberOfDims);
    ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = NumberOfDims - 1;
    LLVM_DEBUG(
      dbgs() << "[DELINEARIZE]: extract number of dimensions from subscripts: "
             << NumberOfDims << "\n");
  } else {
    auto LastConstDim = ArrayInfo.getNumberOfDims();
    for (auto I = ArrayInfo.getNumberOfDims(); I > 0; --I) {
      if (!dyn_cast_or_null<SCEVConstant>(ArrayInfo.getDimSize(I - 1)))
        break;
      LastConstDim = I - 1;
    }
    if (LastConstDim == 0) {
      ArrayInfo.setDelinearized();
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: all dimensions have constant sizes\n");
      return;
    }
    if (!ArrayInfo.isKnownDimSize(0))
      ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = LastConstDim - 1;
  }
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute non-constant dimension sizes\n");
  auto setUnknownDims = [this, &ArrayInfo](
      std::size_t LastUnknownIdx) {
    auto UnknownSize = mSE->getCouldNotCompute();
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: could not compute dimension size\n");
    ArrayInfo.setDimSize(LastUnknownIdx, UnknownSize);
    for (auto J : seq(decltype(LastUnknownIdx)(1), LastUnknownIdx)) {
      if (!ArrayInfo.isKnownDimSize(J))
        ArrayInfo.setDimSize(J, UnknownSize);
    }
  };
  auto *PrevDimSizesProduct = mSE->getConstant(mIndexTy, 1);
  auto DimIdx = LastUnknownDim;
  for (; DimIdx > 0; --DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process dimension " << DimIdx << "\n");
    const SCEV *DimSize;
    if (ArrayInfo.isKnownDimSize(DimIdx) > 0) {
      DimSize = ArrayInfo.getDimSize(DimIdx);
      if (DimSize->isZero()) {
        setUnknownDims(DimIdx);
        return;
      }
    } else {
      SmallVector<const SCEV *, 3> Expressions;
      for (auto &Range: ArrayInfo) {
        if (!Range.isElement() || !Range.isValid())
          continue;
        assert(Range.Subscripts.size() == NumberOfDims &&
          "Number of dimensions is inconsistent with number of subscripts!");
        for (auto J = DimIdx; J > 0; --J) {
          Expressions.push_back(Range.Subscripts[J - 1]);
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: use for GCD computation: ";
            Expressions.back()->dump());
        }
      }
      if (Expressions.empty()) {
        LLVM_DEBUG(
          dbgs() << "[DELINEARIZE]: no valid element found to compute GCD");
        setUnknownDims(DimIdx);
        return;
      }
      DimSize = findGCD(Expressions, *mSE, false);
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: GCD: "; DimSize->dump());
      if (isa<SCEVCouldNotCompute>(DimSize)) {
        setUnknownDims(DimIdx);
        return;
      }
      auto Div = divide(*mSE, DimSize, PrevDimSizesProduct, false);
      DimSize = Div.Quotient;
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        PrevDimSizesProduct->dump();
        dbgs() << "[DELINEARIZE]: quotient "; Div.Quotient->dump();
        dbgs() << "[DELINEARIZE]: remainder "; Div.Remainder->dump());
    }
    ArrayInfo.setDimSize(DimIdx, DimSize);
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: dimension size is "; DimSize->dump());
    DimSize = mSE->getTruncateOrZeroExtend(DimSize, mIndexTy);
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct, DimSize);
  }
  if (DimIdx == 0)
    ArrayInfo.setDelinearized();
}

void DelinearizationPass::findArrayDimesionsFromDbgInfo(Array &ArrayInfo) {
  SmallVector<DIMemoryLocation, 1> DILocs;
  auto DIM = findMetadata(ArrayInfo.getBase(), DILocs, mDT);
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
           << ArrayInfo.getBase()->getName() << " is ";
    dbgs() << (ArrayDims.size() + (IsFirstDimPointer ? 1 : 0)) << "\n");
  if (IsFirstDimPointer) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: first dimension is pointer\n");
    ArrayInfo.setNumberOfDims(ArrayDims.size() + 1);
  } else {
    ArrayInfo.setNumberOfDims(ArrayDims.size());
  }
  if (!ArrayDims)
    return;
  std::size_t PassPtrDim = IsFirstDimPointer ? 1 : 0;
  for (std::size_t DimIdx = 0; DimIdx < ArrayDims.size(); ++DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: size of " << DimIdx << " dimension is ");
    if (auto *DIDim = dyn_cast<DISubrange>(ArrayDims[DimIdx])) {
      auto DIDimCount = DIDim->getCount();
      if (DIDimCount.is<ConstantInt*>()) {
        auto Count = DIDimCount.get<ConstantInt *>()->getValue();
        if (Count.isNonNegative())
          ArrayInfo.setDimSize(DimIdx + PassPtrDim,
            mSE->getSCEV(DIDimCount.get<ConstantInt *>()));
        LLVM_DEBUG(dbgs() << Count << "\n");
      } else if (DIDimCount.is<DIVariable *>()) {
        auto DIVar = DIDimCount.get<DIVariable *>();
        if (auto V = MetadataAsValue::getIfExists(DIVar->getContext(), DIVar)) {
          SmallVector<DbgInfoIntrinsic *, 4> DbgInsts;
          findDbgUsers(DbgInsts, V);
          if (DbgInsts.size() == 1)
            ArrayInfo.setDimSize(DimIdx + PassPtrDim,
              mSE->getSCEV(DbgInsts.front()->getVariableLocation()));
        }
        LLVM_DEBUG(dbgs() << DIVar->getName() << "\n");
      } else {
        LLVM_DEBUG( dbgs() << "unknown\n");
      }
    }
  }
}

void DelinearizationPass::collectArrays(Function &F) {
  for (auto &I : instructions(F)) {
    auto processMemory = [this](Instruction &I, MemoryLocation Loc,
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
      auto ArrayItr = mDelinearizeInfo.getArrays().find_as(BasePtr);
      if (ArrayItr == mDelinearizeInfo.getArrays().end()) {
        ArrayItr = mDelinearizeInfo.getArrays().insert(new Array(BasePtr)).first;
        findArrayDimesionsFromDbgInfo(**ArrayItr);
      }
      auto NumberOfDims = (*ArrayItr)->getNumberOfDims();
      SmallVector<GEPOperator *, 4> GEPs;
      auto *GEP = dyn_cast<GEPOperator>(const_cast<Value *>(Loc.Ptr));
      while (GEP && (NumberOfDims == 0 || GEPs.size() < NumberOfDims)) {
        GEPs.push_back(GEP);
        GEP = dyn_cast<GEPOperator>(GEP->getPointerOperand());
      }
      SmallVector<Value *, 4> SubscriptValues;
      extractSubscriptsFromGEPs(GEPs.rbegin(), GEPs.rend(), SubscriptValues);
      auto &El = (*ArrayItr)->addElement(
        GEPs.empty() ? const_cast<Value *>(Loc.Ptr) : GEPs.back());
      // In some cases zero subscript is dropping out by optimization passes.
      // So, we try to add extra zero subscripts later.
      if (isa<LoadInst>(I) || isa<StoreInst>(I) ||
          isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        El.Traits |= Array::Element::IsElement;
        if (SubscriptValues.size() < NumberOfDims) {
          (*ArrayItr)->setRangeRef();
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
               << (El.isValid() ? "valid" : "invalid") << "\n";
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
      mDelinearizeInfo.getArrays().erase(CurrItr);
    }
  }
}

void DelinearizationPass::findSubscripts(Function &F) {
  collectArrays(F);
  for (auto *ArrayInfo : mDelinearizeInfo.getArrays()) {
    fillArrayDimensionsSizes(*ArrayInfo);
    if (ArrayInfo->isDelinearized()) {
      cleanSubscripts(*ArrayInfo);
    } else {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize "
                        << ArrayInfo->getBase()->getName() << "\n");
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
          Coef = SE.getZero(Info.first->getType());
          ConstTerm = Info.first;
        }
        raw_string_ostream CoefOS(CoefStr);
        Coef->print(CoefOS);
        CoefOS.flush();
        raw_string_ostream ConstTermOS(ConstTermStr);
        ConstTerm->print(ConstTermOS);
        ConstTermOS.flush();
      }
      Accesses.push_back(std::move(Subscripts));
    }
    RawInfo[RawDelinearizeInfo::Sizes].emplace(NameStr, std::move(DimSizes));
    RawInfo[RawDelinearizeInfo::Accesses].emplace(
      std::move(NameStr), std::move(Accesses));
  }
  return RawInfo;
}

