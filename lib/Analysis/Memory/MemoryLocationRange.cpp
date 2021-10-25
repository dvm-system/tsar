//===- MemoryLocationRange.cpp ---- Memory Location Range -------*- C++ -*-===//
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
// This file provides utility analysis objects describing memory locations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#ifndef NDEBUG
#include "tsar/Unparse/Utils.h"
#endif
#include "tsar/Support/SCEVUtils.h"
#include <bcl/Equation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Support/Debug.h>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "def-mem"

namespace {
typedef int64_t ColumnT;
typedef int64_t ValueT;
typedef MemoryLocationRange::Dimension Dimension;
typedef std::pair<llvm::Optional<MemoryLocationRange>, bool> IntersectionResult;
struct ColumnInfo {
  std::array<char, 3> Variables = {'X', 'Y', 'T'};

  template<typename T> T get(ColumnT Column) const { return 0; }
  ColumnT parameterColumn() { return 2; }
  bool isParameter(ColumnT Column) const { return Column > 1; }
  char name(ColumnT Column) const { return Variables[Column]; }
};

enum DimPairKind {
  BothVariable,
  OneStartOtherEndConst,
  BothStartConst,
  BothEndConst,
  OneConstOtherSemiconst,
  OneVariableOtherSemiconst,
  BothConst,
  OneConstOtherVariable,
  Unknown
};

#ifndef NDEBUG
std::string getKindAsString(DimPairKind Kind) {
  switch (Kind)
  {
  case BothVariable:
    return "BothVariable";
  case OneStartOtherEndConst:
    return "OneStartOtherEndConst";
  case BothStartConst:
    return "BothStartConst";
  case BothEndConst:
    return "BothEndConst";
  case OneConstOtherSemiconst:
    return "OneConstOtherSemiconst";
  case OneVariableOtherSemiconst:
    return "OneVariableOtherSemiconst";
  case BothConst:
    return "BothConst"; 
  case OneConstOtherVariable:
    return "OneConstOtherVariable";
  default:
    return "Unknown";
  }
}
#endif

/// Finds difference between dimensions D and I where I is a subset of D
/// and adds results to Res. Return `false` if Threshold is exceeded, `true`
/// otherwise.
bool difference(const Dimension &D, const Dimension &I,
                llvm::SmallVectorImpl<Dimension> &Res,
                llvm::ScalarEvolution *SE, std::size_t Threshold) {
  assert(SE && "ScalarEvolution must be specified!");
  assert(isa<SCEVConstant>(D.Step) && isa<SCEVConstant>(I.Step) &&
         "Dimension step must be constant.");
  assert(isa<SCEVConstant>(D.Start) && isa<SCEVConstant>(I.Start) &&
         "Dimension start must be constant.");
  if (*compareSCEVs(D.Start, I.Start, SE) < 0) {
    auto &Left = Res.emplace_back();
    Left.Step = D.Step;
    Left.Start = D.Start;
    Left.End = subtractSCEVAndCast(I.Start, D.Step, SE);
    assert(isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End) &&
           "Dimension bounds must be constant.");
    Left.DimSize = D.DimSize;
  }
  assert(isa<SCEVConstant>(D.End) && isa<SCEVConstant>(I.End) &&
         "Dimension end must be constant.");
  if (*compareSCEVs(D.End, I.End, SE) > 0) {
    auto &Right = Res.emplace_back();
    Right.Step = D.Step;
    Right.Start = addSCEVAndCast(I.End, D.Step, SE);
    Right.End = D.End;
    assert(isa<SCEVConstant>(Right.Start) && isa<SCEVConstant>(Right.End) &&
           "Dimension start must be constant.");
    Right.DimSize = D.DimSize;
  }
  if (*compareSCEVs(I.End, I.Start, SE) > 0) {
    // I.Step % D.Step is always 0 because I is a subset of D.
    auto Quotient = divide(*SE, I.Step, D.Step).Quotient;
    assert(isa<SCEVConstant>(Quotient) && "Quotient must be constant!");
    // I.Step / D.Step - 1
    auto RepeatNumber = subtractSCEVAndCast(
        Quotient, SE->getOne(Quotient->getType()), SE);
    auto RepeatNumberConst = dyn_cast<SCEVConstant>(RepeatNumber);
    assert(RepeatNumberConst && "Repeat Number must be constant.");
    if (RepeatNumberConst->getValue()->getZExtValue() > Threshold)
      return false;
    // I.TripCount = (I.End - I.Start) / Step + 1
    // CenterTripCount = I.TripCount - 1 = (I.End - I.Start) / Step
    auto CenterTripCountMinusOne = subtractSCEVAndCast(divide(*SE,
        subtractSCEVAndCast(I.End, I.Start, SE), I.Step).Quotient,
        SE->getOne(I.Step->getType()), SE);
    assert(isa<SCEVConstant>(CenterTripCountMinusOne) &&
           "Trip count must be constant!");
    for (auto J = 0; J < RepeatNumberConst->getValue()->getZExtValue(); ++J) {
      auto &Center = Res.emplace_back();
      // Center.Start = I.Start + D.Step * (J + 1);
      Center.Start = addSCEVAndCast(I.Start, SE->getMulExpr(
          D.Step, SE->getConstant(I.Start->getType(), J + 1)), SE);
      Center.Step = I.Step;
      // Center.End = Center.Start + (Center.TripCount - 1) * Center.Step
      Center.End = addSCEVAndCast(
          Center.Start, SE->getMulExpr(CenterTripCountMinusOne, Center.Step), SE);
      assert(isa<SCEVConstant>(Center.Start) && isa<SCEVConstant>(Center.End) &&
             "Dimension bounds must be constant!");
      assert(isa<SCEVConstant>(Center.Step) &&
             "Dimension step must be constant!");
      Center.DimSize = D.DimSize;
    }
  }
  return true;
}

#ifndef NDEBUG
void printSolutionInfo(llvm::raw_ostream &OS,
    const MemoryLocationRange &Int,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  OS << "[INTERSECT] Solution:\n";
  OS << "\tIntersection: ";
  printLocationSource(OS << "\t", Int);
  OS << "\n";
  if (LC && !LC->empty()) {
    OS << "\tLeft: ";
    for (auto &R : *LC) {
      printLocationSource(OS << "\t", R);
      OS << "\n";
    }
  }
  if (RC && !RC->empty()) {
    OS << "\tRight: ";
    for (auto &R : *RC) {
      printLocationSource(OS << "\t", R);
      OS << "\n";
    }
  }
}
#endif

