#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#include <bcl/Equation.h>
#include <llvm/Support/Debug.h>

using namespace tsar;

typedef MemoryLocationRange::Dimension Dimension;

namespace {
/// Finds difference between dimensions D and I where I is a subset of D
/// and adds results to Res. Return `false` if Threshold is exceeded, `true`
/// otherwise.
bool difference(const Dimension &D, const Dimension &I,
                llvm::SmallVectorImpl<Dimension> &Res, std::size_t Threshold) {
  if (D.Start < I.Start) {
    auto &Left = Res.emplace_back();
    Left.Step = D.Step;
    Left.Start = D.Start;
    Left.TripCount = (I.Start - D.Start) / D.Step;
  }
  auto DEnd = D.Start + D.Step * (D.TripCount - 1);
  auto IEnd = I.Start + I.Step * (I.TripCount - 1);
  if (DEnd > IEnd) {
    auto &Right = Res.emplace_back();
    Right.Step = D.Step;
    Right.Start = IEnd + D.Step;
    Right.TripCount = (DEnd - IEnd) / D.Step;
  }
  if (I.TripCount > 1) {
    // I.Step % D.Step is always 0 because I is a subset of D.
    auto RepeatNumber = I.Step / D.Step - 1;
    if (RepeatNumber > Threshold)
      return false;
    for (auto J = 0; J < RepeatNumber; ++J) {
      auto &Center = Res.emplace_back();
      Center.Start = I.Start + D.Step * (J + 1);
      Center.Step = D.Step;
      Center.TripCount = I.TripCount - 1;
    }
  }
  return true;
}

void printSolutionInfo(llvm::raw_ostream &OS,
    const MemoryLocationRange &Int,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  auto PrintRange = [&OS](const MemoryLocationRange &R) {
    auto &Dim = R.DimList[0];
    OS << "{" << (R.Ptr == nullptr ? "Empty" : "Full") << " | ";
    OS << Dim.Start << " + " << Dim.Step << " * T, T in [" << 0 <<
        ", " << Dim.TripCount << ")} ";
  };
  OS << "\n[EQUATION] Solution:\n";
  OS << "Left: ";
  if (LC)
    for (auto &R : *LC) {
      PrintRange(R);
    }
  OS << "\nIntersection: ";
  PrintRange(Int);
  OS << "\nRight: ";
  if (RC)
    for (auto &R : *RC) {
      PrintRange(R);
    }
  OS << "\n[EQUATION] Solution has been printed.\n";
}
};

llvm::Optional<MemoryLocationRange> MemoryLocationRangeEquation::intersect(
    const MemoryLocationRange &LHS,
    const MemoryLocationRange &RHS,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC,
    std::size_t Threshold) {
  typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
  typedef BAEquation::Monom Monom;
  typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
  typedef std::pair<ValueT, ValueT> VarRange;
  typedef MemoryLocationRange::LocKind LocKind;
  assert(LHS.Ptr && RHS.Ptr &&
      "Pointers of intersected memory locations must not be null!");
  assert((LHS.Kind != LocKind::DEFAULT || RHS.Kind != LocKind::DEFAULT) &&
      "It is forbidden to calculate an intersection between scalar variables.");
  // Return a location that may be an intersection, but cannot be calculated 
  // exactly.
  auto GetIncompleteLoc = [](const MemoryLocationRange &Sample) {
    MemoryLocationRange Loc(Sample);
    Loc.Kind = LocKind::NON_COLLAPSABLE;
    Loc.DimList.clear();
    return Loc;
  };
  if (LHS.Ptr != RHS.Ptr)
    return llvm::None;
  if (LHS.DimList.size() != RHS.DimList.size())
    return GetIncompleteLoc(LHS);
  if (LHS.LowerBound == RHS.LowerBound &&
      LHS.UpperBound == RHS.UpperBound &&
      LHS.DimList == RHS.DimList) {
    return LHS;
  }
  bool Intersected = true;
  MemoryLocationRange Int(LHS);
  for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
    auto &Left = LHS.DimList[I];
    auto &Right = RHS.DimList[I];
    auto LeftEnd = Left.Start + Left.Step * (Left.TripCount - 1);
    auto RightEnd = Right.Start + Right.Step * (Right.TripCount - 1);
    if (LeftEnd < Right.Start || RightEnd < Left.Start) {
      Intersected = false;
      break;
    }
    ColumnInfo Info;
    assert(Left.Start >= 0 && Right.Start >= 0 && "Start must be non-negative!");
    // We guarantee that K1 and K2 will not be equal to 0.
    assert(Left.Step > 0 && Right.Step > 0 && "Steps must be non-negative!");
    assert(Left.TripCount > 0 && Right.TripCount > 0 &&
        "Trip count must be positive!");
    ValueT L1 = Left.Start, K1 = Left.Step;
    ValueT L2 = Right.Start, K2 = Right.Step;
    //Info.addVariable(std::make_pair("X", K1));
    //Info.addVariable(std::make_pair("Y", -K2));
    VarRange XRange(0, Left.TripCount - 1), YRange(0, Right.TripCount - 1);
    LinearSystem System;
    System.push_back(Monom(0, K1), Monom(1, -K2), L2 - L1);
    System.instantiate(Info);
    auto SolutionNumber = System.solve<ColumnInfo, false>(Info);
    if (SolutionNumber == 0) {
      Intersected = false;
      break;
    }
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
    if (Tmax < Tmin) {
      Intersected = false;
      break;
    }
    ValueT Shift = Tmin;
    Tmin = 0;
    Tmax -= Shift;
    ValueT Step = K1 * B;
    ValueT Start = (K1 * A + L1) + Step * Shift;
    auto &Intersection = Int.DimList[I];
    Intersection.Start = Start;
    Intersection.Step = Step;
    Intersection.TripCount = Tmax + 1;
    assert(Start >= 0 && "Start must be non-negative!");
    assert(Step > 0 && "Step must be positive!");
    assert(Intersection.TripCount > 0 && "Trip count must be non-negative!");
    if (LC) {
      llvm::SmallVector<Dimension, 3> ComplLeft;
      if (!difference(Left, Intersection, ComplLeft, Threshold)) {
        LC->push_back(GetIncompleteLoc(LHS));
      } else {
        for (auto &Comp : ComplLeft) {
          auto &NewLoc = LC->emplace_back(LHS);
          NewLoc.DimList[I] = Comp;
        }
      }
    }
    if (RC) {
      llvm::SmallVector<Dimension, 3> ComplRight;
      if (!difference(Right, Intersection, ComplRight, Threshold)) {
        RC->push_back(GetIncompleteLoc(RHS));
      } else {
        for (auto &Comp : ComplRight) {
          auto &NewLoc = RC->emplace_back(RHS);
          NewLoc.DimList[I] = Comp;
        }
      }
    }
  }
  if (!Intersected)
    return llvm::None;
  //printSolutionInfo(llvm::dbgs(), Int, LC, RC);
  return Int;
}