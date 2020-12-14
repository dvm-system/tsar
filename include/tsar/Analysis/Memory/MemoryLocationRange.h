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
/// starts at `Ptr + Start` address. In case of llvm::MemoryLocation
/// Start is always 0.
struct MemoryLocationRange {
  enum : uint64_t { UnknownSize = llvm::MemoryLocation::UnknownSize };

  struct Dimension {
    uint64_t Start;
    uint64_t Step;
    uint64_t MaxIter;
    uint64_t DimSize;
    inline bool operator==(const Dimension &Other) const {
      return Start == Other.Start &&
             Step == Other.Step &&
             DimSize == Other.DimSize;
    }
  };

  const llvm::Value * Ptr;
  LocationSize Start;
  LocationSize Width;
  llvm::SmallVector<Dimension, 0> DimList;
  llvm::AAMDNodes AATags;

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
                               LocationSize Start = 0,
                               LocationSize Width = UnknownSize,
                               const llvm::AAMDNodes &AATags = llvm::AAMDNodes())
      : Ptr(Ptr), Start(Start), Width(Width),
        AATags(AATags) {}

  MemoryLocationRange(const llvm::MemoryLocation &Loc)
      : Ptr(Loc.Ptr), Start(0), Width(Loc.Size), AATags(Loc.AATags) {}

  MemoryLocationRange &operator=(const llvm::MemoryLocation &Loc) {
    Ptr = Loc.Ptr;
    Start = 0;
    Width = Loc.Size;
    AATags = Loc.AATags;
    return *this;
  }

  bool operator==(const MemoryLocationRange &Other) const {
    return Ptr == Other.Ptr && AATags == Other.AATags &&
      Start == Other.Start && Width == Other.Width &&
      DimList == Other.DimList;
  }

  LocationSize getEnd() const {
    if (!Start.hasValue() || !Width.hasValue())
      return LocationSize::unknown();
    return Start.getValue() + Width.getValue();
  }

  void setEnd(LocationSize Size) {
    if (!Start.hasValue() || !Width.hasValue())
      Width = LocationSize::unknown();
    else
      Width = Size.getValue() - Start.getValue();
  }
};

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
  MemoryLocationRange intersect(const MemoryLocationRange &LHS,
      const MemoryLocationRange &RHS) {
    // TODO : check scalars
    typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
    typedef BAEquation::Monom Monom;
    typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
    typedef std::pair<ValueT, ValueT> VarRange;
    if (LHS.Ptr != RHS.Ptr || LHS.DimList.size() != RHS.DimList.size()) {
      return MemoryLocationRange();
    }
    bool Intersected = true;
    MemoryLocationRange ResultRange(LHS);
    ResultRange.DimList.resize(LHS.DimList.size());
    for (size_t I = 0; I < LHS.DimList.size(); ++I) {
      auto &Left = LHS.DimList[I];
      auto &Right = RHS.DimList[I];
      ColumnInfo Info;
      // Px = L1 + K1 * X
      // Py = L2 + K2 * Y
      ValueT L1 = Left.Start, K1 = Left.Step;
      ValueT L2 = Right.Start, K2 = Right.Step;
      Info.addVariable(std::make_pair("X", K1));
      Info.addVariable(std::make_pair("Y", -K2));
      VarRange XRange(0, Left.MaxIter), YRange(0, Right.MaxIter);
      ValueT PxMin = K1 * XRange.first + L1, PxMax = K1 * XRange.second + L1;
      ValueT PyMin = K2 * YRange.first + L2, PyMax = K2 * YRange.second + L2;
      if (PxMax <= PyMin || PxMin >= PyMax) {
        Intersected = false;
        break;
      }
      LinearSystem System;
      System.push_back(Monom(0, K1), Monom(1, -K2), L2 - L1);
      System.instantiate(Info);
      auto count = System.solve<ColumnInfo, llvm::raw_ostream, false>(
          Info, llvm::dbgs());
      if (count == 0) {
        Intersected = false;
        break;
      }
      auto &Solution = System.getSolution();
      auto &LineX = Solution[0];
      auto &LineY = Solution[1];
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
      ResultRange.DimList[I].Start = Start;
      ResultRange.DimList[I].Step = Step;
      ResultRange.DimList[I].MaxIter = Tmax;
      ResultRange.DimList[I].DimSize = Left.DimSize;
    }
    return Intersected ? ResultRange : MemoryLocationRange();
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
           DenseMapInfo<LocationSize>::getHashValue(Val.Start) ^
           DenseMapInfo<LocationSize>::getHashValue(Val.Width) ^
           DenseMapInfo<AAMDNodes>::getHashValue(Val.AATags);
  }
  static bool isEqual(const MemoryLocation &LHS, const MemoryLocation &RHS) {
    return LHS == RHS;
  }
};
}
#endif//TSAR_MEMORY_LOCATION_RANGE_H
