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

typedef llvm::SmallVector<MemoryLocationRange, 0> LocationList;

namespace MemoryLocationRangeEquation {
  typedef int64_t ColumnT;
  typedef int64_t ValueT;

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

  /// Returns an intersection between two locations. If intersection is empty,
  /// return default MemoryLocationRange. 
  inline bool intersect(const MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS, MemoryLocationRange &Int,
      llvm::SmallVector<MemoryLocationRange, 0> *LC,
      llvm::SmallVector<MemoryLocationRange, 0> *RC) {
    // TODO : check scalars
    typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
    typedef BAEquation::Monom Monom;
    typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
    typedef std::pair<ValueT, ValueT> VarRange;
    typedef MemoryLocationRange::Dimension Dimension;
    if (LHS.Ptr != RHS.Ptr || !LHS.Ptr ||
        LHS.DimList.size() != RHS.DimList.size()) {
      Int.Ptr = nullptr;
      return false;
    }
    /*auto sizecmp = [](llvm::LocationSize LHS, llvm::LocationSize RHS) {
      if (LHS == RHS || !LHS.hasValue() && !RHS.hasValue())
        return 0;
      if (!LHS.hasValue())
        return 1;
      if (!RHS.hasValue())
        return -1;
      return LHS.getValue() < RHS.getValue() ? -1 :
        LHS.getValue() == RHS.getValue() ? 0 : 1;
    };
    if (LHS.DimList.empty()) {
      Int.Ptr = nullptr;
      if (sizecmp(LHS.UpperBound, RHS.LowerBound) > 0 &&
          sizecmp(LHS.LowerBound, RHS.UpperBound) < 0) {
        Int.Ptr = LHS.Ptr;
        Int.UpperBound = sizecmp(LHS.UpperBound, RHS.UpperBound) < 0 ?
            LHS.UpperBound : RHS.UpperBound;
        Int.LowerBound = sizecmp(LHS.LowerBound, RHS.LowerBound) > 0 ?
            LHS.LowerBound : RHS.LowerBound;
        if (sizecmp(LHS.LowerBound, RHS.LowerBound) < 0) {
          auto &Left = LC.emplace_back(MemoryLocationRange(Int));
          Left.LowerBound = LHS.LowerBound;
          Left.UpperBound = Int.LowerBound;
        }
        if (sizecmp(LHS.UpperBound, RHS.LowerBound) < 0) {
          auto &Left = LC.emplace_back(MemoryLocationRange(Int));
          Left.LowerBound = LHS.LowerBound;
          Left.UpperBound = Int.LowerBound;
        }
        return true;
      }
      return false;
    }*/
    auto diff = [](const Dimension &D, const Dimension &I,
        llvm::SmallVector<Dimension, 0> &Res) {
      if (D.Start < I.Start) {
        auto &Left = Res.emplace_back(Dimension());
        Left.Step = D.Step;
        Left.DimSize = D.DimSize;
        Left.Start = D.Start;
        Left.TripCount = (I.Start - D.Start) / D.Step;
      }
      auto DEnd = D.Start + D.Step * (D.TripCount - 1);
      auto IEnd = I.Start + I.Step * (I.TripCount - 1);
      if (DEnd > IEnd) {
        auto &Right = Res.emplace_back(Dimension());
        Right.Step = D.Step;
        Right.DimSize = D.DimSize;
        Right.Start = IEnd + D.Step;
        Right.TripCount = (DEnd - IEnd) / D.Step;
      }
      if (I.TripCount > 1) {
        // I.Step % D.Step is always 0 because I is a subset of D.
        auto RepeatNumber = I.Step / D.Step - 1;
        for (auto J = 0; J < RepeatNumber; ++J) {
          auto &Center = Res.emplace_back(Dimension());
          Center.Start = I.Start + (J + 1);
          Center.Step = D.Step;
          Center.DimSize = D.DimSize;
          Center.TripCount = I.TripCount - 1;
        }
      }
    };
    bool Intersected = true;
    Int = LHS;
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
      auto LeftCompl = Left;
      auto RightCompl = Right;
      llvm::SmallVector<Dimension, 0> LeftDiff, RightDiff;
      if (LC) {
        diff(Left, Intersection, LeftDiff);
      }
      if (RC) {
        diff(Right, Intersection, RightDiff);
      }
      if (LHS.DimList.size() == 1) {
        if (LC) {
          for (auto &LeftD : LeftDiff) {
            auto &R = LC->emplace_back(MemoryLocationRange(
                LHS.Ptr, LHS.LowerBound, LHS.UpperBound, LHS.Kind, LHS.AATags));
            R.DimList.push_back(LeftD);
          }
        }
        if (RC) {
          for (auto &RightD : RightDiff) {
            auto &R = RC->emplace_back(MemoryLocationRange(
                LHS.Ptr, LHS.LowerBound, LHS.UpperBound, LHS.Kind, LHS.AATags));
            R.DimList.push_back(RightD);
          }
        }
      }
    }
    /*auto PrintRange = [](const MemoryLocationRange &R) {
      auto &Dim = R.DimList[0];
      //std::string IsEmp = R.Ptr == nullptr ? "Yes" : "No";
      llvm::dbgs() << "{" << (R.Ptr == nullptr ? "Empty" : "Full") << " | ";
      llvm::dbgs() << Dim.Start << " + " << Dim.Step << " * T, T in [" << 0 <<
          ", " << Dim.TripCount << ")} ";
    };
    llvm::dbgs() << "[SOLUTION]:\n";
    llvm::dbgs() << "Left: ";
    for (auto &R : LC) {
      PrintRange(R);
    }
    llvm::dbgs() << "\nIntersection: ";
    PrintRange(Int);
    llvm::dbgs() << "\nRight: ";
    for (auto &R : RC) {
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
