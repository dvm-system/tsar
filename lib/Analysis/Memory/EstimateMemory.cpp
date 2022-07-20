//===--- EstimateMemory.cpp ----- Memory Hierarchy --------------*- C++ -*-===//
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
// This file proposes functionality to construct a program alias tree.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/MemorySetInfo.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "estimate-mem"

STATISTIC(NumAliasNode, "Number of alias nodes created");
STATISTIC(NumEstimateNode, "Number of estimate nodes created");
STATISTIC(NumUnknownNode, "Number of unknown nodes created");
STATISTIC(NumMergedNode, "Number of alias nodes merged in");
STATISTIC(NumEstimateMemory, "Number of estimate memory created");
STATISTIC(NumUnknownMemory, "Number of unknown memory created");

static inline void clarifyUnknownSize(const DataLayout &DL,
    MemoryLocation &Loc, const DominatorTree *DT = nullptr) {
  if (Loc.Size.hasValue())
    return;
  int64_t Offset = 0;
  auto Ptr = GetPointerBaseWithConstantOffset(Loc.Ptr, Offset, DL);
  if (Offset > 0)
    Ptr = Loc.Ptr;
  if (auto GV = dyn_cast<GlobalValue>(Ptr)) {
    auto Ty = GV->getValueType();
    if (Ty->isSized())
      Loc.Size = LocationSize::precise(DL.getTypeStoreSize(Ty));
  } else if (auto AI = dyn_cast<AllocaInst>(Ptr)) {
    auto Ty = AI->getAllocatedType();
    auto Size = AI->getArraySize();
    if (Ty->isSized() && isa<ConstantInt>(Size))
      Loc.Size = LocationSize::precise(
          cast<ConstantInt>(Size)->getValue().getZExtValue() *
          DL.getTypeStoreSize(Ty));
    else if (Loc.Size.mayBeBeforePointer())
      Loc.Size = LocationSize::afterPointer();
  }
  LLVM_DEBUG(if (Loc.Size.hasValue()) {
    dbgs()
        << "[ALIAS TREE]: decrease the location size to the allocated size: ";
    printLocationSource(dbgs(), Loc, DT);
    dbgs() << "\n";
  });
}

