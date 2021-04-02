//===- MemoryLocationRange.h -- Memory Location Range -----------*- C++ -*-===//
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

#ifndef TSAR_MEMORY_LOCATION_RANGE_H
#define TSAR_MEMORY_LOCATION_RANGE_H

#include <bcl/Equation.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Support/Debug.h>

namespace tsar {

using LocationSize = llvm::LocationSize;

/// Representation for a memory location with shifted start position.
///
/// The difference from llvm::MemoryLocation is that the current location
/// starts at `Ptr + LowerBound` address. In case of llvm::MemoryLocation
/// LowerBound is always 0.
struct MemoryLocationRange {
  enum : uint64_t { UnknownSize = llvm::MemoryLocation::UnknownSize };
  enum LocKind {
    FIRST_KIND = 0,
    SCALAR = FIRST_KIND,
    NON_COLLAPSABLE,
    COLLAPSABLE,
    COLLAPSED,
    EXPLICIT,
    LAST_KIND = EXPLICIT,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND,
  };

  struct Dimension {
    uint64_t Start;
    uint64_t Step;
    uint64_t TripCount;
    uint64_t DimSize;
    Dimension() : Start(0), Step(0), TripCount(0), DimSize(0) {}
    inline bool operator==(const Dimension &Other) const {
      return Start == Other.Start &&
             Step == Other.Step &&
             DimSize == Other.DimSize;
    }
  };

  const llvm::Value * Ptr;
  LocationSize LowerBound;
  LocationSize UpperBound;
  llvm::SmallVector<Dimension, 0> DimList;
  llvm::AAMDNodes AATags;
  LocKind Kind;

  /// Return a location with information about the memory reference by the given
  /// instruction.
  static MemoryLocationRange get(const llvm::LoadInst *LI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(LI));
  }
  static MemoryLocationRange get(const llvm::StoreInst *SI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(SI));
  }
  static MemoryLocationRange get(const llvm::VAArgInst *VI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(VI));
  }
  static MemoryLocationRange get(const llvm::AtomicCmpXchgInst *CXI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(CXI));
  }
  static MemoryLocationRange get(const llvm::AtomicRMWInst *RMWI) {
    return MemoryLocationRange(llvm::MemoryLocation::get(RMWI));
  }
  static MemoryLocationRange get(const llvm::Instruction *Inst) {
    return *MemoryLocationRange::getOrNone(Inst);
  }

  static llvm::Optional<MemoryLocationRange> getOrNone(
      const llvm::Instruction *Inst) {
    auto Loc = llvm::MemoryLocation::getOrNone(Inst);
    if (Loc)
      return MemoryLocationRange(*Loc);
    else
      return llvm::None;
  }

  /// Return a location representing the source of a memory transfer.
  static MemoryLocationRange getForSource(const llvm::MemTransferInst *MTI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForSource(MTI));
  }
  static MemoryLocationRange getForSource(
      const llvm::AtomicMemTransferInst *MTI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForSource(MTI));
  }
  static MemoryLocationRange getForSource(const llvm::AnyMemTransferInst *MTI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForSource(MTI));
  }

  /// Return a location representing the destination of a memory set or
  /// transfer.
  static MemoryLocationRange getForDest(const llvm::MemIntrinsic *MI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForDest(MI));
  }
  static MemoryLocationRange getForDest(const llvm::AtomicMemIntrinsic *MI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForDest(MI));
  }
  static MemoryLocationRange getForDest(const llvm::AnyMemIntrinsic *MI) {
    return MemoryLocationRange(llvm::MemoryLocation::getForDest(MI));
  }

  /// Return a location representing a particular argument of a call.
  static MemoryLocationRange getForArgument(const llvm::CallBase *Call,
      unsigned ArgIdx, const llvm::TargetLibraryInfo &TLI) {
    return MemoryLocationRange(
      llvm::MemoryLocation::getForArgument(Call, ArgIdx, TLI));
  }

  explicit MemoryLocationRange(const llvm::Value *Ptr = nullptr,
                               LocationSize LowerBound = 0,
                               LocationSize UpperBound = UnknownSize,
                               const llvm::AAMDNodes &AATags = llvm::AAMDNodes())
      : Ptr(Ptr), LowerBound(LowerBound), UpperBound(UpperBound),
        AATags(AATags), Kind(LocKind::SCALAR) {}

  explicit MemoryLocationRange(const llvm::Value *Ptr,
                               LocationSize LowerBound,
                               LocationSize UpperBound,
                               LocKind Kind,
                               const llvm::AAMDNodes &AATags = llvm::AAMDNodes())
      : Ptr(Ptr), LowerBound(LowerBound), UpperBound(UpperBound),
        AATags(AATags), Kind(Kind) {}

  MemoryLocationRange(const llvm::MemoryLocation &Loc)
      : Ptr(Loc.Ptr), LowerBound(0), UpperBound(Loc.Size), AATags(Loc.AATags),
        Kind(LocKind::SCALAR) {}

  MemoryLocationRange(const MemoryLocationRange &Loc)
      : Ptr(Loc.Ptr), LowerBound(Loc.LowerBound), UpperBound(Loc.UpperBound),
        AATags(Loc.AATags), DimList(Loc.DimList), Kind(Loc.Kind) {}

  MemoryLocationRange &operator=(const llvm::MemoryLocation &Loc) {
    Ptr = Loc.Ptr;
    LowerBound = 0;
    UpperBound = Loc.Size;
    AATags = Loc.AATags;
    return *this;
  }

  bool operator==(const MemoryLocationRange &Other) const {
    return Ptr == Other.Ptr && AATags == Other.AATags &&
      LowerBound == Other.LowerBound && UpperBound == Other.UpperBound &&
      DimList == Other.DimList && Kind == Other.Kind;
  }
};