void delinearize(const MemoryLocationRange &From, MemoryLocationRange &What) {
  typedef MemoryLocationRange::LocKind LocKind;
  assert(!(What.Kind & LocKind::Collapsed) &&
      "It is forbidden to delinearize collapsed location!");
  if (What.Kind & LocKind::NonCollapsable || !(From.Kind & LocKind::Collapsed))
    return;
  if (!What.LowerBound.hasValue() || !What.UpperBound.hasValue())
    return;
  auto Lower = What.LowerBound.getValue();
  auto Upper = What.UpperBound.getValue();
  if (Lower >= Upper)
    return;
  const auto DimN = From.DimList.size();
  if (DimN == 0)
    return;
  assert(From.UpperBound.hasValue() &&
      "UpperBound of a collapsed array location must have a value!");
  assert(From.LowerBound.getValue() == 0 &&
      "LowerBound of a collapsed array location must be 0!");
  auto ElemSize = From.UpperBound.getValue();
  std::vector<uint64_t> SizesInBytes(DimN + 1, 0);
  if (Lower % ElemSize != 0 || Upper % ElemSize != 0)
    return;
  SizesInBytes.back() = ElemSize;
  for (int64_t DimIdx = DimN - 1; DimIdx >= 0; --DimIdx) {
    SizesInBytes[DimIdx] = From.DimList[DimIdx].DimSize *
                           SizesInBytes[DimIdx + 1];
    assert(SizesInBytes[DimIdx] != 0 || DimIdx == 0 &&
        "Collapsed memory location should not contain "
        "dimensions of size 0, except for the 0th dimension.");
  }
  std::vector<uint64_t> LowerIdx(DimN, 0), UpperIdx(DimN, 0);
  for (std::size_t I = 0; I < DimN; ++I) {
    auto CurrSize = SizesInBytes[I], NextSize = SizesInBytes[I + 1];
    LowerIdx[I] = CurrSize > 0 ? (Lower % CurrSize) / NextSize :
                                  Lower / NextSize;
    UpperIdx[I] = CurrSize > 0 ?  ((Upper - ElemSize) % CurrSize) / NextSize :
                                  (Upper - ElemSize) / NextSize;
  }
  llvm::SmallVector<Dimension, 4> DimList(DimN);
  bool HasPartialCoverage = false;
  assert(From.SE && "ScalarEvolution must be specified!");
  auto SE = From.SE;
  for (int64_t I = DimN - 1; I >= 0; --I) {
    auto &CurrDim = DimList[I];
    auto &FromDim = From.DimList[I];
    CurrDim.Step = SE->getOne(FromDim.Step->getType());
    CurrDim.DimSize = FromDim.DimSize;
    if (HasPartialCoverage) {
      if (LowerIdx[I] != UpperIdx[I])
        return;
      CurrDim.Start = SE->getConstant(FromDim.Start->getType(), LowerIdx[I]);
      CurrDim.End = CurrDim.Start;
      assert(dyn_cast<SCEVConstant>(CurrDim.Start) &&
             dyn_cast<SCEVConstant>(CurrDim.End) &&
             "Dimension bounds must be constant!");
    } else {
      CurrDim.Start = SE->getConstant(FromDim.Start->getType(), LowerIdx[I]);
      CurrDim.End = SE->getConstant(FromDim.End->getType(), UpperIdx[I]);
      assert(dyn_cast<SCEVConstant>(CurrDim.Start) &&
             dyn_cast<SCEVConstant>(CurrDim.End) &&
             "Dimension bounds must be constant!");
      if (LowerIdx[I] != 0 || UpperIdx[I] + 1 != FromDim.DimSize)
        HasPartialCoverage = true;
    }
  }
  What.LowerBound = 0;
  What.UpperBound = ElemSize;
  What.DimList = std::move(DimList);
  What.Kind = LocKind::Collapsed | (What.Kind & LocKind::Hint);
  What.SE = SE;
  What.AM = From.AM;
}

llvm::Optional<MemoryLocationRange> intersectScalar(
    MemoryLocationRange LHS,
    MemoryLocationRange RHS,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  typedef MemoryLocationRange::LocKind LocKind;
  if (LHS.Ptr != RHS.Ptr)
    return llvm::None;
  assert(!(LHS.Kind & LocKind::Collapsed) && !(RHS.Kind & LocKind::Collapsed) &&
      "It is forbidden to calculate an intersection between non-scalar "
      "variables!");
  if (!LHS.LowerBound.hasValue() || !LHS.UpperBound.hasValue() ||
      !RHS.LowerBound.hasValue() || !RHS.UpperBound.hasValue()) {
    if ((LHS.UpperBound.hasValue() && RHS.LowerBound.hasValue() &&
         LHS.UpperBound.getValue() <= RHS.LowerBound.getValue()) ||
        (LHS.LowerBound.hasValue() && RHS.UpperBound.hasValue() &&
         LHS.LowerBound.getValue() >= RHS.UpperBound.getValue())) 
      return llvm::None;
    return MemoryLocationRange();
  }
  if (LHS.UpperBound.getValue() > RHS.LowerBound.getValue() &&
      LHS.LowerBound.getValue() < RHS.UpperBound.getValue()) {
    MemoryLocationRange Int(LHS);
    Int.Kind = LocKind::Default;
    Int.LowerBound = std::max(LHS.LowerBound.getValue(),
                              RHS.LowerBound.getValue());
    Int.UpperBound = std::min(LHS.UpperBound.getValue(),
                              RHS.UpperBound.getValue());
    if (LC) {
      if (LHS.LowerBound.getValue() < Int.LowerBound.getValue())
        LC->emplace_back(LHS).UpperBound = Int.LowerBound.getValue();
      if (LHS.UpperBound.getValue() > Int.UpperBound.getValue())
        LC->emplace_back(LHS).LowerBound = Int.UpperBound.getValue();
    }
    if (RC) {
      if (RHS.LowerBound.getValue() < Int.LowerBound.getValue())
        RC->emplace_back(RHS).UpperBound = Int.LowerBound.getValue();
      if (RHS.UpperBound.getValue() > Int.UpperBound.getValue())
        RC->emplace_back(RHS).LowerBound = Int.UpperBound.getValue();
    }
    return Int;
  }
  return llvm::None;
}

DimPairKind getDimPairKind(const Dimension &LHS, const Dimension &RHS) {
  LLVM_DEBUG(
    dbgs() << "[INTERSECT] Check the pair:\n";
    LHS.print(dbgs() << "\t"); dbgs() << "\n";
    RHS.print(dbgs() << "\t"); dbgs() << "\n";
  );
  auto LStartConst = isa<SCEVConstant>(LHS.Start);
  auto LEndConst = isa<SCEVConstant>(LHS.End);
  auto RStartConst = isa<SCEVConstant>(RHS.Start);
  auto REndConst = isa<SCEVConstant>(RHS.End);
  if (!LStartConst && !LEndConst && !RStartConst && !REndConst)
    return DimPairKind::BothVariable;
  if ((LStartConst && LEndConst && !RStartConst && !REndConst) ||
      (!LStartConst && !LEndConst && RStartConst && REndConst))
    return DimPairKind::OneConstOtherVariable;
  if ((LStartConst && !LEndConst && !RStartConst && REndConst) ||
     (RStartConst && !REndConst && !LStartConst && LEndConst))
    return DimPairKind::OneStartOtherEndConst;
  if (LStartConst && RStartConst && !LEndConst && !REndConst)
    return DimPairKind::BothStartConst;
  if (!LStartConst && !RStartConst && LEndConst && REndConst)
    return DimPairKind::BothEndConst;
  if ((LStartConst && LEndConst && (RStartConst ^ REndConst)) ||
      (RStartConst && REndConst && (LStartConst ^ LEndConst)))
    return DimPairKind::OneConstOtherSemiconst;
  if ((!LStartConst && !LEndConst && (RStartConst ^ REndConst)) ||
      (!RStartConst && !REndConst && (LStartConst ^ LEndConst)))
    return DimPairKind::OneVariableOtherSemiconst;
  if (LStartConst && LEndConst && RStartConst && REndConst)
    return DimPairKind::BothConst;
  return DimPairKind::Unknown;
}

struct IntersectVarInfo {
  IntersectionResult UnknownIntersection;
  IntersectionResult EmptyIntersection;
  IntersectionResult TrueIntersection;
  llvm::Optional<int64_t> CmpStart;
  llvm::Optional<int64_t> CmpEnd;
  llvm::Optional<int64_t> CmpLeftEndRightStart;
  llvm::Optional<int64_t> CmpLeftStartRightEnd;
  bool IsValidStep;
};