namespace tsar {
Value * stripPointer(const DataLayout &DL, Value *Ptr) {
  assert(Ptr && "Pointer to memory location must not be null!");
  Ptr = getUnderlyingObject(Ptr, 0);
  if (auto LI = dyn_cast<LoadInst>(Ptr))
    return stripPointer(DL, LI->getPointerOperand());
  if (Operator::getOpcode(Ptr) == Instruction::IntToPtr) {
    return stripPointer(DL,
      getUnderlyingObject(cast<Operator>(Ptr)->getOperand(0), 0));
  }
  return Ptr;
}

void stripToBase(const DataLayout &DL, MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  // GepUnderlyingObject() will strip `getelementptr` instruction, so ignore such
  // behavior.
  if (auto  GEP = dyn_cast<const GEPOperator>(Loc.Ptr))
    return;
  // We should not strip 'inttoptr' cast because alias analysis for values with
  // no-pointer types always returns NoAlias.
  if (Operator::getOpcode(Loc.Ptr) == Instruction::IntToPtr)
    return;
  // It seams that it is safe to strip 'addrspacecast' and that an alias
  // analysis works well in this case. LLVM IR specification requires that
  // if the address space conversion is legal then both result and operand
  // refer to the same memory location.
  auto BasePtr = getUnderlyingObject(const_cast<Value *>(Loc.Ptr), 1);
  if (BasePtr == Loc.Ptr)
    return;
  Loc.Ptr = BasePtr;
  LLVM_DEBUG(dbgs() << "[ALIAS TREE]: strip offset from base to ";
             printLocationSource(dbgs(), Loc); dbgs() << "\n");
  stripToBase(DL, Loc);
}

bool stripMemoryLevel(const DataLayout &DL, MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  auto Ty = Loc.Ptr->getType();
  if (auto PtrTy = dyn_cast<PointerType>(Ty)) {
    auto *PointeeTy{getPointerElementType(*Loc.Ptr)};
    auto Size = PointeeTy && PointeeTy->isSized()
                    ? LocationSize::precise(
                          DL.getTypeStoreSize(PointeeTy))
                    : LocationSize::beforeOrAfterPointer();
    if (MemorySetInfo<MemoryLocation>::sizecmp(Size, Loc.Size) > 0) {
      auto NewLoc{Loc.getWithNewSize(Size)};
      clarifyUnknownSize(DL, NewLoc);
      return NewLoc.Size != Loc.Size ? Loc = std::move(NewLoc), true : false;
    }
    auto GEP = dyn_cast<const GEPOperator>(Loc.Ptr);
    // It seems that it is safe to not assume unknown size for non-GEP
    // instructions. If this pointer is used as an actual parameter
    // corresponding location with a correct size is built while a call
    // instruction is processed. If there is a GEP instruction which uses
    // this pointer, corresponding location with a correct size is built while
    // GEP is stripped. If there is a cast to integer and arithmetic operations
    // with the casted value, a distinct memory location corresponding to a
    // produced result is build.
#if 0
    if (!GEP) {
      if (Loc.Size.mayBeBeforePointer())
        return false;
      auto NewLoc{Loc.getWithNewSize(!Loc.Size.hasValue()
                                         ? LocationSize::beforeOrAfterPointer()
                                         : LocationSize::afterPointer())};
      clarifyUnknownSize(DL, NewLoc);
      return NewLoc.Size != Loc.Size ? Loc = std::move(NewLoc), true : false;

      return false;
    }
#endif
    if (!GEP)
      return false;
    unsigned ZeroTailIdx = GEP->getNumOperands();
    for (; ZeroTailIdx > 1; --ZeroTailIdx) {
      auto OpC = dyn_cast<ConstantInt>(GEP->getOperand(ZeroTailIdx - 1));
      if (!OpC || !OpC->isZero())
        break;
    }
    if (ZeroTailIdx < GEP->getNumOperands()) {
      unsigned Idx = 1;
      for (auto OpTy = gep_type_begin(GEP), OpTyE = gep_type_end(GEP);
           OpTy != OpTyE; ++OpTy, ++Idx) {
        if (Idx < ZeroTailIdx - 1)
          continue;
        auto ITy = OpTy.getIndexedType();
        if (MemorySetInfo<MemoryLocation>::sizecmp(Loc.Size,
              LocationSize::precise(DL.getTypeStoreSize(ITy))) >= 0) {
          // If ZeroTailIdx == 1 and Idx == 1 we should also break the loop.
          if (Idx == 1 || Idx == ZeroTailIdx - 1) {
            Size = LocationSize::precise(DL.getTypeStoreSize(ITy));
            break;
          }
          if (Idx < GEP->getNumOperands()) {
            LLVM_DEBUG(dbgs()
                       << "[ALIAS TREE]: strip zero offset in GEP from size "
                       << Loc.Size << " to size " << Size << "\n");
            Loc.Size = Size;
            return true;
          }
        }
        Size = LocationSize::precise(DL.getTypeStoreSize(ITy));
      }
    }
    // In case of sequence of GEPs try to subsequently strip all GEPs to build
    // a single estimate memory tree with the whole array as a root.
    // It is not safe to extend GEP memory location if its current size Loc.Size
    // is known and it is greater then size of known subrange (element,
    // size of structure or array dimension) because if we do not known
    // the size of the whole array we can not check that <Loc.Ptr, Loc.Size> is
    // inside <pointer to array, size of array>. However, we assume that
    // unknown size can not be greater than allocated size.
    // Note, if array dimensions have constant size than Size == Loc.Size and
    // level will be striped.
    if (Loc.Size.hasValue() &&
        MemorySetInfo<MemoryLocation>::sizecmp(Size, Loc.Size) >= 0) {
      LLVM_DEBUG(dbgs() << "[ALIAS TREE]: strip GEP to base pointer\n");
      Loc.Ptr = GEP->getPointerOperand();
      Loc.AATags = llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey();
      Type *SrcTy = GEP->getSourceElementType();
      if (SrcTy->isArrayTy() || SrcTy->isStructTy()) {
        auto SrcSize = DL.getTypeStoreSize(SrcTy);
        APInt GEPOffset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
        if (GEP->accumulateConstantOffset(DL, GEPOffset))
          if (Loc.Size.getValue() + GEPOffset.getSExtValue() <= SrcSize) {
            Loc.Size = LocationSize::precise(SrcSize);
            return true;
          }
      }
    }
    for (;;) {
      auto BasePtr = getUnderlyingObject(Loc.Ptr, 0);
      if (BasePtr == Loc.Ptr)
        break;
      Loc.Ptr = BasePtr;
    }
    Loc.Size = Loc.Size.mayBeBeforePointer()
                   ? LocationSize::beforeOrAfterPointer()
                   : LocationSize::afterPointer();
    clarifyUnknownSize(DL, Loc);
    return true;
  }
  return false;
}

bool isSameBase(const DataLayout &DL,
    const llvm::Value *BasePtr1, const llvm::Value *BasePtr2) {
  if (BasePtr1 == BasePtr2)
    return true;
  if (!BasePtr1 || !BasePtr2)
    return false;
  // Try to strip constant offset, to avoid construction of different location
  // for &X and &X + 0 and other similar cases. Different locations produce the
  // same metadata-level location &X, but this location can not be inserted in
  // metadata-level alias tree twice.
  int64_t Offset1 = 0, Offset2 = 0;
  BasePtr1 = GetPointerBaseWithConstantOffset(BasePtr1, Offset1, DL);
  BasePtr2 = GetPointerBaseWithConstantOffset(BasePtr2, Offset2, DL);
  if (Offset1 != Offset2 ||  BasePtr1->getValueID() != BasePtr2->getValueID())
    return false;
  if (BasePtr1 == BasePtr2)
    return true;
  auto Opcode1 = Operator::getOpcode(BasePtr1);
  auto Opcode2 = Operator::getOpcode(BasePtr2);
  // Value::getValueID() does not distinguish instructions (constant
  // expressions) kinds.
  if (Opcode1 != Opcode2 || Opcode1 == Instruction::UserOp1)
    return false;
  if (Opcode1 == Instruction::IntToPtr ||
      Opcode1 == Instruction::BitCast ||
      Opcode1 == Instruction::AddrSpaceCast)
    return isSameBase(DL,
      cast<const Operator>(BasePtr1)->getOperand(0),
      cast<const Operator>(BasePtr2)->getOperand(0));
  if (auto LI = dyn_cast<const LoadInst>(BasePtr1))
    return isSameBase(DL, LI->getPointerOperand(),
      cast<const LoadInst>(BasePtr2)->getPointerOperand());
  return false;
}

AliasDescriptor aliasRelation(AAResults &AA, const DataLayout &DL,
    const MemoryLocation &LHS, const MemoryLocation &RHS) {
  AliasDescriptor Dptr;
  auto AR = AA.alias(
    isAAInfoCorrupted(LHS.AATags) ? LHS.getWithoutAATags() : LHS,
    isAAInfoCorrupted(RHS.AATags) ? RHS.getWithoutAATags() : RHS);
  switch (AR) {
  default: llvm_unreachable("Unknown result of alias analysis!");
  case AliasResult::NoAlias: Dptr.set<trait::NoAlias>(); break;
  case AliasResult::MayAlias: Dptr.set<trait::MayAlias>(); break;
  case AliasResult::PartialAlias:
    {
      Dptr.set<trait::PartialAlias>();
      // Now we try to prove that one location covers other location.
      if (LHS.Size == RHS.Size ||
          !LHS.Size.isPrecise() || !RHS.Size.isPrecise())
        break;
      int64_t OffsetLHS, OffsetRHS;
      auto BaseLHS = GetPointerBaseWithConstantOffset(LHS.Ptr, OffsetLHS, DL);
      auto BaseRHS = GetPointerBaseWithConstantOffset(RHS.Ptr, OffsetRHS, DL);
      if (OffsetLHS == 0 && OffsetRHS == 0)
        break;
      auto BaseAlias = AA.alias(
        BaseLHS, LocationSize::afterPointer(),
        BaseRHS, LocationSize::afterPointer());
      // It is possible to precisely compare two partially overlapped
      // locations in case of the same base pointer only.
      if (BaseAlias != AliasResult::MustAlias)
        break;
      if (OffsetLHS < OffsetRHS &&
          OffsetLHS + LHS.Size.getValue() >= OffsetRHS + RHS.Size.getValue())
        Dptr.set<trait::CoverAlias>();
      else if (OffsetLHS > OffsetRHS &&
          OffsetLHS + LHS.Size.getValue() <= OffsetRHS + RHS.Size.getValue())
        Dptr.set<trait::ContainedAlias>();
    }
    break;
  case AliasResult::MustAlias:
    Dptr.set<trait::MustAlias>();
    if (!LHS.Size.isPrecise() || !RHS.Size.isPrecise()) {
      // It is safe to compare sizes, which are not precise, if pointers are
      // only the same.
      int64_t OffsetLHS, OffsetRHS;
      auto BaseLHS = GetPointerBaseWithConstantOffset(LHS.Ptr, OffsetLHS, DL);
      auto BaseRHS = GetPointerBaseWithConstantOffset(RHS.Ptr, OffsetRHS, DL);
      if (OffsetLHS == OffsetRHS && BaseLHS == BaseRHS) {
        auto Cmp = MemorySetInfo<MemoryLocation>::sizecmp(LHS.Size, RHS.Size);
        if (Cmp == 0)
          Dptr.set<trait::CoincideAlias>();
        else if (Cmp > 0)
          Dptr.set<trait::CoverAlias>();
        else
          Dptr.set<trait::ContainedAlias>();
      }
      break;
    }
    if (LHS.Size == RHS.Size)
      Dptr.set<trait::CoincideAlias>();
    else if (LHS.Size.getValue() > RHS.Size.getValue())
      Dptr.set<trait::CoverAlias>();
    else
      Dptr.set<trait::ContainedAlias>();
    break;
  }
  return Dptr;
}

AliasDescriptor aliasRelation(AAResults &AA, const DataLayout &DL,
    const EstimateMemory &LHS, const EstimateMemory &RHS) {
  auto MergedAD = aliasRelation(AA, DL,
    MemoryLocation(LHS.front(), LHS.getSize(), LHS.getAAInfo()),
    MemoryLocation(RHS.front(), RHS.getSize(), RHS.getAAInfo()));
  if (MergedAD.is<trait::MayAlias>())
    return MergedAD;
  for (auto PtrLHS: LHS)
    for (auto PtrRHS : RHS) {
      auto AD = aliasRelation(AA, DL,
        MemoryLocation(PtrLHS, LHS.getSize(), LHS.getAAInfo()),
        MemoryLocation(PtrRHS, RHS.getSize(), RHS.getAAInfo()));
      MergedAD = mergeAliasRelation(MergedAD, AD);
      if (MergedAD.is<trait::MayAlias>())
        return MergedAD;
    }
  return MergedAD;
}

const EstimateMemory * ancestor(
    const EstimateMemory *LHS, const EstimateMemory *RHS) noexcept {
  for (auto EM = LHS; EM; EM = EM->getParent())
    if (EM == RHS)
      return RHS;
  for (auto EM = RHS; EM; EM = EM->getParent())
    if (EM == LHS)
      return LHS;
  return nullptr;
}

AliasDescriptor mergeAliasRelation(
    const AliasDescriptor &LHS, const AliasDescriptor &RHS) {
  assert((LHS.is<trait::NoAlias>() || LHS.is<trait::MayAlias>() ||
    LHS.is<trait::PartialAlias>() || LHS.is<trait::MustAlias>()) &&
    "Alias results must be set!");
  assert((RHS.is<trait::NoAlias>() || RHS.is<trait::MayAlias>() ||
    RHS.is<trait::PartialAlias>() || RHS.is<trait::MustAlias>()) &&
    "Alias results must be set!");
  if (LHS == RHS)
    return LHS;
  // Now we know that for LHS and RHS is not set NoAlias.
  AliasDescriptor ARLHS(LHS), ARRHS(RHS);
  ARLHS.unset<trait::CoincideAlias, trait::ContainedAlias, trait::CoverAlias>();
  ARRHS.unset<trait::CoincideAlias, trait::ContainedAlias, trait::CoverAlias>();
  if (ARLHS == ARRHS) {
    // ARLHS and ARRHS is both MustAlias or PartialAlias.
    if (LHS.is<trait::CoincideAlias>() || RHS.is<trait::CoincideAlias>())
      ARLHS.set<trait::CoincideAlias>();
    if (LHS.is<trait::ContainedAlias>() && RHS.is<trait::CoverAlias>() ||
      LHS.is<trait::CoverAlias>() && RHS.is<trait::ContainedAlias>())
      ARLHS.unset<
        trait::CoincideAlias, trait::CoverAlias, trait::ContainedAlias>();
    return ARLHS;
  }
  AliasDescriptor Dptr;
  if (LHS.is<trait::PartialAlias>() && RHS.is<trait::MustAlias>() ||
    LHS.is<trait::MustAlias>() && RHS.is<trait::PartialAlias>()) {
    // If MustAlias and PartialAlias are merged then PartialAlias is obtained.
    Dptr.set<trait::PartialAlias>();
    if (LHS.is<trait::CoincideAlias>() || RHS.is<trait::CoincideAlias>())
      Dptr.set<trait::CoincideAlias>();
    if (LHS.is<trait::ContainedAlias>() && RHS.is<trait::CoverAlias>() ||
      LHS.is<trait::CoverAlias>() && RHS.is<trait::ContainedAlias>())
      Dptr.unset<
        trait::CoincideAlias, trait::CoverAlias, trait::ContainedAlias>();
  } else {
    // Otherwise, we do not know anything.
    Dptr.set<trait::MayAlias>();
  }
  return Dptr;
}
}