namespace MemoryLocationRangeEquation {
  typedef int64_t ColumnT;
  typedef int64_t ValueT;
  typedef MemoryLocationRange::Dimension Dimension;

  struct ColumnInfo {
    void addVariable(const std::pair<std::string, ValueT> &Var) {
      Variables.push_back(Var);
    }

    template<typename T>
    T get(ColumnT Column) const {
      return Variables[Column].second;
    }

    ColumnT parameterColumn() {
      Variables.push_back(std::make_pair("T", 0));
      return Variables.size() - 1;
    }

    ColumnT parameterColumn(ColumnT Column) {
      Variables.push_back(Variables[Column]);
      Variables.back().first += "'";
      return Variables.size() - 1;
    }

    bool isParameter(ColumnT Column) const {
      return Column > 1;
    }

    std::string name(ColumnT Column) const {
      return Variables[Column].first;
    }

    std::vector<std::pair<std::string, ValueT>> Variables;
  };

  class DimensionGraph {
  public:
    typedef llvm::SmallVector<MemoryLocationRange, 0> LocationList;
    DimensionGraph(std::size_t Depth, LocationList *LC, LocationList *RC)
        : mLC(LC), mRC(RC) {
      mNodes.resize(Depth + 1);
      assert(mNodes.size() != 0 && "Node list must not be empty!");
      mSource = &mNodes[0].emplace_back();
    }

    void addLevel(const Dimension &L, const Dimension &I, const Dimension &R,
                  std::size_t LevelN){
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

    void generateRanges(const MemoryLocationRange &Loc) {
      MemoryLocationRange NewLoc(Loc);
      if (mLC)
        for (auto *Child : mSource->LeftChildren)
          fillDimension(Child, true, NewLoc, 0, mLC, Child->IsIntersection);
      if (mRC)
        for (auto *Child : mSource->RightChildren)
          fillDimension(Child, false, NewLoc, 0, mRC, Child->IsIntersection);
    }

    friend llvm::raw_ostream &operator<<(
        llvm::raw_ostream &OS, const DimensionGraph &G);
  private:
    struct Node {
      Dimension Dim;
      llvm::SmallVector<Node *, 0> LeftChildren, RightChildren;
      bool IsIntersection;
      Node(bool IsIntersection = false) : IsIntersection(IsIntersection) {}
      Node(const Dimension &Dim, bool IsIntersection = false)
          : Dim(Dim), IsIntersection(IsIntersection) {}
    };
    /// Finds difference between dimensions D and I where I is a subset of D
    /// and adds results in Res.
    void difference(const Dimension &D, const Dimension &I,
        llvm::SmallVector<Node, 3> &Res) {
      if (D.Start < I.Start) {
        auto &Left = Res.emplace_back().Dim;
        Left.Step = D.Step;
        Left.DimSize = D.DimSize;
        Left.Start = D.Start;
        Left.TripCount = (I.Start - D.Start) / D.Step;
      }
      auto DEnd = D.Start + D.Step * (D.TripCount - 1);
      auto IEnd = I.Start + I.Step * (I.TripCount - 1);
      if (DEnd > IEnd) {
        auto &Right = Res.emplace_back().Dim;
        Right.Step = D.Step;
        Right.DimSize = D.DimSize;
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
          Center.DimSize = D.DimSize;
          Center.TripCount = I.TripCount - 1;
        }
      }
    }
    
