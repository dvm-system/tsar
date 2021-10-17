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

enum DimPairKind {
  BothVariable,
  OneStartOtherEndConst,
  BothStartConst,
  BothEndConst,
  OneConstOtherSemiconst,
  FullConst,
  OneFullConstOtherFullVariable,
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
  case FullConst:
    return "FullConst"; 
  case OneFullConstOtherFullVariable:
    return "OneFullConstOtherFullVariable";
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
    Left.End = SE->getMinusSCEV(I.Start, D.Step);
    assert(isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End) &&
           "Dimension bounds must be constant.");
    Left.DimSize = D.DimSize;
  }
  assert(isa<SCEVConstant>(D.End) && isa<SCEVConstant>(I.End) &&
         "Dimension end must be constant.");
  if (*compareSCEVs(D.End, I.End, SE) > 0) {
    auto &Right = Res.emplace_back();
    Right.Step = D.Step;
    Right.Start = SE->getAddExpr(I.End, D.Step);
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
    auto RepeatNumber = SE->getMinusSCEV(
        Quotient, SE->getOne(Quotient->getType()));
    auto RepeatNumberConst = dyn_cast<SCEVConstant>(RepeatNumber);
    assert(RepeatNumberConst && "Repeat Number must be constant.");
    if (RepeatNumberConst->getValue()->getZExtValue() > Threshold)
      return false;
    // I.TripCount = (I.End - I.Start) / Step + 1
    // CenterTripCount = I.TripCount - 1 = (I.End - I.Start) / Step
    auto CenterTripCountMinusOne = SE->getMinusSCEV(divide(*SE,
        SE->getMinusSCEV(I.End, I.Start), I.Step).Quotient,
        SE->getOne(I.Step->getType()));
    assert(isa<SCEVConstant>(CenterTripCountMinusOne) &&
           "Trip count must be constant!");
    for (auto J = 0; J < RepeatNumberConst->getValue()->getZExtValue(); ++J) {
      auto &Center = Res.emplace_back();
      //Center.Start = I.Start + D.Step * (J + 1);
      Center.Start = SE->getAddExpr(I.Start, SE->getMulExpr(
          D.Step, SE->getConstant(I.Start->getType(), J + 1)));
      Center.Step = I.Step;
      // Center.End = Center.Start + (Center.TripCount - 1) * Center.Step
      Center.End = SE->getAddExpr(
          Center.Start, SE->getMulExpr(CenterTripCountMinusOne, Center.Step));
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
    return DimPairKind::OneFullConstOtherFullVariable;
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
  if (LStartConst && LEndConst && RStartConst && REndConst)
    return DimPairKind::FullConst;
  return DimPairKind::Unknown;
}

std::pair<llvm::Optional<MemoryLocationRange>, bool>
processVariableBound(const MemoryLocationRange &LHS,
                     const Dimension &Right,
                     Dimension &Intersection,
                     std::size_t DimIdx,
                     DimPairKind PairKind,
                     llvm::SmallVectorImpl<MemoryLocationRange> *LC,
                     llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  auto &Left = LHS.DimList[DimIdx];
  auto SE = LHS.SE;
  auto CmpStart = compareSCEVs(Left.Start, Right.Start, SE);
  auto CmpEnd = compareSCEVs(Left.End, Right.End, SE);
  auto CmpBC = compareSCEVs(Left.End, Right.Start, SE);
  auto CmpAD = compareSCEVs(Left.Start, Right.End, SE);
  auto UnknownIntersection = std::make_pair(MemoryLocationRange(), true);
  auto EmptyIntersection = std::make_pair(llvm::None, true);
  if (PairKind == DimPairKind::BothVariable) {
    if (CmpBC && *CmpBC < 0 || CmpAD && *CmpAD > 0)
      return EmptyIntersection;
    if (!CmpStart || !CmpEnd)
      return UnknownIntersection;
    if (*CmpStart >= 0 && *CmpEnd <= 0) {
      Intersection = Left;
      if (RC && *CmpStart > 0) {
        auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
        Dim.Start = Right.Start;
        Dim.End = SE->getMinusSCEV(Left.Start,
            SE->getOne(Left.Start->getType()));
      }
      if (RC && *CmpEnd < 0) {
        auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
        Dim.Start = SE->getAddExpr(Left.End, SE->getOne(Left.End->getType()));
        Dim.End = Right.End;
      }
    } else if (*CmpStart <= 0 && *CmpEnd >= 0) {
      Intersection = Right;
      if (LC && *CmpStart < 0) {
        auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
        Dim.Start = Left.Start;
        Dim.End = SE->getMinusSCEV(Right.Start,
            SE->getOne(Right.Start->getType()));
      }
      if (LC && *CmpEnd > 0) {
        auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
        Dim.Start = SE->getAddExpr(Right.End, SE->getOne(Right.End->getType()));
        Dim.End = Left.End;
      }
    } else if (CmpBC && CmpAD && *CmpBC >= 0 && *CmpAD <= 0) {
      Intersection.Start = (*CmpStart) > 0 ? Left.Start : Right.Start;
      Intersection.End = (*CmpEnd) < 0 ? Left.End : Right.End;
      if (LC && *CmpStart < 0) {
        auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
        Dim.Start = Left.Start;
        Dim.End = SE->getMinusSCEV(Right.Start,
            SE->getOne(Right.Start->getType()));
      }
      if (RC && *CmpEnd < 0) {
        auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
        Dim.Start = SE->getAddExpr(Left.End, SE->getOne(Left.End->getType()));
        Dim.End = Right.End;
      }
    } else
      return UnknownIntersection;
    // TODO: difference
  } else if (PairKind == DimPairKind::OneStartOtherEndConst) {
    if (isa<SCEVConstant>(Left.Start)) {
      if (!CmpBC)
        return UnknownIntersection;
      if (*CmpBC >= 0) {
        Intersection.Start = Right.Start;
        Intersection.End = Left.End;
        if (LC) {
          auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
          Dim.Start = Left.Start;
          Dim.End = SE->getMinusSCEV(Right.Start,
            SE->getOne(Right.Start->getType()));
        }
        if (RC) {
          auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
          Dim.Start = SE->getAddExpr(Left.End, SE->getOne(Left.End->getType()));
          Dim.End = Right.End;
        }
      } else {
        return EmptyIntersection;
      }
    } else {
      if (!CmpAD)
        return UnknownIntersection;
      if (*CmpAD <= 0) {
        Intersection.Start = Left.Start;
        Intersection.End = Right.End;
        if (LC) {
          auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
          Dim.Start = Right.Start;
          Dim.End = SE->getMinusSCEV(Left.Start,
              SE->getOne(Left.Start->getType()));
        }
        if (RC) {
          auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
          Dim.Start = SE->getAddExpr(Right.End, SE->getOne(Right.End->getType()));
          Dim.End = Left.End;
        }
      } else
        return EmptyIntersection;
    }
  } else if (PairKind == DimPairKind::BothStartConst) {
    if (!CmpEnd || Left.Start != Right.Start)
      return UnknownIntersection;
    Intersection.End = *CmpEnd < 0 ? Left.End : Right.End;
    if (LC && *CmpEnd > 0) {
      auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
      Dim.Start = SE->getAddExpr(Right.End, SE->getOne(Right.End->getType()));
      Dim.End = Left.End;
    }
    if (RC && *CmpEnd < 0) {
      auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
      Dim.Start = SE->getAddExpr(Left.End, SE->getOne(Left.End->getType()));
      Dim.End = Right.End;
    }
  } else if (PairKind == DimPairKind::BothEndConst) {
    if (!CmpStart || Left.End != Right.End)
      return UnknownIntersection;
    Intersection.Start = *CmpStart > 0 ? Left.Start : Right.Start;
    if (LC && *CmpStart < 0) {
      auto &Dim = LC->emplace_back(LHS).DimList[DimIdx];
      Dim.Start = Left.Start;
      Dim.End = SE->getMinusSCEV(Right.Start,
              SE->getOne(Right.Start->getType()));
    }
    if (RC && *CmpStart > 0) {
      auto &Dim = RC->emplace_back(LHS).DimList[DimIdx];
      Dim.Start = Right.Start;
      Dim.End = SE->getMinusSCEV(Left.Start,
              SE->getOne(Left.Start->getType()));
    }
  } else if (PairKind == DimPairKind::OneConstOtherSemiconst) {
    const Dimension *ConstDim = nullptr, *OtherDim = nullptr;
    if (isa<SCEVConstant>(Left.Start) && isa<SCEVConstant>(Left.End)) {
      ConstDim = &Left;
      OtherDim = &Right;
    } else {
      ConstDim = &Right;
      OtherDim = &Left;
    }
    auto CmpCStartOEnd = compareSCEVs(ConstDim->Start, OtherDim->End, SE);
    auto CmpOStartCEnd = compareSCEVs(OtherDim->Start, ConstDim->End, SE);
    // todo: intersection in one point
    if (CmpCStartOEnd && *CmpCStartOEnd > 0 ||
        CmpOStartCEnd && *CmpOStartCEnd > 0)
      return EmptyIntersection;
    if (CmpCStartOEnd && *CmpCStartOEnd == 0 ||
        CmpOStartCEnd && *CmpOStartCEnd == 0) {
      Intersection.Start = Intersection.End = ConstDim->Start;
      auto ConstLength = compareSCEVs(ConstDim->Start, ConstDim->End, SE);
      assert(ConstLength && "Dimension bounds must be constant!");
      auto ConstC = (ConstDim == &Left) ? LC : RC;
      auto OtherC = (ConstDim == &Left) ? RC : LC;
      if (ConstC && *ConstLength != 0) {
        auto &Dim = ConstC->emplace_back(LHS).DimList[DimIdx];
        if (CmpCStartOEnd && *CmpCStartOEnd == 0) {
          Dim.Start = SE->getAddExpr(ConstDim->Start,
                                    SE->getOne(ConstDim->Start->getType()));
          Dim.End = ConstDim->End;
        } else {
          Dim.Start = ConstDim->Start;
          Dim.End = SE->getMinusSCEV(ConstDim->End,
                                     SE->getOne(ConstDim->End->getType()));
        }
      }
      if (OtherC) {
        auto &Dim = ConstC->emplace_back(LHS).DimList[DimIdx];
        if (CmpCStartOEnd && *CmpCStartOEnd == 0) {
          Dim.Start = ConstDim->Start;
          Dim.End = SE->getMinusSCEV(ConstDim->Start,
                                    SE->getOne(ConstDim->Start->getType()));
        } else {
          Dim.Start = SE->getAddExpr(ConstDim->End,
                                     SE->getOne(ConstDim->End->getType()));
          Dim.End = OtherDim->End;
        }
      }
    } else {
      return UnknownIntersection;
    }
  } else if (PairKind == DimPairKind::OneFullConstOtherFullVariable) {
    return UnknownIntersection;
  } else {
    llvm_unreachable("Invalid kind of dimension with variable bounds.");
  }
  return std::make_pair(llvm::None, false);
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
  auto SE = LHS.SE;
  for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
    auto &Left = LHS.DimList[I];
    auto &Right = RHS.DimList[I];
    if (Left.DimSize != Right.DimSize)
      return MemoryLocationRange();
    auto &Intersection = Int.DimList[I];
    auto PairKind = getDimPairKind(Left, Right);
    LLVM_DEBUG(dbgs() << "[INTERSECT] Pair kind: " <<
               getKindAsString(PairKind) << "\n");
    if (PairKind != DimPairKind::FullConst) {
      auto Res = processVariableBound(
          LHS, Right, Intersection, I, PairKind, LC, RC);
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