const AliasEstimateNode * EstimateMemory::getAliasNode(
    const AliasTree &G) const {
  if (mNode && mNode->isForwarding()) {
    auto *OldNode = mNode;
    mNode = cast<AliasEstimateNode>(OldNode->getForwardedTarget(G));
    mNode->retain();
    OldNode->release(G);
  }
  return mNode;
}

const AliasNode * AliasNode::getParent(const AliasTree &G) const {
  if (mParent && mParent->isForwarding()) {
    auto *OldNode = mParent;
    mParent = OldNode->getForwardedTarget(G);
    mParent->retain();
    OldNode->release(G);
  }
  return mParent;
}

namespace {
#ifndef NDEBUG
void evaluateMemoryLevelLog(const MemoryLocation &Loc,
    const DominatorTree &DT) {
  dbgs() << "[ALIAS TREE]: evaluate memory level ";
  printLocationSource(dbgs(), Loc, &DT);
  dbgs() << "\n";
}

void updateEMTreeLog(EstimateMemory *EM,
    bool IsNew, bool AddAmbiguous, const DominatorTree &DT) {
  using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
  dbgs() << "[ALIAS TREE]: update estimate memory location tree:";
  dbgs() << " IsNew=" << (IsNew ? "true" : "false");
  dbgs() << " AddAmbiguous=" << (AddAmbiguous ? "true" : "false");
  dbgs() << " Neighbors={ ";
  if (auto PrevEM = CT::getPrev(EM))
    printLocationSource(dbgs(),
      MemoryLocation(PrevEM->front(), PrevEM->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << " ";
  if (auto NextEM = CT::getNext(EM))
    printLocationSource(dbgs(),
      MemoryLocation(NextEM->front(), NextEM->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << " }";
  dbgs() << " Parent=";
  if (auto Parent = EM->getParent())
    printLocationSource(dbgs(),
      MemoryLocation(Parent->front(), Parent->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << " Children={ ";
  if (EM->isLeaf()) {
    dbgs() << "NULL ";
  } else {
    for (auto &Child : make_range(EM->child_begin(), EM->child_end())) {
      printLocationSource(dbgs(),
        MemoryLocation(Child.front(), Child.getSize()), &DT);
      dbgs() << " ";
    }
  }
  dbgs() << "}\n";
}

void mergeChainBeforeLog(EstimateMemory *EM, EstimateMemory *To,
    const DominatorTree &DT) {
  dbgs() << "[ALIAS TREE]: merge location ";
  printLocationSource(dbgs(), MemoryLocation(EM->front(), EM->getSize()), &DT);
  dbgs() << " to the end of ";
  printLocationSource(dbgs(),
    MemoryLocation(To->front(), To->getSize()), &DT);
}

void mergeChainAfterLog(EstimateMemory *EM, const DominatorTree &DT) {
  using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
  dbgs() << ": Neighbors={";
  if (auto PrevEM = CT::getPrev(EM))
    printLocationSource(dbgs(),
      MemoryLocation(PrevEM->front(), PrevEM->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << " ";
  if (auto NextEM = CT::getNext(EM))
    printLocationSource(dbgs(),
      MemoryLocation(NextEM->front(), NextEM->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << " }";
  dbgs() << " Parent=";
  if (auto Parent = EM->getParent())
    printLocationSource(dbgs(),
      MemoryLocation(Parent->front(), Parent->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << " Children={ ";
  if (EM->isLeaf()) {
    dbgs() << "NULL ";
  } else {
    for (auto &Child : make_range(EM->child_begin(), EM->child_end())) {
      printLocationSource(dbgs(),
        MemoryLocation(Child.front(), Child.getSize()), &DT);
      dbgs() << " ";
    }
  }
  dbgs() << "}\n";
}
#endif
}

void AliasTree::add(const MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  assert(!isa<UndefValue>(Loc.Ptr) && "Pointer to memory location must be valid!");
  LLVM_DEBUG(dbgs() << "[ALIAS TREE]: add memory location ";
             printLocationSource(dbgs(), Loc, mDT); dbgs() << "\n");
  mSearchCache.clear();
  using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
  MemoryLocation Base(Loc);
  EstimateMemory *PrevChainEnd = nullptr;
  stripToBase(*mDL, Base);
  clarifyUnknownSize(*mDL, Base, mDT);
  do {
    LLVM_DEBUG(evaluateMemoryLevelLog(Base, getDomTree()));
    stripToBase(*mDL, Base);
    // Do not clarify unknown size if memory level has been stripped.
    // If we insert child location with unknown size then size of its parrent
    // cannot be known.
    EstimateMemory *EM;
    bool IsNew, AddAmbiguous;
    std::tie(EM, IsNew, AddAmbiguous) = insert(Base);
    EM->setExplicit(EM->isExplicit() || !PrevChainEnd);
    LLVM_DEBUG(updateEMTreeLog(EM, IsNew, AddAmbiguous, getDomTree()));
    assert(EM && "New estimate memory must not be null!");
    if (PrevChainEnd && !PrevChainEnd->isSameBase(*EM)) {
      assert((!PrevChainEnd->getParent() || PrevChainEnd->getParent() == EM) &&
        "Inconsistent parent of a node in estimate memory tree!");
      LLVM_DEBUG(mergeChainBeforeLog(EM, PrevChainEnd, getDomTree()));
      CT::mergeNext(EM, PrevChainEnd);
      LLVM_DEBUG(mergeChainAfterLog(EM, getDomTree()));
    }
    if (!IsNew && !AddAmbiguous) {
      if (PrevChainEnd == EM) {
        LLVM_DEBUG(dbgs() << "[ALIAS TREE]: skip memory level processing\n");
        continue;
      }
      LLVM_DEBUG(dbgs() << "[ALIAS TREE]: end memory levels processing\n");
      return;
    }
    PrevChainEnd = EM;
    while (CT::getNext(PrevChainEnd))
      PrevChainEnd = CT::getNext(PrevChainEnd);
    if (AddAmbiguous) {
      /// TODO (kaniandr@gmail.com): optimize duplicate search.
      if (IsNew) {
        auto Node = addEmptyNode(*EM, *getTopLevelNode());
        EM->setAliasNode(*Node, *this);
      }
      while (CT::getPrev(EM))
        EM = CT::getPrev(EM);
      do {
        auto Node = addEmptyNode(*EM, *getTopLevelNode());
        AliasNode *Forward = EM->getAliasNode(*this);
        assert(Forward && "Alias node for memory location must not be null!");
        SmallVector<AliasUnknownNode *, 2> UnknownNodes;
        while (Forward != Node) {
          auto Parent = Forward->getParent(*this);
          assert(Parent && "Parent node must not be null!");
          if (auto UN = dyn_cast<AliasUnknownNode>(Parent)) {
            if (!UnknownNodes.empty())
              UnknownNodes.back()->setParent(*UN, *this);
            else
              UN->retain();
            UnknownNodes.push_back(UN);
            Forward->setParent(*Parent->getParent(*this), *this);
            continue;
          }
          Parent->mergeNodeIn(*Forward, *this), ++NumMergedNode;
          Forward = Parent;
        }
        if (!UnknownNodes.empty()) {
          while (Forward != getTopLevelNode() || isa<AliasUnknownNode>(Forward))
            Forward = Forward->getParent(*this);
          if (Forward == getTopLevelNode()) {
            UnknownNodes.push_back(
              make_node<AliasUnknownNode, llvm::Statistic, 2> (
                *getTopLevelNode(), {&NumAliasNode, &NumUnknownNode}));
          }
          auto UI = UnknownNodes.begin(), UE = UnknownNodes.end();
          auto ForwardUI = UI;
          auto FirstForward = *ForwardUI;
          for (++UI; UI != UE; ForwardUI = UI++, ++NumMergedNode)
            (*UI)->mergeNodeIn(**ForwardUI, *this);
          // The following release() is a pair for retain() in a loop which
          // builds UnknownNodes collection.
          FirstForward->release(*this);
        }
        EM = CT::getNext(EM);
      } while (EM);
    } else {
      auto *CurrNode = CT::getNext(EM) ?
        CT::getNext(EM)->getAliasNode(*this) : getTopLevelNode();
      auto Node = addEmptyNode(*EM, *CurrNode);
      EM->setAliasNode(*Node, *this);
    }
  } while (stripMemoryLevel(*mDL, Base));
  LLVM_DEBUG(dbgs() << "[ALIAS TREE]: end memory levels processing\n");
}

void tsar::AliasTree::addUnknown(llvm::Instruction *I) {
  assert(I && "Instruction which accesses unknown memory must not be null!");
  LLVM_DEBUG(dbgs() << "[ALIAS TREE]: add unknown memory location\n");
  if (auto *II = dyn_cast<IntrinsicInst>(I))
    if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
        isDbgInfoIntrinsic(II->getIntrinsicID()))
      return;
  /// Calls which does not access memory are not ignored because this calls
  /// may access addresses of some memory locations (which is not known) and
  /// such address accesses should be underlined in analysis results.
  ///
  /// Is it safe to ignore intrinsics here? It seems that all intrinsics in
  /// LLVM does not use addresses to perform  computations instead of
  /// memory accesses (see, PrivateAnalysis pass for details).
  if ((!isa<CallBase>(I) || isa<IntrinsicInst>(I)) &&
      !I->mayReadOrWriteMemory())
    return;
  SmallVector<AliasNode *, 4> UnknownAliases, EstimateAliases;
  auto Children = make_range(
    getTopLevelNode()->child_begin(), getTopLevelNode()->child_end());
  for (auto &Child : Children) {
    auto AR = Child.slowMayAliasUnknown(I, *mAA);
    if (AR.first)
      if (AR.second == I)
        return;
      else if (isa<AliasUnknownNode>(Child))
        UnknownAliases.push_back(&Child);
      else
        EstimateAliases.push_back(&Child);
  }
  mSearchCache.clear();
  AliasUnknownNode *Node;
  if (!UnknownAliases.empty()) {
    auto AI = UnknownAliases.begin(), EI = UnknownAliases.end();
    Node = cast<AliasUnknownNode>(*AI);
    for (++AI; AI != EI; ++AI, ++NumMergedNode)
      Node->mergeNodeIn(**AI, *this);
  } else {
    Node = make_node<AliasUnknownNode, llvm::Statistic, 2>(
      *getTopLevelNode(), { &NumAliasNode, &NumUnknownNode });
  }
  for (auto Alias : EstimateAliases)
      Alias->setParent(*Node, *this);
  if (Node->empty())
    Node->retain();
  Node->push_back(I), ++NumUnknownMemory;
}

void AliasTree::removeNode(AliasNode *N) {
  if (auto *Fwd = N->mForward) {
    Fwd->release(*this);
    N->mForward = nullptr;
  }
  mNodes.erase(N);
}

std::pair<bool, Instruction *>
AliasEstimateNode::slowMayAliasUnknownImp(
    const Instruction *I, AAResults &AA) const {
  assert(I && "Instruction must not be null!");
  for (auto &EM : *this) {
    for (auto *Ptr : EM)
      if (AA.getModRefInfo(I, MemoryLocation(Ptr, EM.getSize(), EM.getAAInfo()))
          != ModRefInfo::NoModRef)
        return std::make_pair(true, nullptr);
  }
  return std::make_pair(false, nullptr);
}

std::pair<bool, Instruction *>
AliasUnknownNode::slowMayAliasUnknownImp(
    const Instruction *I, AAResults &AA) const {
  assert(I && "Instruction must not be null!");
  if (mUnknownInsts.count(const_cast<Instruction *>(I)))
    return std::make_pair(true, const_cast<Instruction *>(I));
  for (auto *UI : *this) {
    auto *C1 = dyn_cast<CallBase>(UI);
    auto *C2 = dyn_cast<CallBase>(I);
    if (!C1 || !C2 || AA.getModRefInfo(C1, C2) != ModRefInfo::NoModRef ||
      AA.getModRefInfo(C2, C1) != ModRefInfo::NoModRef)
      return std::make_pair(true, UI);
  }
  return std::make_pair(false, nullptr);
}

std::pair<bool, EstimateMemory *>
AliasEstimateNode::slowMayAliasImp(const EstimateMemory &EM, AAResults &AA) {
  for (auto &ThisEM : *this)
    for (auto *LHSPtr : ThisEM)
      for (auto *RHSPtr : EM) {
        auto AR = AA.alias(
          MemoryLocation(LHSPtr, ThisEM.getSize(), ThisEM.getAAInfo()),
          MemoryLocation(RHSPtr, EM.getSize(), EM.getAAInfo()));
        if (AR == AliasResult::NoAlias)
          continue;
        return std::make_pair(true, &ThisEM);
      }
  return std::make_pair(false, nullptr);
}

std::pair<bool, EstimateMemory *>
AliasUnknownNode::slowMayAliasImp(const EstimateMemory &EM, AAResults &AA) {
  for (auto *UI : *this) {
    for (auto *Ptr : EM)
      if (AA.getModRefInfo(UI, MemoryLocation(Ptr, EM.getSize(), EM.getAAInfo()))
          != ModRefInfo::NoModRef)
        return std::make_pair(true, nullptr);
  }
  return std::make_pair(false, nullptr);
}

AliasEstimateNode * AliasTree::addEmptyNode(
    const EstimateMemory &NewEM,  AliasNode &Start) {
  auto Current = &Start;
  SmallPtrSet<const AliasNode *, 8> ChildrenNodes;
  for (auto I = NewEM.child_begin(), E = NewEM.child_end(); I != E; ++I)
    ChildrenNodes.insert(I->getAliasNode(*this));
  SmallVector<PointerUnion<EstimateMemory *, AliasNode *>, 4> Aliases;
  auto getAliasNode = [this](decltype(Aliases)::reference Ptr) {
    return Ptr.is<AliasNode *>() ? Ptr.get<AliasNode *>() :
      Ptr.get<EstimateMemory *>()->getAliasNode(*this);
  };
  for (;;) {
    // This condition is necessary due to alias node which contains full memory
    // should not be descendant of a node which contains part of this memory.
    if (ChildrenNodes.count(Current))
      return cast<AliasEstimateNode>(Current);
    Aliases.clear();
    for (auto &Ch : make_range(Current->child_begin(), Current->child_end())) {
      auto Result = Ch.slowMayAlias(NewEM, *mAA);
      if (Result.first) {
        if (Result.second)
          Aliases.push_back(Result.second);
        else
          Aliases.push_back(&Ch);
      } else if (isa<AliasUnknownNode>(Ch)) {
        // If unknown node does not alias with a memory it does not mean
        // that its children nodes do not alias with this memory. The issue is
        // that unknown node may not cover its children nodes.
        for (auto &N : make_range(Ch.child_begin(), Ch.child_end())) {
          auto Result = N.slowMayAlias(NewEM, *mAA);
          if (Result.first) {
            Aliases.push_back(&Ch);
            break;
          }
        }
      }
    }
    if (Aliases.empty())
      return make_node<AliasEstimateNode, llvm::Statistic, 2>(
        *Current, {&NumAliasNode, &NumEstimateNode});
    if (Aliases.size() == 1) {
      if (Aliases.front().is<AliasNode *>()) {
        Current = Aliases.front().get<AliasNode *>();
        continue;
      }
      auto EM = Aliases.front().get<EstimateMemory *>();
      auto Node = EM->getAliasNode(*this);
      assert(Node && "Alias node for memory location must not be null!");
      auto AD = aliasRelation(
        *mAA, *mDL, NewEM, AliasEstimateNode::iterator(EM), Node->end());
      if (AD.is<trait::CoverAlias>()) {
        auto *NewNode = make_node<AliasEstimateNode, llvm::Statistic, 2>(
          *Current, {&NumAliasNode, &NumEstimateNode});
        Node->setParent(*NewNode, *this);
        return NewNode;
      }
      if (!AD.is<trait::ContainedAlias>())
        return Node;
      Current = Node;
      continue;
    }
    auto I = Aliases.begin(), EI = Aliases.end();
    for (; I != EI; ++I) {
      if (I->is<EstimateMemory *>()) {
        auto EM = I->get<EstimateMemory *>();
        auto Node = EM->getAliasNode(*this);
        assert(Node && "Alias node for memory location must not be null!");
        auto AD = aliasRelation(*mAA, *mDL, NewEM, Node->begin(), Node->end());
        if (AD.is<trait::CoverAlias>() ||
            (AD.is<trait::CoincideAlias>() && !AD.is<trait::ContainedAlias>()))
          continue;
      }
      break;
    }
    if (I == EI) {
      auto *NewNode = make_node<AliasEstimateNode, llvm::Statistic, 2>(
          *Current, {&NumAliasNode, &NumEstimateNode});
      for (auto &Alias : Aliases)
        getAliasNode(Alias)->setParent(*NewNode, *this);
      return NewNode;
    }
    // If the new estimate location aliases with locations from different
    // alias nodes at the same level and does not cover (or coincide with)
    // memory described by this nodes, this nodes should be merged.
    // If the new estimate location aliases with some unknown memory
    // this nodes should be merged also. However, in this case it is possible
    // to go down to the next level in the alias tree.
    I = Aliases.begin(), EI = Aliases.end();
    Current = getAliasNode(*I);
    AliasNode *Opposite = nullptr;
    for (++I; I != EI; ++I, ++NumMergedNode) {
      auto *Node = getAliasNode(*I);
      if (Node->getKind() == Current->getKind())
        Current->mergeNodeIn(*Node, *this);
      else if (Opposite)
        Opposite->mergeNodeIn(*Node, *this);
      else
        Opposite = Node;
    }
    if (!Opposite) {
      if (isa<AliasUnknownNode>(Current))
        continue;
    } else {
      if (!isa<AliasUnknownNode>(Opposite))
        std::swap(Current, Opposite);
      Current->setParent(*Opposite, *this);
    }
    return cast<AliasEstimateNode>(Current);
  }
}

AliasResult AliasTree::isSamePointer(
    const EstimateMemory &EM, const MemoryLocation &Loc) const {
  auto LocAATags = sanitizeAAInfo(Loc.AATags);
  bool IsAmbiguous = false;
  for (auto *Ptr : EM) {
    switch (mAA->alias(
        MemoryLocation(Ptr, 1, EM.getAAInfo()),
        MemoryLocation(Loc.Ptr, 1, LocAATags))) {
      case AliasResult::MustAlias: return AliasResult::MustAlias;
      case AliasResult::MayAlias: IsAmbiguous = true; break;
    }
  }
  return IsAmbiguous ? AliasResult::MayAlias : AliasResult::NoAlias;
}

const AliasUnknownNode * AliasTree::findUnknown(
    const llvm::Instruction &I) const {
  auto Children = make_range(
    getTopLevelNode()->child_begin(), getTopLevelNode()->child_end());
  for (auto &Child : Children) {
    if (!isa<AliasUnknownNode>(Child))
      continue;
    for (auto *UI : cast<AliasUnknownNode>(Child)) {
      if (&I == UI)
        return &cast<AliasUnknownNode>(Child);
    }
  }
  return nullptr;
}

const EstimateMemory * AliasTree::find(const llvm::MemoryLocation &Loc) const {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  MemoryLocation Base(Loc);
  stripToBase(*mDL, Base);
  clarifyUnknownSize(*mDL, Base, mDT);
  auto SearchInfo = mSearchCache.try_emplace(Base, nullptr);
  if (!SearchInfo.second)
    return SearchInfo.first->second;
  Value *StrippedPtr = stripPointer(*mDL, const_cast<Value *>(Base.Ptr));
  auto I = mBases.find(StrippedPtr);
  if (I == mBases.end())
    return nullptr;
  // If the most appropriate result is not found, the first one is returned.
  EstimateMemory *FirstResult = nullptr;
  for (auto &ChainBegin : I->second) {
    auto Chain = ChainBegin;
    if (!isSameBase(*mDL, Chain->front(), Base.Ptr))
      continue;
    switch (isSamePointer(*Chain, Base)) {
    case AliasResult::NoAlias: continue;
    case AliasResult::MustAlias: break;
    case AliasResult::MayAlias:
      llvm_unreachable("It seems that something goes wrong or memory location"\
                       "was not added to alias tree!"); break;
    default:
      llvm_unreachable("Unknown result of alias query!"); break;
    }
    using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
    EstimateMemory *Prev = nullptr, *Result = nullptr;
    do {
      if (MemorySetInfo<MemoryLocation>::sizecmp(
            Base.Size, Chain->getSize()) > 0)
        continue;
      if (!Result)
        Result = Chain;
    } while (Prev = Chain, Chain = CT::getNext(Chain));
    if (!Result)
      continue;
    if (!FirstResult)
      FirstResult = Result;
    bool IsStripedBase = false;
    auto StripedBase = Base;
    do {
      IsStripedBase = stripMemoryLevel(*mDL, StripedBase);
    } while (IsStripedBase && isSameBase(*mDL, StripedBase.Ptr, Base.Ptr));
    if (IsStripedBase) {
      if (auto Parent = Prev->getParent()) {
        auto SizeCmp = MemorySetInfo<MemoryLocation>::sizecmp(
          Parent->getSize(), StripedBase.Size);
        if (!isSameBase(*mDL, Parent->front(), StripedBase.Ptr) ||
            SizeCmp < 0 || SizeCmp > 0 && Result == Prev)
          continue;
      } else {
        if (MemorySetInfo<MemoryLocation>::sizecmp(Prev->getSize(),
                                                   StripedBase.Size) > 0)
          continue;
      }
    }
    SearchInfo.first->second = Result;
    return Result;
  }
  SearchInfo.first->second = FirstResult;
  return FirstResult;
}

std::tuple<EstimateMemory *, bool, bool>
AliasTree::insert(const MemoryLocation &Base) {
  assert(Base.Ptr && "Pointer to memory location must not be null!");
  Value *StrippedPtr = stripPointer(*mDL, const_cast<Value *>(Base.Ptr));
  BaseList *BL;
  auto I = mBases.find(StrippedPtr);
  if (I != mBases.end()) {
    BL = &I->second;
    for (auto &ChainBegin : *BL) {
      auto Chain = ChainBegin;
      if (!isSameBase(*mDL, Chain->front(), Base.Ptr))
        continue;
      /// TODO (kaniandr@gamil.com): The following case is possible:
      /// p = &x; ... p = &y;
      /// In this case single estimate memory locations will be created at this
      /// (*(p = &x), *(p = &y)). This is permissible but not good because
      /// it is not possible to analyze &x and &y separately. In case of
      /// creation of two locations there are also some problems.
      /// Let us consider the following case:
      /// if (...) { p = &x;} else { p = &y;} *p = ...
      /// It is not also possible to create (*(p = &x), *p) and (*(p = &y), *p)
      /// because *p must not have two different estimate memory locations.
      /// So, this two locations should be merged: (*(p = &x), *p, *(p = &y))
      /// and all chains of locations also should be merged. After that alias
      /// tree should be updated. The last operation is not simple.
      /// The worst case is if estimate memory locations for p and p does not
      /// alias but for striped bases they alias. In this case it is not
      /// trivially to update alias tree.
      /// So the more accurate creation of estimate memory locations will
      /// be implemented later.
      bool AddAmbiguous = false;
      switch (isSamePointer(*Chain, Base)) {
      default: llvm_unreachable("Unknown result of alias query!"); break;
      // TODO(kaniandr@gmail.com): Is it correct to ignore NoAlias? Algorithm
      // should be accurately explored to understand this case.
      case AliasResult::NoAlias: continue;
      case AliasResult::MustAlias: break;
      case AliasResult::MayAlias:
        AddAmbiguous = true;
        Chain->getAmbiguousList()->push_back(Base.Ptr);
        break;
      }
      using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
      EstimateMemory *Prev = nullptr, *UpdateChain = nullptr;
      do {
        if (MemorySetInfo<MemoryLocation>::sizecmp(
              Base.Size, Chain->getSize()) <= 0 &&
            !UpdateChain)
          UpdateChain = Chain;
      } while (Prev = Chain, Chain = CT::getNext(Chain));
      // This condition checks that it is safe to insert a specified location
      // Base into the estimate memory tree which contains location Chain.
      // The following cases lead to building new estimate memory tree:
      // 1. Base and Chain are x.y.z but striped locations are x.y and x.
      // It is possible due to implementation of stripMemoryLevel() function.
      // 2. Inconsistent sizes after execution of stripMemrmoyLevel() function.
      bool IsStripedBase = false;
      auto StripedBase = Base;
      do {
        IsStripedBase = stripMemoryLevel(*mDL, StripedBase);
      } while (IsStripedBase && isSameBase(*mDL, StripedBase.Ptr, Base.Ptr));
      if (IsStripedBase) {
        // For example, let's consider a chain <dum[1], 8> with parent
        // <dum, 24>-<dum, ?>. Try to insert <dum[1], ?>. If we insert it after
        // <dum[1], 8> we will obtain incorrect sequence of sizes (? < 24)
        // <dum[1], 8>-<dum[1],?>-<dum, 24>-<dum, ?>. So, we should create a new
        // chain <dum[1],?> with parent <dum,?>.
        if (auto Parent = Prev->getParent()) {
          auto SizeCmp = MemorySetInfo<MemoryLocation>::sizecmp(
            Parent->getSize(), StripedBase.Size);
          if (!isSameBase(*mDL, Parent->front(), StripedBase.Ptr) ||
              SizeCmp < 0 || SizeCmp > 0 && !UpdateChain)
            continue;
        } else {
          // For example, let's consider a chain <dum[2],?> with parent <dum,
          // ?>. Try to insert <dum[2], 8>, note that stripMemoryLevel() returns
          // <dum, 24>. So, if we insert it before <dum[2],?> we will obtain
          // incorrect sequence of sizes after insertion its parent <dum, 24>:
          // <dum[2],8>-<dum[2],?>-<dum,24>-<dum,?>.
          if (MemorySetInfo<MemoryLocation>::sizecmp(Prev->getSize(),
                                                     StripedBase.Size) > 0)
            continue;
        }
      }
      if (!UpdateChain) {
        auto EM = new EstimateMemory(*Prev, Base.Size, Base.AATags);
        ++NumEstimateMemory;
        CT::spliceNext(EM, Prev);
        return std::make_tuple(EM, true, AddAmbiguous);
      }
      if (MemorySetInfo<MemoryLocation>::sizecmp(Base.Size,
                                                 UpdateChain->getSize()) == 0) {
        UpdateChain->updateAAInfo(Base.AATags);
        return std::make_tuple(UpdateChain, false, AddAmbiguous);
      }
      assert(MemorySetInfo<MemoryLocation>::sizecmp(
               Base.Size, UpdateChain->getSize()) < 0 && "Invariant broken!");
      auto EM = new EstimateMemory(*UpdateChain, Base.Size, Base.AATags);
      ++NumEstimateMemory;
      CT::splicePrev(EM, UpdateChain);
      if (ChainBegin == UpdateChain)
        ChainBegin = EM; // update start point of this chain in a base list
      return std::make_tuple(EM, true, AddAmbiguous);
    }
  } else {
    BL = &mBases.insert(std::make_pair(StrippedPtr, BaseList())).first->second;
  }
  LLVM_DEBUG(dbgs() << "[ALIAS TREE]: build new chain\n");
  auto Chain = new EstimateMemory(Base, AmbiguousRef::make(mAmbiguousPool));
  ++NumEstimateMemory;
  BL->push_back(Chain);
  return std::make_tuple(Chain, true, false);
}

char EstimateMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(EstimateMemoryPass, "estimate-mem",
  "Memory Estimator", false, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(EstimateMemoryPass, "estimate-mem",
  "Memory Estimator", false, true)

void EstimateMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<AAResultsWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createEstimateMemoryPass() {
  return new EstimateMemoryPass();
}

bool EstimateMemoryPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto M = F.getParent();
  auto &DL = M->getDataLayout();
  mAliasTree = new AliasTree(AA, DL, DT);
  DenseSet<const Value *> AccessedMemory, AccessedUnknown;
  auto addLocation = [&AccessedMemory, this](MemoryLocation &&Loc) {
    AccessedMemory.insert(Loc.Ptr);
    mAliasTree->add(Loc);
  };
  auto addUnknown = [&AccessedUnknown, this](Instruction &I) {
    AccessedUnknown.insert(&I);
    mAliasTree->addUnknown(&I);
  };
  for_each_memory(F, TLI, [&DL, &addLocation](
    Instruction &I, MemoryLocation &&Loc, unsigned, AccessInfo, AccessInfo) {
      addLocation(std::move(Loc));
  },
    [&addUnknown](Instruction &I, AccessInfo, AccessInfo) {
      addUnknown(I);
  });
  // If there are some pointers to memory locations which are not accessed
  // then such memory locations also should be inserted in the alias tree.
  // To avoid destruction of metadata for locations which have been already
  // inserted into the alias tree `AccessedMemory` set is used.
  //
  // TODO (kaniandr@gmail.com): `AccessedMemory` does not contain locations
  // which have been added implicitly. For example, if stripMemoryLevel() has
  // been called.
  auto addPointeeIfNeed = [&DL, &AccessedMemory, &addLocation, &F](
      const Value *V) {
    if (isa<UndefValue>(V))
      return;
    if (!V->getType() || !V->getType()->isPointerTy())
      return;
    if (const auto *CPN = dyn_cast<ConstantPointerNull>(V))
      if (!NullPointerIsDefined(&F, CPN->getType()->getAddressSpace()))
        return;
    if (auto F = dyn_cast<Function>(V))
      if (isDbgInfoIntrinsic(F->getIntrinsicID()) ||
          isMemoryMarkerIntrinsic(F->getIntrinsicID()))
        return;
    if (AccessedMemory.count(V))
      return;
    auto PointeeTy{getPointerElementType(*V)};
    addLocation(MemoryLocation(
        V, PointeeTy && PointeeTy->isSized()
               ? LocationSize::precise(DL.getTypeStoreSize(PointeeTy))
               : LocationSize::beforeOrAfterPointer()));
  };
  for (auto &Arg : F.args())
    addPointeeIfNeed(&Arg);
  auto &GAP{getAnalysis<GlobalsAccessWrapper>()};
  assert(GAP && "Explicitly accessed globals must be collected!");
  if (GAP)
    if (auto GA{GAP->find(&F)}; GA != GAP->end()) {
      for (auto &GV : GA->second)
        if (GV && !isa<UndefValue>(GV))
          addPointeeIfNeed(GV);
    }
  for (auto &I : make_range(inst_begin(F), inst_end(F))) {
    addPointeeIfNeed(&I);
    for (auto *Op : I.operand_values()) {
      addPointeeIfNeed(Op);
      /// Constant expression may access addresses of some locations, so
      /// add this locations into the alias tree.
      if (auto *CE = dyn_cast<ConstantExpr>(Op)) {
        SmallVector<ConstantExpr *, 4> WorkList{ CE };
        do {
          auto *Expr = WorkList.pop_back_val();
          for (auto *ExprOp : Expr->operand_values()) {
            addPointeeIfNeed(ExprOp);
            if (auto ExprCEOp = dyn_cast<ConstantExpr>(ExprOp))
              WorkList.push_back(ExprCEOp);
          }
        } while (!WorkList.empty());
      } else if (isa<CallBase>(I) && !AccessedUnknown.count(&I)) {
        mAliasTree->addUnknown(&I);
      }
    }
  }
  return false;
}