    void fillDimension(Node *N, bool IsLeft, MemoryLocationRange &Loc,
                       std::size_t Depth, LocationList *List, bool IntFlag) {
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

    Node *mSource;
    llvm::SmallVector<llvm::SmallVector<Node, 3>, 4> mNodes;
    LocationList *mLC, *mRC;
  };

  inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                       const DimensionGraph &G) {
    OS << "Dimension Graph:\n";
    std::size_t N = 0;
    for (auto &Level : G.mNodes) {
      OS << "Level " << N << ":\n";
      for (auto &Node : Level) {
        auto &Dim = Node.Dim;
        OS << "[" << &Node << "] ";
        OS << "(" << (Node.IsIntersection ? "I" : "N") << ")";
        OS << "{Start: " << Dim.Start << ", Step: " << Dim.Step <<
            ", TripCount: " << Dim.TripCount << ", DimSize: " <<
            Dim.DimSize << "} ";
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
    OS << "Has Left: " << (G.mLC ? "Yes" : "No") << "\n";
    OS << "Has Right: " << (G.mRC ? "Yes" : "No") << "\n";
    return OS;
  }

  /// Finds an intersection between locations LHS and RHS. 
  inline bool intersect(const MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS, MemoryLocationRange &Int,
      llvm::SmallVector<MemoryLocationRange, 0> *LC,
      llvm::SmallVector<MemoryLocationRange, 0> *RC) {
    typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
    typedef BAEquation::Monom Monom;
    typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
    typedef std::pair<ValueT, ValueT> VarRange;
    if (LHS.Ptr != RHS.Ptr || !LHS.Ptr ||
        LHS.DimList.size() != RHS.DimList.size()) {
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
      assert(Left.DimSize == Right.DimSize &&
          "It is forbidden to compare arrays of different dimension sizes!");
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
      auto SolutionNumber = System.solve<ColumnInfo, llvm::raw_ostream, false>(
          Info, llvm::dbgs());
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
      Intersection.DimSize = Left.DimSize;
      DimGraph.addLevel(Left, Intersection, Right, I);
    }
    //llvm::dbgs() << "[SOLUTION] " << DimGraph;
    DimGraph.generateRanges(LHS);
    /*auto PrintRange = [](const MemoryLocationRange &R) {
      auto &Dim = R.DimList[0];
      //std::string IsEmp = R.Ptr == nullptr ? "Yes" : "No";
      llvm::dbgs() << "{" << (R.Ptr == nullptr ? "Empty" : "Full") << " | ";
      llvm::dbgs() << Dim.Start << " + " << Dim.Step << " * T, T in [" << 0 <<
          ", " << Dim.TripCount << ")} ";
    };
    llvm::dbgs() << "[SOLUTION]:\n";
    llvm::dbgs() << "Left: ";
    if (LC)
      for (auto &R : *LC) {
        PrintRange(R);
      }
    llvm::dbgs() << "\nIntersection: ";
    PrintRange(Int);
    llvm::dbgs() << "\nRight: ";
    if (RC)
      for (auto &R : *RC) {
        PrintRange(R);
      }
    llvm::dbgs() << "\n";*/
    if (!Intersected)
      Int.Ptr = nullptr;
    return Intersected;
  }
}
}

namespace llvm {
// Specialize DenseMapInfo for tsar::MemoryLocationRange.
template <> struct DenseMapInfo<tsar::MemoryLocationRange> {
  static inline tsar::MemoryLocationRange getEmptyKey() {
    return tsar::MemoryLocationRange(
      DenseMapInfo<const Value *>::getEmptyKey(), 0);
  }
  static inline tsar::MemoryLocationRange getTombstoneKey() {
    return tsar::MemoryLocationRange(
      DenseMapInfo<const Value *>::getTombstoneKey(), 0);
  }
  static unsigned getHashValue(const tsar::MemoryLocationRange &Val) {
    return DenseMapInfo<const Value *>::getHashValue(Val.Ptr) ^
           DenseMapInfo<LocationSize>::getHashValue(Val.LowerBound) ^
           DenseMapInfo<LocationSize>::getHashValue(Val.UpperBound) ^
           DenseMapInfo<AAMDNodes>::getHashValue(Val.AATags);
  }
  static bool isEqual(const MemoryLocation &LHS, const MemoryLocation &RHS) {
    return LHS == RHS;
  }
};
}
#endif//TSAR_MEMORY_LOCATION_RANGE_H
