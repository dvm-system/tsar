#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#include <bcl/Equation.h>
#include <llvm/Support/Debug.h>

using namespace tsar;

void MemoryLocationRangeEquation::DimensionGraph::addLevel(
    const Dimension &L, const Dimension &I, const Dimension &R,
    std::size_t LevelN) {
  if (!mLC && !mRC)
        return;
  assert(mSource && "Source Node of a DimensionGraph must be null.");
  LevelN += 1;
  assert(LevelN != 0 && "Level number must not be zero!");
  auto &CurLevel = mNodes[LevelN];
  if (mLC)
    difference(L, I, CurLevel);
  auto Idx = CurLevel.size();
  if (mLC) {
    if (LevelN <= 1) {
      for (auto &N : CurLevel)
        mSource->LeftChildren.push_back(&N);
    } else {
      for (auto &BeforeLastNodes : mNodes[LevelN - 2])
        for (auto *Child : BeforeLastNodes.LeftChildren)
          for (std::size_t I = 0; I < Idx; ++I)
            Child->LeftChildren.push_back(&CurLevel[I]);
    }
  }
  if (mRC) {
    difference(R, I, CurLevel);
    if (LevelN <= 1) {
      for (std::size_t I = Idx; I < CurLevel.size(); ++I)
        mSource->RightChildren.push_back(&CurLevel[I]);
    } else {
      for (auto &BeforeLastNodes : mNodes[LevelN - 2])
        for (auto *Child : BeforeLastNodes.RightChildren)
          for (std::size_t I = Idx; I < CurLevel.size(); ++I)
            Child->RightChildren.push_back(&CurLevel[I]);
    }
  }
  CurLevel.push_back(Node(I, true));
  if (mLC) {
    if (LevelN <= 1) {
      mSource->LeftChildren.push_back(&CurLevel.back());
    } else {
      for (auto &BeforeLastNodes : mNodes[LevelN - 2])
        for (auto *Child : BeforeLastNodes.LeftChildren)
          Child->LeftChildren.push_back(&CurLevel.back());
    }
  }
  if (mRC) {
    if (LevelN <= 1) {
      mSource->RightChildren.push_back(&CurLevel.back());
    } else {
      for (auto &BeforeLastNodes : mNodes[LevelN - 2])
        for (auto *Child : BeforeLastNodes.RightChildren)
          Child->RightChildren.push_back(&CurLevel.back());
    }
  }
}

void MemoryLocationRangeEquation::DimensionGraph::difference(
    const Dimension &D, const Dimension &I, llvm::SmallVector<Node, 3> &Res) {
  if (D.Start < I.Start) {
    auto &Left = Res.emplace_back().Dim;
    Left.Step = D.Step;
    Left.Start = D.Start;
    Left.TripCount = (I.Start - D.Start) / D.Step;
  }
  auto DEnd = D.Start + D.Step * (D.TripCount - 1);
  auto IEnd = I.Start + I.Step * (I.TripCount - 1);
  if (DEnd > IEnd) {
    auto &Right = Res.emplace_back().Dim;
    Right.Step = D.Step;
    Right.Start = IEnd + D.Step;
    Right.TripCount = (DEnd - IEnd) / D.Step;
  }
  if (I.TripCount > 1) {
    // I.Step % D.Step is always 0 because I is a subset of D.
    auto RepeatNumber = I.Step / D.Step - 1;
    for (auto J = 0; J < RepeatNumber; ++J) {
      auto &Center = Res.emplace_back().Dim;
      Center.Start = I.Start + (J + 1);
      Center.Step = D.Step;
      Center.TripCount = I.TripCount - 1;
    }
  }
}

void MemoryLocationRangeEquation::DimensionGraph::generateRanges(
    const MemoryLocationRange &Loc) {
  MemoryLocationRange NewLoc(Loc);
  if (mLC)
    for (auto *Child : mSource->LeftChildren)
      fillDimension(Child, true, NewLoc, 0, mLC, Child->IsIntersection);
  if (mRC)
    for (auto *Child : mSource->RightChildren)
      fillDimension(Child, false, NewLoc, 0, mRC, Child->IsIntersection);
}

void MemoryLocationRangeEquation::DimensionGraph::fillDimension(
    Node *N, bool IsLeft, MemoryLocationRange &Loc, std::size_t Depth,
    LocationList *List, bool IntFlag) {
  Loc.DimList[Depth] = N->Dim;
  if (Depth + 2 == mNodes.size()) {
    if (!IntFlag) {
      List->push_back(Loc);
    }
  } else {
    llvm::SmallVector<Node *, 0> &ChildList = IsLeft ?
        N->LeftChildren : N->RightChildren;
    for (auto *Child : ChildList)
      fillDimension(Child, IsLeft, Loc, Depth + 1, List,
          IntFlag & Child->IsIntersection);
  }
}

