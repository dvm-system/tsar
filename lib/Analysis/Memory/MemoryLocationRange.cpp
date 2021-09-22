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

struct ColumnInfo {
  std::array<char, 3> Variables = {'X', 'Y', 'T'};

  template<typename T> T get(ColumnT Column) const { return 0; }
  ColumnT parameterColumn() { return 2; }
  bool isParameter(ColumnT Column) const { return Column > 1; }
  char name(ColumnT Column) const { return Variables[Column]; }
};

/// Finds difference between dimensions D and I where I is a subset of D
/// and adds results to Res. Return `false` if Threshold is exceeded, `true`
/// otherwise.
bool difference(const Dimension &D, const Dimension &I,
                llvm::SmallVectorImpl<Dimension> &Res,
                llvm::ScalarEvolution &SE, std::size_t Threshold) {
  
  // if D.Start < I.Start
  auto DStepConst = dyn_cast<SCEVConstant>(D.Step);
  auto IStepConst = dyn_cast<SCEVConstant>(I.Step);
  assert(DStepConst && IStepConst && "Dimension step must be constant.");
  auto DStartConst = dyn_cast<SCEVConstant>(D.Start);
  auto IStartConst = dyn_cast<SCEVConstant>(I.Start);
  assert(DStartConst && IStartConst && "Dimension start must be constant.");
  if (DStartConst->getValue()->getZExtValue() <
      IStartConst->getValue()->getZExtValue()) {
    auto &Left = Res.emplace_back();
    Left.Step = D.Step;
    Left.Start = D.Start;
    Left.End = SE.getMinusSCEV(I.Start, D.Step);
    assert(dyn_cast<SCEVConstant>(Left.Start) &&
           dyn_cast<SCEVConstant>(Left.End) &&
           "Dimension bounds must be constant.");
    Left.DimSize = D.DimSize;
  }
  auto DEndConst = dyn_cast<SCEVConstant>(D.End);
  auto IEndConst = dyn_cast<SCEVConstant>(I.End);
  assert(DEndConst && IEndConst && "Dimension end must be constant.");
  // if D.End > I.End
  if (DEndConst->getValue()->getZExtValue() >
      IEndConst->getValue()->getZExtValue()) {
    auto &Right = Res.emplace_back();
    Right.Step = D.Step;
    Right.Start = SE.getAddExpr(I.End, D.Step);
    Right.End = D.End;
    assert(dyn_cast<SCEVConstant>(Right.Start) &&
           dyn_cast<SCEVConstant>(Right.End) &&
           "Dimension start must be constant.");
    Right.DimSize = D.DimSize;
  }
  // if I.TripCount > 1 (I.End > I.Start)
  if (IEndConst->getValue()->getZExtValue() >
      IStartConst->getValue()->getZExtValue()) {
    // I.Step % D.Step is always 0 because I is a subset of D.
    auto Quotient = divide(SE, I.Step, D.Step).Quotient;
    assert(dyn_cast<SCEVConstant>(Quotient) && "Quotient must be constant!");
    // I.Step / D.Step - 1
    auto RepeatNumber = SE.getMinusSCEV(
        Quotient, SE.getOne(Quotient->getType()));
    auto RepeatNumberConst = llvm::dyn_cast<llvm::SCEVConstant>(RepeatNumber);
    assert(RepeatNumberConst && "Repeat Number must be constant.");
    if (RepeatNumberConst->getValue()->getZExtValue() > Threshold)
      return false;
    // I.TripCount = (I.End - I.Start) / Step + 1
    // CenterTripCount = I.TripCount - 1 = (I.End - I.Start) / Step
    auto CenterTripCountMinusOne = SE.getMinusSCEV(divide(SE,
        SE.getMinusSCEV(I.End, I.Start), I.Step).Quotient,
        SE.getOne(I.Step->getType()));
    assert(dyn_cast<SCEVConstant>(CenterTripCountMinusOne) &&
           "Trip count must be constant!");
    for (auto J = 0; J < RepeatNumberConst->getValue()->getZExtValue(); ++J) {
      auto &Center = Res.emplace_back();
      //Center.Start = I.Start + D.Step * (J + 1);
      Center.Start = SE.getAddExpr(I.Start, SE.getMulExpr(
          D.Step, SE.getConstant(I.Start->getType(), J + 1)));
      Center.Step = I.Step;
      // Center.End = Center.Start + (Center.TripCount - 1) * Center.Step
      Center.End = SE.getAddExpr(
          Center.Start, SE.getMulExpr(CenterTripCountMinusOne, Center.Step));
      assert(dyn_cast<SCEVConstant>(Center.Start) &&
             "Dimension start must be constant!");
      assert(dyn_cast<SCEVConstant>(Center.Step) &&
             "Dimension step must be constant!");
      assert(dyn_cast<SCEVConstant>(Center.End) &&
             "Dimension end must be constant!");
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
  assert(LHS.SE && RHS.SE && LHS.SE == RHS.SE && "ScalarEvolution must be specified!");
  auto &SE = *LHS.SE;
  for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
    auto &Left = LHS.DimList[I];
    auto &Right = RHS.DimList[I];
    if (Left.DimSize != Right.DimSize)
      return MemoryLocationRange();
    auto LeftStepConst = dyn_cast<SCEVConstant>(Left.Step);
    auto RightStepConst = dyn_cast<SCEVConstant>(Right.Step);
    assert(LeftStepConst && RightStepConst &&
           "Dimension step must be constant!");
    auto LeftStartConst = dyn_cast<SCEVConstant>(Left.Start);
    auto LeftEndConst = dyn_cast<SCEVConstant>(Left.End);
    auto RightStartConst = dyn_cast<SCEVConstant>(Right.Start);
    auto RightEndConst = dyn_cast<SCEVConstant>(Right.End);
    assert(LeftStartConst && LeftEndConst && RightStartConst &&
           RightEndConst && "Dimension bounds must be constant!");
    auto LeftStart = LeftStartConst->getValue()->getZExtValue();
    auto LeftEnd = LeftEndConst->getValue()->getZExtValue();
    auto LeftStep = LeftStepConst->getValue()->getZExtValue();
    auto RightStart = RightStartConst->getValue()->getZExtValue();
    auto RightEnd = RightEndConst->getValue()->getZExtValue();
    auto RightStep = RightStepConst->getValue()->getZExtValue();
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
    auto &Intersection = Int.DimList[I];
    Intersection.Start = SE.getConstant(Left.Start->getType(), Start);
    Intersection.Step = SE.getConstant(Left.Step->getType(), Step);
    Intersection.End = SE.getConstant(Left.End->getType(), Start + Step * Tmax);
    Intersection.DimSize = Left.DimSize;
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