/// We can compare the bounds of the segments only if they can be represented
/// as f(V) = V + C, where V is a variable expression and C is an integer
/// constant. For the expression S representing one bound of the segment
/// this function returns a pair consisting of an llvm::Value* of V and an
/// integer value of C. If S does not have the form f(V), this function
/// returns llvm::None.
llvm::Optional<std::pair<Value *, int64_t>>
parseBoundExpression(const llvm::SCEV *S) {
  if (auto *Unknown = dyn_cast<SCEVUnknown>(S))
    return std::make_pair(Unknown->getValue(), int64_t(0));
  auto *NAry = dyn_cast<SCEVNAryExpr>(S);
  if (!NAry || NAry->getSCEVType() != llvm::SCEVTypes::scAddExpr ||
      NAry->getNumOperands() != 2)
    return llvm::None;
  auto *S1 = NAry->getOperand(0);
  auto *S2 = NAry->getOperand(1);
  auto T1 = static_cast<SCEVTypes>(S1->getSCEVType());
  auto T2 = static_cast<SCEVTypes>(S2->getSCEVType());
  int64_t Constant = 0;
  Value *Variable = nullptr;
  if (T1 == SCEVTypes::scConstant && T2 == SCEVTypes::scUnknown) {
    Constant = cast<SCEVConstant>(S1)->getAPInt().getSExtValue();
    Variable = cast<SCEVUnknown>(S2)->getValue();
  } else if (T2 == SCEVTypes::scConstant && T1 == SCEVTypes::scUnknown) {
    Constant = cast<SCEVConstant>(S2)->getAPInt().getSExtValue();
    Variable = cast<SCEVUnknown>(S1)->getValue();
  } else {
    return llvm::None;
  }
  assert(Variable && "Value must not be nullptr!");
  return std::make_pair(Variable, Constant);
}

inline std::function<Dimension& (llvm::SmallVectorImpl<MemoryLocationRange> *)>
getGrowFunction(const MemoryLocationRange &LeftRange, std::size_t DimIdx) {
  assert(DimIdx < LeftRange.DimList.size() &&
      "DimIdx must match the size of LeftRange.DimList!");
  auto Lambda = [&LeftRange, DimIdx](
    llvm::SmallVectorImpl<MemoryLocationRange> *C) -> Dimension & {
    return C->emplace_back(LeftRange).DimList[DimIdx];
  };
  return Lambda;
}

IntersectionResult processBothVariable(const MemoryLocationRange &LeftRange,
    std::size_t DimIdx, const Dimension &Right, const IntersectVarInfo &Info,
    Dimension &Intersection, llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  auto SE = LeftRange.SE;
  auto &CmpStart = Info.CmpStart;
  auto &CmpEnd = Info.CmpEnd;
  auto &CmpLSRE = Info.CmpLeftStartRightEnd;
  auto &CmpLERS = Info.CmpLeftEndRightStart;
  if (CmpLERS && *CmpLERS < 0 || CmpLSRE && *CmpLSRE > 0)
    return Info.EmptyIntersection;
  if (!CmpStart || !CmpEnd || !Info.IsValidStep)
    return Info.UnknownIntersection;
  if (CmpLERS && CmpLSRE && *CmpLERS >= 0 && *CmpLSRE <= 0) {
    // The left dimension overlaps the right.
    Intersection.Start = (*CmpStart) > 0 ? Left.Start : Right.Start;
    Intersection.End = (*CmpEnd) < 0 ? Left.End : Right.End;
    if (LC) {
      if (*CmpStart < 0) {
        auto &Dim = Grow(LC);
        Dim.Start = Left.Start;
        Dim.End = subtractOneFromSCEV(Right.Start, SE);
      }
      if (*CmpEnd > 0) {
        auto &Dim = Grow(LC);
        Dim.Start = addOneToSCEV(Right.End, SE);
        Dim.End = Left.End;
      }
    }
    if (RC) {
      if (*CmpStart > 0) {
        auto &Dim = Grow(RC);
        Dim.Start = Right.Start;
        Dim.End = subtractOneFromSCEV(Left.Start, SE);
      }
      if (*CmpEnd < 0) {
        auto &Dim = Grow(RC);
        Dim.Start = addOneToSCEV(Left.End, SE);
        Dim.End = Right.End;
      }
    }
  } else
    return Info.UnknownIntersection;
  return Info.TrueIntersection;
}

IntersectionResult processOneStartOtherEndConst(
    const MemoryLocationRange &LeftRange, std::size_t DimIdx,
    const Dimension &Right, Dimension &Intersection,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC, IntersectVarInfo &Info) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  assert((isa<SCEVConstant>(Left.Start) && !isa<SCEVConstant>(Left.End) &&
         !isa<SCEVConstant>(Right.Start) && isa<SCEVConstant>(Right.End)) ||
         (isa<SCEVConstant>(Right.Start) && !isa<SCEVConstant>(Right.End) &&
         !isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End)));
  auto SE = LeftRange.SE;
  auto *AM = LeftRange.AM;
  // Let the First dimension be a dimension whose Start is constant and End
  // is variable. We will denote the segments as follows:
  // First = [m, N], Second = [P, q], where lowercase letters mean constants,
  // and capital letters mean variables.
  auto *First { &Left }, *Second { &Right };
  auto *FC { LC }, *SC { RC };
  auto &Cmpmq { Info.CmpLeftStartRightEnd },
       &CmpNP { Info.CmpLeftEndRightStart };
  if (!isa<SCEVConstant>(Left.Start)) {
    std::swap(First, Second);
    std::swap(FC, SC);
    std::swap(Cmpmq, CmpNP);
  }
  assert(Cmpmq && "Constant bounds must be comparable!");
  if (*Cmpmq > 0)
    return Info.EmptyIntersection;
  if (!CmpNP)
    return Info.UnknownIntersection;
  if (*CmpNP < 0)
    return Info.EmptyIntersection;
  if (!Info.IsValidStep)
    return Info.UnknownIntersection;
  // N >= P
  auto *M = First->Start, *N = First->End;
  auto *P = Second->Start, *Q = Second->End;
  auto BN = parseBoundExpression(N);
  auto BP = parseBoundExpression(P);
  if (!BP || !BN || !AM)
    return Info.UnknownIntersection;
  auto BPItr = AM->find(BP->first);
  auto BNItr = AM->find(BN->first);
  if (BPItr == AM->end() || BNItr == AM->end())
    return Info.UnknownIntersection;
  auto &BoundsP = BPItr->second;
  auto &BoundsN = BNItr->second;
  auto MInt = cast<SCEVConstant>(M)->getAPInt().getSExtValue();
  auto QInt = cast<SCEVConstant>(Q)->getAPInt().getSExtValue();
  if (!BoundsN.Lower || !BoundsN.Upper || !BoundsP.Lower || !BoundsP.Upper)
    return Info.UnknownIntersection;
  if (MInt < *BoundsP.Lower) {
    if (QInt > *BoundsN.Upper) {
      Intersection.Start = P;
      Intersection.End = N;
      if (FC) {
        // [m, P-1]
        auto &Dim = Grow(FC);
        Dim.Start = M;
        Dim.End = subtractOneFromSCEV(P, SE);
      }
      if (SC) {
        // [N+1, q]
        auto &Dim = Grow(SC);
        Dim.Start = addOneToSCEV(N, SE);
        Dim.End = Q;
      }
    } else if (QInt < *BoundsN.Lower) {
      Intersection = *Second;
      if (FC) {
        // [m, P-1], [q+1, N]
        auto &Dim1 = Grow(FC);
        Dim1.Start = M;
        Dim1.End = subtractOneFromSCEV(P, SE);
        auto &Dim2 = Grow(FC);
        Dim2.Start = addOneToSCEV(Q, SE);
        Dim2.End = N;
      }
    } else {
      return Info.UnknownIntersection;
    }
  } else if (MInt <= *BoundsP.Lower && *BoundsN.Lower >= QInt) {
    if (FC || SC)
      return Info.UnknownIntersection;
    Intersection = *Second;
  } else if (MInt > *BoundsP.Upper) {
    if (QInt < BoundsN.Lower) {
      Intersection.Start = M;
      Intersection.End = Q;
      if (FC) {
        // [P, m-1]
        auto &Dim = Grow(FC);
        Dim.Start = P;
        Dim.End = subtractOneFromSCEV(M, SE);
      }
      if (SC) {
        // [q+1, N]
        auto &Dim = Grow(SC);
        Dim.Start = addOneToSCEV(Q, SE);
        Dim.End = N;
      }
    } else if (QInt > BoundsN.Upper) {
      Intersection = *First;
      if (SC) {
        // [P, m-1], [N+1, q]
        auto &Dim1 = Grow(SC);
        Dim1.Start = P;
        Dim1.End = subtractOneFromSCEV(M, SE);
        auto &Dim2 = Grow(SC);
        Dim2.Start = subtractOneFromSCEV(N, SE);
        Dim2.End = Q;
      }
    } else {
      return Info.UnknownIntersection;
    }
  } else if (MInt >= *BoundsP.Upper && BoundsN.Upper <= QInt) {
    if (FC || SC)
      return Info.UnknownIntersection;
    Intersection = *First;
  } else {
    return Info.UnknownIntersection;
  }
  return Info.TrueIntersection;
}