void MemoryLocationRangeEquation::DimensionGraph::printGraph(
    llvm::raw_ostream &OS) {
  OS << "Dimension Graph:\n";
  std::size_t N = 0;
  for (auto &Level : mNodes) {
    OS << "Level " << N << ":\n";
    for (auto &Node : Level) {
      auto &Dim = Node.Dim;
      OS << "[" << &Node << "] ";
      OS << "(" << (Node.IsIntersection ? "I" : "N") << ")";
      OS << "{Start: " << Dim.Start << ", Step: " << Dim.Step <<
          ", TripCount: " << Dim.TripCount << "} ";
      OS << "[L: ";
      for (auto *Child : Node.LeftChildren)
        OS << Child << ", ";
      OS << "][R: ";
      for (auto *Child : Node.RightChildren)
        OS << Child << ", ";
      OS << "]\n";
    }
    ++N;
    OS << "\n";
  }
  OS << "Has Left: " << (mLC ? "Yes" : "No") << "\n";
  OS << "Has Right: " << (mRC ? "Yes" : "No") << "\n";
}

void MemoryLocationRangeEquation::DimensionGraph::printSolutionInfo(
    llvm::raw_ostream &OS,
    const MemoryLocationRange &Int) {
  auto PrintRange = [&OS](const MemoryLocationRange &R) {
    auto &Dim = R.DimList[0];
    OS << "{" << (R.Ptr == nullptr ? "Empty" : "Full") << " | ";
    OS << Dim.Start << " + " << Dim.Step << " * T, T in [" << 0 <<
        ", " << Dim.TripCount << ")} ";
  };
  OS << "[EQUATION] Solution:\n";
  printGraph(OS);
  OS << "Left: ";
  if (mLC)
    for (auto &R : *mLC) {
      PrintRange(R);
    }
  OS << "\nIntersection: ";
  PrintRange(Int);
  OS << "\nRight: ";
  if (mRC)
    for (auto &R : *mRC) {
      PrintRange(R);
    }
  OS << "\n[EQUATION] Solution has been printed.\n";
}

bool MemoryLocationRangeEquation::intersect(const MemoryLocationRange &LHS,
    const MemoryLocationRange &RHS, MemoryLocationRange &Int,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
  typedef BAEquation::Monom Monom;
  typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
  typedef std::pair<ValueT, ValueT> VarRange;
  if (LHS.Ptr != RHS.Ptr || !LHS.Ptr ||
      LHS.DimList.size() != RHS.DimList.size() ||
      LHS.DimList.size() == 0) {
    Int.Ptr = nullptr;
    return false;
  }
  bool Intersected = true;
  Int = LHS;
  DimensionGraph DimGraph(LHS.DimList.size(), LC, RC);
  for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
    auto &Left = LHS.DimList[I];
    auto &Right = RHS.DimList[I];
    auto LeftEnd = Left.Start + Left.Step * (Left.TripCount - 1);
    auto RightEnd = Right.Start + Right.Step * (Right.TripCount - 1);
    if (LHS.LowerBound == RHS.LowerBound &&
        LHS.UpperBound == RHS.UpperBound &&
        LHS.DimList == RHS.DimList) {
      Int.DimList[I] = Left;
      continue;
    } else if (LeftEnd < Right.Start || RightEnd < Left.Start) {
      Intersected = false;
      break;
    }
    ColumnInfo Info;
    ValueT L1 = Left.Start, K1 = Left.Step;
    ValueT L2 = Right.Start, K2 = Right.Step;
    Info.addVariable(std::make_pair("X", K1));
    Info.addVariable(std::make_pair("Y", -K2));
    assert(Left.TripCount > 0 && Right.TripCount > 0 &&
        "Trip count must be positive!");
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
    ValueT A = LineX.Constant, B = -LineX.RHS.Value;
    ValueT C = LineY.Constant, D = -LineY.RHS.Value;
    ValueT TXmin = std::ceil((XRange.first - A) / double(B));
    ValueT TXmax = std::floor((XRange.second - A) / double(B));
    ValueT TYmin = std::ceil((YRange.first - C) / double(D));
    ValueT TYmax = std::floor((YRange.second - C) / double(D));
    ValueT Tmin = std::max(TXmin, TYmin);
    ValueT Tmax = std::min(TXmax, TYmax);
    ValueT Shift = Tmin;
    Tmin = 0;
    Tmax -= Shift;
    ValueT Step = K1 * B;
    ValueT Start = (K2 * A + L1) + Step * Shift;
    assert(Start >= 0 && "Start must be non-negative!");
    assert(Step >= 0 && "Step must be non-negative!");
    auto &Intersection = Int.DimList[I];
    Intersection.Start = Start;
    Intersection.Step = Step;
    Intersection.TripCount = Tmax + 1;
    DimGraph.addLevel(Left, Intersection, Right, I);
  }
  if (Intersected)
    DimGraph.generateRanges(LHS);
  else
    Int.Ptr = nullptr;
  //printSolutionInfo(llvm::dbgs(), Int, DimGraph);
  return Intersected;
}