IntersectionResult processBothStartConst(const MemoryLocationRange &LeftRange,
    std::size_t DimIdx, const Dimension &Right, Dimension &Intersection,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC, IntersectVarInfo &Info) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  assert(isa<SCEVConstant>(Left.Start) && !isa<SCEVConstant>(Left.End) &&
         isa<SCEVConstant>(Right.Start) && !isa<SCEVConstant>(Right.End));
  auto SE = LeftRange.SE;
  auto *AM = LeftRange.AM;
  // Let the First dimension be a dimension whose Start is less than the Start
  // of the Second dimension. We will denote the segments as follows:
  // First = [m, N], Second = [p, Q], where lowercase letters mean constants,
  // and capital letters mean variables.
  auto *First { &Left }, *Second { &Right };
  auto *FC { LC }, *SC { RC };
  auto CmpNQ { Info.CmpEnd };
  if (cast<SCEVConstant>(Left.Start)->getAPInt().getSExtValue() >
      cast<SCEVConstant>(Right.Start)->getAPInt().getSExtValue()) {
    std::swap(First, Second);
    std::swap(FC, SC);
    if (CmpNQ)
      *CmpNQ = -(*CmpNQ);
  }
  auto *M = First->Start, *N = First->End;
  auto *P = Second->Start, *Q = Second->End;
  auto MInt = cast<SCEVConstant>(M)->getAPInt().getSExtValue();
  auto PInt = cast<SCEVConstant>(P)->getAPInt().getSExtValue();
  if (!CmpNQ)
    return Info.UnknownIntersection;
  if (MInt == PInt) {
    // [m, min(N, Q)]
    Intersection.Start = M;
    Intersection.End = *CmpNQ > 0 ? Q : N;
    if (FC && *CmpNQ > 0) {
      // [Q+1, N]
      auto &Dim = Grow(FC);
      Dim.Start = addOneToSCEV(Q, SE);
      Dim.End = N;
    }
    if (SC && *CmpNQ < 0) {
      // [N+1, Q]
      auto &Dim = Grow(SC);
      Dim.Start = addOneToSCEV(N, SE);
      Dim.End = Q;
    }
  } else {
    if (N == Q) {
      // [max(m, p), N]
      Intersection.Start = MInt > PInt ? M : P;
      Intersection.End = N;
      if (FC && MInt < PInt) {
        // [m, p-1]
        auto &Dim = Grow(FC);
        Dim.Start = M;
        Dim.End = subtractOneFromSCEV(P, SE);
      }
      if (SC && MInt > PInt)
        llvm_unreachable("M must be less than or equal to P!");
    } else {
      auto BN = parseBoundExpression(N);
      auto BQ = parseBoundExpression(Q);
      if (!BN || !BQ || !AM)
        return Info.UnknownIntersection;
      auto BNItr = AM->find(BN->first);
      auto BQItr = AM->find(BQ->first);
      if (BNItr == AM->end() || BQItr == AM->end())
        return Info.UnknownIntersection;
      auto &BoundsN = BNItr->second;
      auto &BoundsQ = BQItr->second;
      if (BoundsN.Upper && PInt > *BoundsN.Upper) {
        return Info.EmptyIntersection;
      } else if (BoundsN.Lower && PInt <= *BoundsN.Lower) {
        // [max(m, p), min(N, Q)]
        Intersection.Start = MInt > PInt ? M : P;
        Intersection.End = *CmpNQ < 0 ? N : Q;
        if (FC && MInt < PInt) {
          // [m, p-1]
          auto &Dim = Grow(FC);
          Dim.Start = M;
          Dim.End = subtractOneFromSCEV(P, SE);
        }
        if (SC && *CmpNQ < 0) {
          // [N+1, Q]
          auto &Dim = Grow(SC);
          Dim.Start = addOneToSCEV(N, SE);
          Dim.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    }
  }
  return Info.TrueIntersection;
}

IntersectionResult processBothEndConst(const MemoryLocationRange &LeftRange,
    std::size_t DimIdx, const Dimension &Right, Dimension &Intersection,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC, IntersectVarInfo &Info) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  assert(!isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End) &&
         !isa<SCEVConstant>(Right.Start) && isa<SCEVConstant>(Right.End));
  auto SE = LeftRange.SE;
  auto *AM = LeftRange.AM;
  // Let the First dimension be a dimension whose End is less than the End
  // of the Second dimension. We will denote the segments as follows:
  // First = [M, n], Second = [P, q], where lowercase letters mean constants,
  // and capital letters mean variables.
  auto *First { &Left }, *Second { &Right };
  auto *FC { LC }, *SC { RC };
  auto CmpMP { Info.CmpEnd };
  if (cast<SCEVConstant>(Left.End)->getAPInt().getSExtValue() >
      cast<SCEVConstant>(Right.End)->getAPInt().getSExtValue()) {
    std::swap(First, Second);
    std::swap(FC, SC);
    if (CmpMP)
      *CmpMP = -(*CmpMP);
  }
  auto *M = First->Start, *N = First->End;
  auto *P = Second->Start, *Q = Second->End;
  auto NInt = cast<SCEVConstant>(N)->getAPInt().getSExtValue();
  auto QInt = cast<SCEVConstant>(Q)->getAPInt().getSExtValue();
  if (!CmpMP)
    return Info.UnknownIntersection;
  if (NInt == QInt) {
    // [max(M, P), n]
    Intersection.Start = *CmpMP > 0 ? M : P;
    Intersection.End = N;
    if (FC && *CmpMP < 0) {
      // [M, P-1]
      auto &Dim = Grow(FC);
      Dim.Start = M;
      Dim.End = subtractOneFromSCEV(P, SE);
    }
    if (SC && *CmpMP > 0) {
      // [P+1, M]
      auto &Dim = Grow(SC);
      Dim.Start = addOneToSCEV(P, SE);
      Dim.End = M;
    }
  } else {
    if (M == P) {
      // [M, min(n, q)]
      Intersection.Start = M;
      Intersection.End = NInt < QInt ? N : Q;
      if (FC && NInt > QInt)
        llvm_unreachable("N must be less than or equal to Q!");
      if (SC && QInt > NInt) {
        // [n+1, q]
        auto &Dim = Grow(SC);
        Dim.Start = addOneToSCEV(N, SE);
        Dim.End = Q;
      }
    } else {
      auto BM = parseBoundExpression(M);
      auto BP = parseBoundExpression(P);
      if (!BM || !BP || !AM)
        return Info.UnknownIntersection;
      auto BMItr = AM->find(BM->first);
      auto BPItr = AM->find(BP->first);
      if (BMItr == AM->end() || BPItr == AM->end())
        return Info.UnknownIntersection;
      auto &BoundsM = BMItr->second;
      auto &BoundsP = BPItr->second;
      if (BoundsM.Lower && QInt < *BoundsM.Lower) {
        return Info.EmptyIntersection;
      } else if (BoundsM.Upper && QInt >= *BoundsM.Upper) {
        // [max(M, P), min(n, q)]
        Intersection.Start = *CmpMP > 0 ? M : P;
        Intersection.End = NInt < QInt ? N : Q;
        if (FC && *CmpMP < 0) {
          // [M, P-1]
          auto &Dim = Grow(FC);
          Dim.Start = M;
          Dim.End = subtractOneFromSCEV(P, SE);
        }
        if (SC && NInt < QInt) {
          // [N+1, Q]
          auto &Dim = Grow(SC);
          Dim.Start = addOneToSCEV(N, SE);
          Dim.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    }
  }
  return Info.TrueIntersection;
}

IntersectionResult processOneConstOtherSemiconst(
    const MemoryLocationRange &LeftRange, std::size_t DimIdx,
    const Dimension &Right, Dimension &Intersection,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC, IntersectVarInfo &Info) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  assert((isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End) &&
         (isa<SCEVConstant>(Right.Start) ^ isa<SCEVConstant>(Right.End))) ||
         (isa<SCEVConstant>(Right.Start) && isa<SCEVConstant>(Right.End) &&
         (isa<SCEVConstant>(Left.Start) ^ isa<SCEVConstant>(Left.End))));
  auto SE = LeftRange.SE;
  auto *AM = LeftRange.AM;
  // Let the First dimension be a dimension whose Start and End are constant.
  // We will denote the segments as follows: First = [m, n], Second = [p, Q]
  // (or [P, q]), where lowercase letters mean constants, and capital letters
  // mean variables.
  auto *First { &Left }, *Second { &Right };
  auto *FC { LC }, *SC { RC };
  if (isa<SCEVConstant>(Left.Start) ^ isa<SCEVConstant>(Left.End)) {
    std::swap(First, Second);
    std::swap(FC, SC);
  }
  auto *M = First->Start, *N = First->End;
  auto *P = Second->Start, *Q = Second->End;
  auto MInt = cast<SCEVConstant>(M)->getAPInt().getSExtValue();
  auto NInt = cast<SCEVConstant>(N)->getAPInt().getSExtValue();
  auto BP = parseBoundExpression(P);
  auto BQ = parseBoundExpression(Q);
  if (isa<SCEVConstant>(P)) {
    auto PInt = cast<SCEVConstant>(P)->getAPInt().getSExtValue();
    if (PInt > NInt)
      return Info.EmptyIntersection;
    if (!BQ || !AM)
      return Info.UnknownIntersection;
    auto BQItr = AM->find(BQ->first);
    if (BQItr == AM->end())
      return Info.UnknownIntersection;
    auto &BoundsQ = BQItr->second;
    if (BoundsQ.Lower && NInt < *BoundsQ.Lower) {
      // [max(m, p), n]
      Intersection.Start = MInt > PInt ? M : P;
      Intersection.End = N;
      if (FC && MInt < PInt) {
        // [m, p-1]
        auto &Dim = Grow(FC);
        Dim.Start = M;
        Dim.End = subtractOneFromSCEV(P, SE);
      }
      if (SC) {
        // [n+1, Q]
        auto &Dim1 = Grow(SC);
        Dim1.Start = addOneToSCEV(N, SE);
        Dim1.End = Q;
        if (PInt < MInt) {
          // [p, m-1]
          auto &Dim2 = Grow(SC);
          Dim2.Start = P;
          Dim2.End = subtractOneFromSCEV(M, SE);
        }
      }
    } else if (BoundsQ.Upper) {
      if (*BoundsQ.Upper < MInt)
        return Info.EmptyIntersection;
      if (NInt > BoundsQ.Upper) {
        // [max(m, p), Q]
        Intersection.Start = MInt > PInt ? M : P;
        Intersection.End = Q;
        if (FC) {
          // [Q+1, n]
          auto &Dim1 = Grow(FC);
          Dim1.Start = addOneToSCEV(Q, SE);
          Dim1.End = N;
          if (MInt < PInt) {
            // [m, p-1]
            auto &Dim2 = Grow(FC);
            Dim2.Start = M;
            Dim2.End = subtractOneFromSCEV(P, SE);
          }
        }
        if (SC && PInt < MInt) {
          // [p, m-1]
          auto &Dim = Grow(SC);
          Dim.Start = P;
          Dim.End = subtractOneFromSCEV(M, SE);
        }
      } else {
        return Info.UnknownIntersection;
      }
    } else {
      return Info.UnknownIntersection;
    }
  } else {
    auto QInt = cast<SCEVConstant>(Q)->getAPInt().getSExtValue();
    if (MInt > QInt)
      return Info.EmptyIntersection;
    if (!BP || !AM)
      return Info.UnknownIntersection;
    auto BPItr = AM->find(BP->first);
    if (BPItr == AM->end())
      return Info.UnknownIntersection;
    auto &BoundsP = BPItr->second;
    if (BoundsP.Upper && MInt > *BoundsP.Upper) {
      // [m, min(n, q)]
      Intersection.Start = M;
      Intersection.End = NInt < QInt ? N : Q;
      if (FC && NInt > QInt) {
        // [q+1, n]
        auto &Dim = Grow(FC);
        Dim.Start = addOneToSCEV(Q, SE);
        Dim.End = N;
      }
      if (SC) {
        // [P, m-1]
        auto &Dim1 = Grow(SC);
        Dim1.Start = P;
        Dim1.End = subtractOneFromSCEV(M, SE);
        if (QInt > NInt) {
          // [n+1, q]
          auto &Dim2 = Grow(SC);
          Dim2.Start = addOneToSCEV(N, SE);
          Dim2.End = Q;
        }
      }
    } else if (BoundsP.Lower) {
      if (*BoundsP.Lower > NInt)
        return Info.EmptyIntersection;
      if (MInt < BoundsP.Lower) {
        // [P, min(n, q)]
        Intersection.Start = P;
        Intersection.End = NInt < QInt ? N : Q;
        if (FC) {
          // [m, P-1]
          auto &Dim1 = Grow(FC);
          Dim1.Start = M;
          Dim1.End = subtractOneFromSCEV(P, SE);
          if (NInt > QInt) {
            // [q+1, n]
            auto &Dim2 = Grow(FC);
            Dim2.Start = addOneToSCEV(Q, SE);
            Dim2.End = N;
          }
        }
        if (SC && QInt > NInt) {
          // [n+1, q]
          auto &Dim = Grow(SC);
          Dim.Start = addOneToSCEV(N, SE);
          Dim.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    } else {
      return Info.UnknownIntersection;
    }
  }
  return Info.TrueIntersection;
}

IntersectionResult processOneVariableOtherSemiconst(
    const MemoryLocationRange &LeftRange, std::size_t DimIdx,
    const Dimension &Right, Dimension &Intersection,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC, IntersectVarInfo &Info) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  assert((!isa<SCEVConstant>(Left.Start) && !isa<SCEVConstant>(Left.End) &&
         (isa<SCEVConstant>(Right.Start) ^ isa<SCEVConstant>(Right.End))) ||
         (!isa<SCEVConstant>(Right.Start) && !isa<SCEVConstant>(Right.End) &&
         (isa<SCEVConstant>(Left.Start) ^ isa<SCEVConstant>(Left.End))));
  auto SE = LeftRange.SE;
  auto *AM = LeftRange.AM;
  // Let the First dimension be a dimension whose Start and End are variable.
  // We will denote the segments as follows: First = [M, N], Second = [p, Q]
  // (or [P, q]), where lowercase letters mean constants, and capital letters
  // mean variables.
  auto *First { &Left }, *Second { &Right };
  auto *FC { LC }, *SC { RC };
  auto &CmpMQ = Info.CmpLeftStartRightEnd;
  auto &CmpNQ = Info.CmpEnd;
  auto &CmpMP = Info.CmpStart;
  auto &CmpNP = Info.CmpLeftEndRightStart;
  if (isa<SCEVConstant>(Left.Start) || isa<SCEVConstant>(Left.End)) {
    std::swap(First, Second);
    std::swap(FC, SC);
    if (CmpMQ)
      *CmpMQ = -(*CmpMQ);
    if (CmpNQ)
      *CmpNQ = -(*CmpNQ);
    if (CmpMP)
      *CmpMP = -(*CmpMP);
    if (CmpNP)
      *CmpNP = -(*CmpNP);
  }
  auto *M = First->Start, *N = First->End;
  auto *P = Second->Start, *Q = Second->End;
  auto BM = parseBoundExpression(M);
  auto BN = parseBoundExpression(N);
  if (!BM || !BN || !AM)
      return Info.UnknownIntersection;
  auto BMItr = AM->find(BM->first);
  auto BNItr = AM->find(BN->first);
  if (BMItr == AM->end() || BNItr == AM->end())
    return Info.UnknownIntersection;
  auto &BoundsM = BMItr->second;
  auto &BoundsN = BNItr->second;
  if (isa<SCEVConstant>(P)) {
    auto PInt = cast<SCEVConstant>(P)->getAPInt().getSExtValue();
    auto BQ = parseBoundExpression(Q);
    if (!BQ)
      return Info.UnknownIntersection;
    auto BQItr = AM->find(BQ->first);
    if (BQItr == AM->end())
      return Info.UnknownIntersection;
    auto &BoundsQ = BQItr->second;
    if (BoundsN.Upper && *BoundsN.Upper < PInt ||
        BoundsQ.Upper && BoundsM.Lower && *BoundsQ.Upper < *BoundsM.Lower ||
        CmpMQ && *CmpMQ > 0)
      return Info.EmptyIntersection;
    if (BoundsM.Upper && PInt > *BoundsM.Upper) {
      if (BoundsQ.Upper && BoundsN.Lower && *BoundsQ.Upper < *BoundsN.Lower ||
          CmpNQ && *CmpNQ > 0) {
        // [p, Q]
        Intersection.Start = P;
        Intersection.End = Q;
        if (FC) {
          // [M, p-1]
          auto &Dim1 = Grow(FC);
          Dim1.Start = M;
          Dim1.End = subtractOneFromSCEV(P, SE);
          // [Q+1, N]
          auto &Dim2 = Grow(FC);
          Dim2.Start = addOneToSCEV(Q, SE);
          Dim2.End = N;
        }
      } else if (BoundsN.Upper && BoundsQ.Lower &&
                 *BoundsN.Upper < *BoundsQ.Lower ||
                 CmpNQ && *CmpNQ < 0) {
        // [p, N]
        Intersection.Start = P;
        Intersection.End = N;
        if (FC) {
          // [M, p-1]
          auto &Dim = Grow(FC);
          Dim.Start = M;
          Dim.End = subtractOneFromSCEV(P, SE);
        }
        if (SC) {
          // [N+1, Q]
          auto &Dim = Grow(SC);
          Dim.Start = addOneToSCEV(N, SE);
          Dim.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    } else if (BoundsM.Lower && PInt < *BoundsM.Lower) {
      if (BoundsQ.Upper && BoundsN.Lower && *BoundsQ.Upper < *BoundsN.Lower ||
          CmpNQ && *CmpNQ > 0) {
        // [M, Q]
        Intersection.Start = M;
        Intersection.End = Q;
        if (FC) {
          // [Q+1, N]
          auto &Dim = Grow(FC);
          Dim.Start = addOneToSCEV(Q, SE);
          Dim.End = N;
        }
        if (SC) {
          // [p, M-1]
          auto &Dim = Grow(SC);
          Dim.Start = P;
          Dim.End = subtractOneFromSCEV(M, SE);
        }
      } else if (BoundsN.Upper && BoundsQ.Lower &&
                 *BoundsN.Upper < *BoundsQ.Lower ||
                 CmpNQ && *CmpNQ < 0) {
        Intersection.Start = M;
        Intersection.End = N;
        if (SC) {
          // [p, M-1]
          auto &Dim1 = Grow(SC);
          Dim1.Start = P;
          Dim1.End = subtractOneFromSCEV(M, SE);
          // [N+1, Q]
          auto &Dim2 = Grow(SC);
          Dim2.Start = addOneToSCEV(N, SE);
          Dim2.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    } else {
      return Info.UnknownIntersection;
    }
  } else {
    auto QInt = cast<SCEVConstant>(Q)->getAPInt().getSExtValue();
    auto BP = parseBoundExpression(P);
    if (!BP)
      return Info.UnknownIntersection;
    auto BPItr = AM->find(BP->first);
    if (BPItr == AM->end())
      return Info.UnknownIntersection;
    auto &BoundsP = BPItr->second;
    if (BoundsM.Lower && *BoundsM.Lower > QInt ||
        BoundsP.Lower && BoundsN.Upper && *BoundsP.Lower > *BoundsN.Upper ||
        CmpNP && *CmpNP < 0)
      return Info.EmptyIntersection;
    if (BoundsM.Upper && BoundsP.Lower && *BoundsP.Lower > *BoundsM.Upper ||
        CmpMP && *CmpMP < 0) {
      if (BoundsN.Lower && QInt < *BoundsN.Lower) {
        // [p, Q]
        Intersection.Start = P;
        Intersection.End = Q;
        if (FC) {
          // [M, p-1]
          auto &Dim1 = Grow(FC);
          Dim1.Start = M;
          Dim1.End = subtractOneFromSCEV(P, SE);
          // [Q+1, N]
          auto &Dim2 = Grow(FC);
          Dim2.Start = addOneToSCEV(Q, SE);
          Dim2.End = N;
        }
      } else if (BoundsN.Upper && *BoundsN.Upper < QInt) {
        // [p, N]
        Intersection.Start = P;
        Intersection.End = N;
        if (FC) {
          // [M, p-1]
          auto &Dim = Grow(FC);
          Dim.Start = M;
          Dim.End = subtractOneFromSCEV(P, SE);
        }
        if (SC) {
          // [N+1, Q]
          auto &Dim = Grow(SC);
          Dim.Start = addOneToSCEV(N, SE);
          Dim.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    } else if (BoundsM.Lower && BoundsP.Upper &&
               *BoundsP.Upper < *BoundsM.Lower ||
               CmpMP && *CmpMP > 0) {
      if (BoundsN.Lower && QInt < *BoundsN.Lower) {
        // [M, Q]
        Intersection.Start = M;
        Intersection.End = Q;
        if (FC) {
          // [Q+1, N]
          auto &Dim = Grow(FC);
          Dim.Start = addOneToSCEV(Q, SE);
          Dim.End = N;
        }
        if (SC) {
          // [p, M-1]
          auto &Dim = Grow(SC);
          Dim.Start = P;
          Dim.End = subtractOneFromSCEV(M, SE);
        }
      } else if (BoundsN.Upper && *BoundsN.Upper < QInt) {
        Intersection.Start = M;
        Intersection.End = N;
        if (SC) {
          // [p, M-1]
          auto &Dim1 = Grow(SC);
          Dim1.Start = P;
          Dim1.End = subtractOneFromSCEV(M, SE);
          // [N+1, Q]
          auto &Dim2 = Grow(SC);
          Dim2.Start = addOneToSCEV(N, SE);
          Dim2.End = Q;
        }
      } else {
        return Info.UnknownIntersection;
      }
    } else {
      return Info.UnknownIntersection;
    }
  }
  return Info.TrueIntersection;
}

IntersectionResult processOneConstOtherVariable(
    const MemoryLocationRange &LeftRange, std::size_t DimIdx,
    const Dimension &Right, Dimension &Intersection,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC, IntersectVarInfo &Info) {
  auto Grow = getGrowFunction(LeftRange, DimIdx);
  auto &Left = LeftRange.DimList[DimIdx];
  assert(isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End) &&
        !isa<SCEVConstant>(Right.Start) && !isa<SCEVConstant>(Right.End) ||
         isa<SCEVConstant>(Right.Start) && isa<SCEVConstant>(Right.End) &&
        !isa<SCEVConstant>(Left.Start) && !isa<SCEVConstant>(Left.End));
  auto SE = LeftRange.SE;
  auto *AM = LeftRange.AM;
  // Let the First dimension be a dimension whose Start and End are constant.
  // We will denote the segments as follows: First = [m, n], Second = [P, Q]
  // where lowercase letters mean constants, and capital letter mean
  // variables.
  auto *First { &Left }, *Second { &Right };
  auto *FC { LC }, *SC { RC };
  if (!isa<SCEVConstant>(Left.Start)) {
    std::swap(First, Second);
    std::swap(FC, SC);
  }
  auto *M = First->Start, *N = First->End;
  auto *P = Second->Start, *Q = Second->End;
  auto BP = parseBoundExpression(P);
  auto BQ = parseBoundExpression(Q);
  if (!BP || !BQ || !AM)
      return Info.UnknownIntersection;
  auto BPItr = AM->find(BP->first);
  auto BQItr = AM->find(BQ->first);
  if (BPItr == AM->end() || BQItr == AM->end())
    return Info.UnknownIntersection;
  auto &BoundsP = BPItr->second;
  auto &BoundsQ = BQItr->second;
  auto MInt = cast<SCEVConstant>(M)->getAPInt().getSExtValue();
  auto NInt = cast<SCEVConstant>(N)->getAPInt().getSExtValue();
  if (BoundsQ.Upper && *BoundsQ.Upper < MInt ||
      BoundsP.Lower && *BoundsP.Lower > NInt)
    return Info.EmptyIntersection;
  if (BoundsP.Lower && MInt < *BoundsP.Lower) {
    if (BoundsQ.Upper && *BoundsQ.Upper < NInt) {
      // [P, Q]
      Intersection.Start = P;
      Intersection.End = Q;
      if (FC) {
        // [m, P-1]
        auto &Dim1 = Grow(FC);
        Dim1.Start = M;
        Dim1.End = subtractOneFromSCEV(P, SE);
        // [Q+1, n]
        auto &Dim2 = Grow(FC);
        Dim2.Start = addOneToSCEV(Q, SE);
        Dim2.End = N;
      }
    } else if (BoundsQ.Lower && *BoundsQ.Lower > NInt) {
      // [P, n]
      Intersection.Start = P;
      Intersection.End = N;
      if (FC) {
        // [m, P-1]
        auto &Dim = Grow(FC);
        Dim.Start = M;
        Dim.End = subtractOneFromSCEV(P, SE);
      }
      if (SC) {
        // [n+1, Q]
        auto &Dim = Grow(SC);
        Dim.Start = addOneToSCEV(N, SE);
        Dim.End = Q;
      }
    } else {
      return Info.UnknownIntersection;
    }
  } else if (BoundsP.Upper && MInt > *BoundsP.Upper) {
    if (BoundsQ.Upper && *BoundsQ.Upper < NInt) {
      // [m, Q]
      Intersection.Start = M;
      Intersection.End = Q;
      if (FC) {
        // [Q+1, n]
        auto &Dim = Grow(FC);
        Dim.Start = addOneToSCEV(Q, SE);
        Dim.End = N;
      }
      if (SC) {
        // [P, m-1]
        auto &Dim = Grow(SC);
        Dim.Start = P;
        Dim.End = subtractOneFromSCEV(M, SE);
      }
    } else if (BoundsQ.Lower && *BoundsQ.Lower > NInt) {
        Intersection.Start = M;
        Intersection.End = N;
        if (SC) {
          // [P, m-1]
          auto &Dim1 = Grow(SC);
          Dim1.Start = P;
          Dim1.End = subtractOneFromSCEV(M, SE);
          // [n+1, Q]
          auto &Dim2 = Grow(SC);
          Dim2.Start = addOneToSCEV(N, SE);
          Dim2.End = Q;
        }
    } else {
      return Info.UnknownIntersection;
    }
  } else {
    return Info.UnknownIntersection;
  }
  return Info.TrueIntersection;
}

/// \brief Intersects dimensions where at least one dimension has a variable 
/// bound.
///
/// \param [in] LeftRange A full MemoryLocationRange for the first dimension 
/// to intersect. This dimension is stored in `LeftRange.DimList[DimIdx].`
/// \param [in] DimIdx The index of the first dimension to intersect (described 
/// above).
/// \param [in] Right The second dimension to intersect.
/// \param [in] PairKind A kind of the pair of dimensions.
/// \param [out] Intersection The result of intersection.
/// \param [out] LC List of memory locations to store the difference 
/// between locations LeftRange[DimIdx] and Intersection. It will not be 
/// changed if the intersection is empty. If `LC == nullptr`, the difference 
/// will not be calculated and will not be stored anywhere.
/// \param [out] RC List of memory locations to store the difference 
/// between locations Right and Intersection. It will not be changed if the 
/// intersection is empty. If `RC == nullptr`, the difference will not be 
/// calculated and will not be stored anywhere.
/// \return A pair of an optional MemoryLocationRange and a flag. The 
/// MemoryLocationRange is set to default if the intersection is unknown and
/// is set to `None` if the intersection is empty. The flag is set to `false` if
/// the intersection is neither empty or unknown (and the real intersection is
/// stored in `Intersection`) and is set to `true` otherwise.
IntersectionResult intersectVarDims(const MemoryLocationRange &LeftRange,
    std::size_t DimIdx, const Dimension &Right, DimPairKind PairKind,
    Dimension &Intersection, llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  IntersectVarInfo Info;
  auto &Left = LeftRange.DimList[DimIdx];
  Info.UnknownIntersection = std::make_pair(MemoryLocationRange(), true);
  Info.EmptyIntersection = std::make_pair(llvm::None, true);
  Info.TrueIntersection = std::make_pair(llvm::None, false);
  Info.IsValidStep = Left.Step == Right.Step && isa<SCEVConstant>(Left.Step) &&
      cast<SCEVConstant>(Left.Step)->getAPInt().getSExtValue() == 1;
  Info.CmpStart = compareSCEVs(Left.Start, Right.Start, LeftRange.SE);
  Info.CmpEnd = compareSCEVs(Left.End, Right.End, LeftRange.SE);
  Info.CmpLeftEndRightStart = compareSCEVs(Left.End, Right.Start, LeftRange.SE);
  Info.CmpLeftStartRightEnd = compareSCEVs(Left.Start, Right.End, LeftRange.SE);
  switch (PairKind) {
  case DimPairKind::BothVariable:
    return processBothVariable(LeftRange, DimIdx, Right, Info, Intersection,
                               LC, RC);
  case DimPairKind::OneStartOtherEndConst:
    return processOneStartOtherEndConst(LeftRange, DimIdx, Right,
                                        Intersection, LC, RC, Info);
  case DimPairKind::BothStartConst:
    return processBothStartConst(LeftRange, DimIdx, Right, Intersection, LC,
                                 RC, Info);
  case DimPairKind::BothEndConst:
    return processBothEndConst(LeftRange, DimIdx, Right, Intersection, LC, RC,
                               Info);
  case DimPairKind::OneConstOtherSemiconst:
    return processOneConstOtherSemiconst(LeftRange, DimIdx, Right,
                                         Intersection, LC, RC, Info);
  case DimPairKind::OneVariableOtherSemiconst:
    return processOneVariableOtherSemiconst(LeftRange, DimIdx, Right,
                                            Intersection, LC, RC, Info);
  case DimPairKind::OneConstOtherVariable:
    return processOneConstOtherVariable(LeftRange, DimIdx, Right,
                                        Intersection, LC, RC, Info);
  default:
    llvm_unreachable("Invalid kind of dimension with variable bounds.");
  }
}
};

namespace tsar {
llvm::Optional<MemoryLocationRange> intersect(
    MemoryLocationRange LHS,
    MemoryLocationRange RHS,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC,
    unsigned Threshold) {
  typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
  typedef BAEquation::Monom Monom;
  typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
  typedef std::pair<ValueT, ValueT> VarRange;
  typedef MemoryLocationRange::LocKind LocKind;
  assert(LHS.Ptr && RHS.Ptr &&
         "Pointers of intersected memory locations must not be null!");
  // Return a location that may be an intersection, but cannot be calculated 
  // exactly.
  LLVM_DEBUG(
    llvm::dbgs() << "[INTERSECT] Intersect locations:\n";
    printLocationSource(llvm::dbgs() << "\t", LHS, nullptr, true);
    llvm::dbgs() << "\n";
    printLocationSource(llvm::dbgs() << "\t", RHS, nullptr, true);
    llvm::dbgs() << "\n";
  );
  if (LHS.Ptr != RHS.Ptr)
    return llvm::None;
  if (!(LHS.Kind & LocKind::Collapsed) &&
      !(LHS.Kind & LocKind::NonCollapsable) &&
       (RHS.Kind & LocKind::Collapsed))
    delinearize(RHS, LHS);
  if (!(RHS.Kind & LocKind::Collapsed) &&
      !(RHS.Kind & LocKind::NonCollapsable) &&
       (LHS.Kind & LocKind::Collapsed))
    delinearize(LHS, RHS);
  if (!(LHS.Kind & LocKind::Collapsed) && !(RHS.Kind & LocKind::Collapsed))
    return intersectScalar(LHS, RHS, LC, RC);
  if (!(LHS.Kind & LocKind::Collapsed) || !(RHS.Kind & LocKind::Collapsed))
    return MemoryLocationRange();
  if (LHS.DimList.size() != RHS.DimList.size())
    return MemoryLocationRange();
  if (LHS.LowerBound == RHS.LowerBound && LHS.UpperBound == RHS.UpperBound &&
      LHS.DimList == RHS.DimList)
    return LHS;
  MemoryLocationRange Int(LHS);
  assert(LHS.SE && RHS.SE && LHS.SE == RHS.SE &&
         "ScalarEvolution must be specified!");
  auto SE = LHS.SE;
  for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
    auto &Left = LHS.DimList[I];
    auto &Right = RHS.DimList[I];
    if (Left.DimSize != Right.DimSize &&
       !(LHS.DimList.size() == 1 && Left.DimSize ^ Right.DimSize))
      return MemoryLocationRange();
    auto &Intersection = Int.DimList[I];
    auto PairKind = getDimPairKind(Left, Right);
    LLVM_DEBUG(dbgs() << "[INTERSECT] Pair kind: " <<
               getKindAsString(PairKind) << "\n");
    if (PairKind != DimPairKind::BothConst) {
      auto Res = intersectVarDims(LHS, I, Right, PairKind, Intersection, LC, RC);
      if (Res.second)
        return Res.first;
      continue;
    }
    auto LeftStart = cast<SCEVConstant>(Left.Start)->getValue()->getZExtValue();
    auto LeftEnd = cast<SCEVConstant>(Left.End)->getValue()->getZExtValue();
    auto LeftStep = cast<SCEVConstant>(Left.Step)->getValue()->getZExtValue();
    auto RightStart = cast<SCEVConstant>(Right.Start)->getValue()->getZExtValue();
    auto RightEnd = cast<SCEVConstant>(Right.End)->getValue()->getZExtValue();
    auto RightStep = cast<SCEVConstant>(Right.Step)->getValue()->getZExtValue();
    if (LeftEnd < RightStart || RightEnd < LeftStart)
      return llvm::None;
    ColumnInfo Info;
    // We guarantee that K1 and K2 will not be equal to 0.
    assert(LeftStep > 0 && RightStep > 0 && "Steps must be positive!");
    assert(LeftStart <= LeftEnd && RightStart <= RightEnd &&
        "Start of dimension must be less or equal than End.");
    ValueT L1 = LeftStart, K1 = LeftStep;
    ValueT L2 = RightStart, K2 = RightStep;
    VarRange XRange(0, (LeftEnd - LeftStart) / LeftStep),
             YRange(0, (RightEnd - RightStart) / RightStep);
    LinearSystem System;
    System.push_back(Monom(0, K1), Monom(1, -K2), L2 - L1);
    System.instantiate(Info);
    auto SolutionNumber = System.solve<ColumnInfo, false>(Info);
    if (SolutionNumber == 0)
      return llvm::None;
    auto &Solution = System.getSolution();
    auto &LineX = Solution[0], &LineY = Solution[1];
    // B will be equal to 0 only if K1 is equal to 0 but K1 is always positive.
    ValueT A = LineX.Constant, B = -LineX.RHS.Value;
    // D will be equal to 0 only if K2 is equal to 0 but K2 is always positive.
    ValueT C = LineY.Constant, D = -LineY.RHS.Value;
    assert(B > 0 && "B must be positive!");
    ValueT TXmin = std::ceil((XRange.first - A) / double(B));
    ValueT TXmax = std::floor((XRange.second - A) / double(B));
    assert(D > 0 && "D must be positive!");
    ValueT TYmin = std::ceil((YRange.first - C) / double(D));
    ValueT TYmax = std::floor((YRange.second - C) / double(D));
    ValueT Tmin = std::max(TXmin, TYmin);
    ValueT Tmax = std::min(TXmax, TYmax);
    if (Tmax < Tmin)
      return llvm::None;
    ValueT Shift = Tmin;
    Tmin = 0;
    Tmax -= Shift;
    ValueT Step = K1 * B;
    ValueT Start = (K1 * A + L1) + Step * Shift;
    Intersection.Start = SE->getConstant(Left.Start->getType(), Start);
    Intersection.Step = SE->getConstant(Left.Step->getType(), Step);
    Intersection.End = SE->getConstant(Left.End->getType(), Start + Step * Tmax);
    Intersection.DimSize = std::max(Left.DimSize, Right.DimSize);
    assert(Start >= 0 && "Start must be non-negative!");
    assert(Step > 0 && "Step must be positive!");
    if (LC) {
      llvm::SmallVector<Dimension, 3> ComplLeft;
      if (!difference(Left, Intersection, ComplLeft, SE, Threshold)) {
        return MemoryLocationRange();
      } else {
        for (auto &Comp : ComplLeft)
          LC->emplace_back(LHS).DimList[I] = Comp;
      }
    }
    if (RC) {
      llvm::SmallVector<Dimension, 3> ComplRight;
      if (!difference(Right, Intersection, ComplRight, SE, Threshold)) {
        return MemoryLocationRange();
      } else {
        for (auto &Comp : ComplRight)
          RC->emplace_back(RHS).DimList[I] = Comp;
      }
    }
  }
  LLVM_DEBUG(printSolutionInfo(llvm::dbgs(), Int, LC, RC));
  return Int;
}
}

void Dimension::print(llvm::raw_ostream &OS, bool IsDebug) const {
  auto PrintSCEV = [](const llvm::SCEV *Expr, llvm::raw_ostream &OS) {
    if (!Expr)
      OS << "(nullptr)";
    else
      Expr->print(OS);
  };
  if (IsDebug) {
    OS << "{Start: ";
    PrintSCEV(Start, OS);
    OS << ", End: ";
    PrintSCEV(End, OS);
    OS << ", Step: ";
    PrintSCEV(Step, OS);
    OS << ", DimSize: " << DimSize << "}";
  } else {
    OS << "[";
    PrintSCEV(Start, OS);
    OS << ":";
    PrintSCEV(End, OS);
    OS << ":";
    PrintSCEV(Step, OS);
    OS << "," << DimSize;
    OS << "]";
  }
}