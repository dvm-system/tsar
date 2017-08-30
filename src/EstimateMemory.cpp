//===--- EstimateMemory.cpp ----- Memory Hierarchy --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file proposes functionality to construct a program alias tree.
//
//===----------------------------------------------------------------------===//

#include "EstimateMemory.h"
#include "tsar_dbg_output.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "estimate-mem"

STATISTIC(NumAliasNode, "Number of alias nodes created");
STATISTIC(NumMergedNode, "Number of alias nodes merged in");
STATISTIC(NumEstimateMemory, "Number of estimate memory created");

namespace tsar {
Value * stripPointer(const DataLayout &DL, Value *Ptr) {
  assert(Ptr && "Pointer to memory location must not be null!");
  Ptr = GetUnderlyingObject(Ptr, DL);
  if (auto LI = dyn_cast<LoadInst>(Ptr))
    return stripPointer(DL, LI->getPointerOperand());
  if (Operator::getOpcode(Ptr) == Instruction::IntToPtr) {
    return stripPointer(DL,
      GetUnderlyingObject(cast<Operator>(Ptr)->getOperand(0), DL));
  }
  return Ptr;
}

void stripToBase(const DataLayout &DL, MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  // GepUnderlyingObject() will strip `getelementptr` instruction, so ignore such
  // behavior.
  if (auto  GEP = dyn_cast<const GEPOperator>(Loc.Ptr))
    return;
  // It seams that it is safe to strip 'inttoptr', 'addrspacecast' and that an
  // alias analysis works well in this case. LLVM IR specification requires that
  // if the address space conversion is legal then both result and operand refer
  // to the same memory location.
  if (Operator::getOpcode(Loc.Ptr) == Instruction::IntToPtr) {
    Loc.Ptr = cast<const Operator>(Loc.Ptr)->getOperand(0);
    return stripToBase(DL, Loc);
  }
  auto BasePtr = GetUnderlyingObject(const_cast<Value *>(Loc.Ptr), DL, 1);
  if (BasePtr == Loc.Ptr)
    return;
  Loc.Ptr = BasePtr;
  stripToBase(DL, Loc);
}

bool stripMemoryLevel(const DataLayout &DL, MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  auto Ty = Loc.Ptr->getType();
  if (auto PtrTy = dyn_cast<PointerType>(Ty)) {
    auto Size = DL.getTypeStoreSize(PtrTy->getElementType());
    if (Size > Loc.Size) {
      Loc.Size = Size;
      return true;
    } else if (Size < Loc.Size) {
      return false;
    }
  }
  if (auto GEP = dyn_cast<const GEPOperator>(Loc.Ptr)) {
    Loc.Ptr = GEP->getPointerOperand();
    Loc.AATags = llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey();
    Loc.Size = MemoryLocation::UnknownSize;
    Type *SrcTy = GEP->getSourceElementType();
    // We can to precise location size, if this instruction is used to
    // access element of array or structure without shifting of a pointer.
    if ((SrcTy->isArrayTy() || SrcTy->isStructTy()) &&
        GEP->getNumOperands() > 2)
      if (auto OpC = dyn_cast<ConstantInt>(GEP->getOperand(1)))
        if (OpC->isZero())
          Loc.Size = DL.getTypeStoreSize(SrcTy);
    return true;
  }
  return false;
}

bool isSameBase(const DataLayout &DL,
    const llvm::Value *BasePtr1, const llvm::Value *BasePtr2) {
  if (BasePtr1 == BasePtr2)
    return true;
  if (!BasePtr1 || !BasePtr2 ||
      BasePtr1->getValueID() != BasePtr2->getValueID())
    return false;
  if (Operator::getOpcode(BasePtr1) == Instruction::IntToPtr ||
      Operator::getOpcode(BasePtr1) == Instruction::BitCast ||
      Operator::getOpcode(BasePtr1) == Instruction::AddrSpaceCast)
    return isSameBase(DL,
      cast<const Operator>(BasePtr1)->getOperand(0),
      cast<const Operator>(BasePtr2)->getOperand(0));
  if (auto LI = dyn_cast<const LoadInst>(BasePtr1))
    return isSameBase(DL, LI->getPointerOperand(),
      cast<const LoadInst>(BasePtr2)->getPointerOperand());
  if (auto GEP1 = dyn_cast<const GEPOperator>(BasePtr1)) {
    auto GEP2 = dyn_cast<const GEPOperator>(BasePtr2);
    if (!isSameBase(DL, GEP1->getPointerOperand(), GEP2->getPointerOperand()))
      return false;
    if (GEP1->getSourceElementType() != GEP2->getSourceElementType())
      return false;
    if (GEP1->getNumIndices() != GEP2->getNumIndices())
      return false;
    auto I1 = gep_type_begin(GEP1), E1 = gep_type_end(GEP1);
    auto I2 = gep_type_begin(GEP2);
    auto BitWidth1 = DL.getPointerSizeInBits(GEP1->getPointerAddressSpace());
    auto BitWidth2 = DL.getPointerSizeInBits(GEP2->getPointerAddressSpace());
    for (; I1 != E1; ++I1, I2++) {
      if (I1.getOperand() == I2.getOperand())
        continue;
      auto OpC1 = dyn_cast<ConstantInt>(I1.getOperand());
      auto OpC2 = dyn_cast<ConstantInt>(I2.getOperand());
      if (!OpC1 || !OpC2)
        return false;
      APInt Offset1, Offset2;
      if (auto STy1 = I1.getStructTypeOrNull()) {
        assert(I2.getStructTypeOrNull() && "It must be a structure!");
        auto Idx1 = OpC1->getZExtValue();
        auto SL1 = DL.getStructLayout(STy1);
        Offset1 = APInt(BitWidth1, SL1->getElementOffset(Idx1));
        auto STy2 = I2.getStructType();
        auto Idx2 = OpC2->getZExtValue();
        auto SL2 = DL.getStructLayout(STy2);
        Offset2 = APInt(BitWidth2, SL2->getElementOffset(Idx2));
      } else {
        assert(!I2.getStructTypeOrNull() && "It must not be a structure!");
        APInt Idx1 = OpC1->getValue().sextOrTrunc(BitWidth1);
        Offset1 = Idx1 *
          APInt(BitWidth1, DL.getTypeAllocSize(I1.getIndexedType()));
        APInt Idx2 = OpC2->getValue().sextOrTrunc(BitWidth2);
        Offset2 = Idx2 *
          APInt(BitWidth2, DL.getTypeAllocSize(I2.getIndexedType()));
      }
      if (Offset1 != Offset2)
        return false;
    }
    return true;
  }
  return false;
}

AliasDescriptor aliasRelation(AAResults &AA, const DataLayout &DL,
    const MemoryLocation &LHS, const MemoryLocation &RHS) {
  AliasDescriptor Dptr;
  auto AR = AA.alias(LHS, RHS);
  switch (AR) {
  default: llvm_unreachable("Unknown result of alias analysis!");
  case NoAlias: Dptr.set<trait::NoAlias>(); break;
  case MayAlias: Dptr.set<trait::MayAlias>(); break;
  case PartialAlias:
    {
      Dptr.set<trait::PartialAlias>();
      // Now we try to prove that one location covers other location.
      if (LHS.Size == RHS.Size ||
          LHS.Size == MemoryLocation::UnknownSize &&
          RHS.Size == MemoryLocation::UnknownSize)
        break;
      int64_t OffsetLHS, OffsetRHS;
      auto BaseLHS = GetPointerBaseWithConstantOffset(LHS.Ptr, OffsetLHS, DL);
      auto BaseRHS = GetPointerBaseWithConstantOffset(RHS.Ptr, OffsetRHS, DL);
      if (OffsetLHS == 0 && OffsetRHS == 0)
        break;
      auto BaseAlias = AA.alias(
        BaseLHS, MemoryLocation::UnknownSize,
        BaseRHS, MemoryLocation::UnknownSize);
      // It is possible to precisely compare two partially overlapped
      // locations in case of the same base pointer only.
      if (BaseAlias != MustAlias)
        break;
      if (OffsetLHS < OffsetRHS &&
          OffsetLHS + LHS.Size >= OffsetRHS + RHS.Size)
        Dptr.set<trait::CoverAlias>();
      else if (OffsetLHS > OffsetRHS &&
          OffsetLHS + LHS.Size <= OffsetRHS + RHS.Size)
        Dptr.set<trait::ContainedAlias>();
    }
    break;
  case MustAlias:
    Dptr.set<trait::MustAlias>();
    if (LHS.Size == RHS.Size)
      Dptr.set<trait::CoincideAlias>();
    else if (LHS.Size > RHS.Size)
      Dptr.set<trait::CoverAlias>();
    if (LHS.Size < RHS.Size)
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

const AliasNode * EstimateMemory::getAliasNode(const AliasTree &G) const {
  assert(mNode && "Alias node is not specified yet!");
  if (mNode->isForwarding()) {
    auto *OldNode = mNode;
    mNode = OldNode->getForwardedTarget(G);
    mNode->retain();
    OldNode->release(G);
  }
  return mNode;
}

const AliasNode * AliasNode::getParent(const AliasTree &G) const {
  assert(mParent && "Parent is not specified yet!");
  if (mParent->isForwarding()) {
    auto *OldNode = mParent;
    mParent = OldNode->getForwardedTarget(G);
    mParent->retain();
    OldNode->release(G);
  }
  return mParent;
}

void AliasTree::add(const MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  DEBUG(dbgs() << "[ALIAS TREE]: add memory location\n");
  using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
  MemoryLocation Base(Loc);
  EstimateMemory *PrevChainEnd = nullptr;
  do {
    DEBUG(
      dbgs() << "[ALIAS TREE]: evaluate memory level ";
      printLocationSource(dbgs(), Base);
      dbgs() << "\n";
    );
    stripToBase(*mDL, Base);
    EstimateMemory *EM;
    bool IsNew, AddAmbiguous;
    std::tie(EM, IsNew, AddAmbiguous) = insert(Base);
    DEBUG(
      dbgs() << "[ALIAS TREE]: update estimate memory location tree:";
      dbgs() << " IsNew=" << (IsNew ? "true" : "false");
      dbgs() << " AddAmbiguous=" << (AddAmbiguous ? "true" : "false") << "\n";
    );
    assert(EM && "New estimate memory must not be null!");
    if (PrevChainEnd && PrevChainEnd != EM) {
      assert((!PrevChainEnd->getParent() || PrevChainEnd->getParent() == EM) &&
        "Inconsistent parent of a node in estimate memory tree!");
      DEBUG(
        dbgs() << "[ALIAS TREE]: merge location ";
        printLocationSource(dbgs(), MemoryLocation(EM->front(), EM->getSize()));
        dbgs() << " to the end of ";
        printLocationSource(dbgs(),
          MemoryLocation(PrevChainEnd->front(), PrevChainEnd->getSize()));
      );
      CT::mergeNext(EM, PrevChainEnd);
    }
    if (!IsNew && !AddAmbiguous)
      return;
    PrevChainEnd = EM;
    while (CT::getNext(PrevChainEnd))
      PrevChainEnd = CT::getNext(PrevChainEnd);
    // Already evaluated locations should be omitted to avoid loops in chain.
    Base.Size = PrevChainEnd->getSize();
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
        auto Forward = EM->getAliasNode(*this);
        assert(Forward && "Alias node for memory location must not be null!");
        while (Forward != Node) {
          auto Parent = Forward->getParent(*this);
          assert(Parent && "Parent node must not be null!");
          Parent->mergeNodeIn(*Forward, *this), ++NumMergedNode;
          Forward = Parent;
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
}

void AliasTree::removeNode(AliasNode *N) {
  if (auto *Fwd = N->mForward) {
    Fwd->release(*this);
    N->mForward = nullptr;
  }
  mNodes.erase(N);
}

AliasNode * AliasTree::addEmptyNode(
    const EstimateMemory &NewEM,  AliasNode &Start) {
  auto Current = &Start;
  auto newNode = [this](AliasNode &Parrent) {
    auto *NewNode = new AliasNode;
    ++NumAliasNode;
    mNodes.push_back(NewNode);
    NewNode->setParent(Parrent);
    return NewNode;
  };
  SmallPtrSet<const AliasNode *, 8> ChildrenNodes;
  for (auto I = NewEM.child_begin(), E = NewEM.child_end(); I != E; ++I)
    ChildrenNodes.insert(I->getAliasNode(*this));
  SmallVector<EstimateMemory *, 4> Aliases;
  for (;;) {
    Aliases.clear();
    for (auto &Child : make_range(Current->child_begin(), Current->child_end()))
      for (auto &EM : Child)
        if (slowMayAlias(EM, NewEM)) {
          Aliases.push_back(&EM);
          break;
        }
    if (Aliases.empty())
      return newNode(*Current);
    if (Aliases.size() == 1) {
      auto Node = Aliases.front()->getAliasNode(*this);
      assert(Node && "Alias node for memory location must not be null!");
      auto AD = aliasRelation(
        *mAA, *mDL, NewEM, AliasNode::iterator(Aliases.front()), Node->end());
      if (AD.is<trait::CoverAlias>()) {
        auto *NewNode = newNode(*Current);
        Node->setParent(*NewNode);
        return NewNode;
      }
      // The second condition is necessary due to alias node which contains
      // full memory should not be descendant of a node which contains part
      // of this memory.
      if (!AD.is<trait::ContainedAlias>() || ChildrenNodes.count(Node))
        return Node;
      Current = Node;
      continue;
    }
    for (auto EM : Aliases) {
      auto Node = EM->getAliasNode(*this);
      assert(Node && "Alias node for memory location must not be null!");
      auto AD = aliasRelation(*mAA, *mDL, NewEM, Node->begin(), Node->end());
      if (AD.is<trait::CoverAlias>() ||
          (AD.is<trait::CoincideAlias>() && !AD.is<trait::ContainedAlias>()))
        continue;
      // If the new estimate location aliases with locations from different
      // alias nodes at the same level and does not cover (or coincide with)
      // memory described by this nodes, this nodes should be merged.
      auto I = Aliases.begin(), EI = Aliases.end();
      auto ForwardNode = (*I)->getAliasNode(*this);
      for (++I; I != EI; ++I, ++NumMergedNode)
        ForwardNode->mergeNodeIn(*(*I)->getAliasNode(*this), *this);
      return ForwardNode;
    }
    auto *NewNode = newNode(*Current);
    for (auto EM : Aliases)
      EM->getAliasNode(*this)->setParent(*NewNode);
    return NewNode;
  }
}

AliasResult AliasTree::isSamePointer(
    const EstimateMemory &EM, const MemoryLocation &Loc) const {
  bool IsAmbiguous = false;
  for (auto *Ptr : EM) {
    switch (mAA->alias(
        MemoryLocation(Ptr, 1, EM.getAAInfo()),
        MemoryLocation(Loc.Ptr, 1, Loc.AATags))) {
      case MustAlias: return MustAlias;
      case MayAlias: IsAmbiguous = true; break;
    }
  }
  return IsAmbiguous ? MayAlias : NoAlias;
}

bool AliasTree::slowMayAlias(
    const EstimateMemory &LHS, const EstimateMemory &RHS) const {
  for (auto &LHSPtr : LHS)
    for (auto &RHSPtr : RHS) {
      auto AR = mAA->alias(
        MemoryLocation(LHSPtr, LHS.getSize(), LHS.getAAInfo()),
        MemoryLocation(RHSPtr, RHS.getSize(), RHS.getAAInfo()));
      if (AR == NoAlias)
        continue;
      return true;
    }
  return false;
}

const EstimateMemory * AliasTree::find(const llvm::MemoryLocation & Loc) const {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  MemoryLocation Base(Loc);
  stripToBase(*mDL, Base);
  Value *StrippedPtr = stripPointer(*mDL, const_cast<Value *>(Base.Ptr));
  auto I = mBases.find(StrippedPtr);
  if (I == mBases.end())
    return nullptr;
  for (auto ChainItr = I->second.begin(), ChainEItr = I->second.end();
       ChainItr != ChainEItr; ++ChainItr) {
    auto Chain = *ChainItr;
    if (!isSameBase(*mDL, Chain->front(), Base.Ptr))
      continue;
    switch (isSamePointer(*Chain, Base)) {
    case NoAlias: continue;
    case MayAlias: case MustAlias:  break;
    default: llvm_unreachable("Unknown result of alias query!"); break;
    }
    using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
    EstimateMemory *Prev = nullptr;
    do {
      if (Base.Size > Chain->getSize())
        continue;
      auto AATags = Chain->getAAInfo();
      return (AATags == Base.AATags ||
        AATags == DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey()) ?
        Chain : nullptr;
    } while (Prev = Chain, Chain = CT::getNext(Chain));
    return nullptr;
  }
  return nullptr;
}

std::tuple<EstimateMemory *, bool, bool>
AliasTree::insert(const MemoryLocation &Base) {
  assert(Base.Ptr && "Pointer to memory location must not be null!");
  Value *StrippedPtr = stripPointer(*mDL, const_cast<Value *>(Base.Ptr));
  BaseList *BL;
  auto I = mBases.find(StrippedPtr);
  if (I != mBases.end()) {
    BL = &I->second;
    for (auto ChainItr = BL->begin(), ChainEItr = BL->end();
         ChainItr != ChainEItr; ++ChainItr) {
      auto Chain = *ChainItr;
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
      case NoAlias: continue;
      case MustAlias: break;
      case MayAlias:
        AddAmbiguous = true;
        Chain->getAmbiguousList()->push_back(Base.Ptr);
        break;
      }
      auto StripedBase = Base;
      MemoryLocation StripedChain(
        Chain->front(), Chain->getSize(), Chain->getAAInfo());
      bool IsStripedBase = stripMemoryLevel(*mDL, StripedBase);
      bool IsStripedChain = stripMemoryLevel(*mDL, StripedChain);
      // This condition checks that it is safe to insert a specified location
      // Base into the estimate memory tree which contains location Chain.
      // The following cases lead to building new estimate memory tree:
      // 1. Base and Chain are x.y.z but striped locations are x.y and x.
      // It is possible due to implementation of strimMemoryLevel() function.
      // 2. Size of Base or Chain are greater than getTypeStoreSize(). In this
      // case it is not possible to strip such location. Note, that if both
      // Base and Chain have such problem they can be inserted in a single tree.
      if ((IsStripedBase || IsStripedChain) &&
          !isSameBase(*mDL, StripedBase.Ptr, StripedChain.Ptr))
        continue;
      using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
      EstimateMemory *Prev = nullptr;
      do {
        if (Base.Size == Chain->getSize()) {
          Chain->updateAAInfo(Base.AATags);
          return std::make_tuple(Chain, false, AddAmbiguous);
        }
        if (Base.Size < Chain->getSize()) {
          auto EM = new EstimateMemory(*Chain, Base.Size, Base.AATags);
          ++NumEstimateMemory;
          CT::splicePrev(EM, Chain);
          *ChainItr = EM; // update start point of this chain in a base list
          return std::make_tuple(EM, true, AddAmbiguous);
        }
      } while (Prev = Chain, Chain = CT::getNext(Chain));
      auto EM = new EstimateMemory(*Prev, Base.Size, Base.AATags);
      ++NumEstimateMemory;
      CT::spliceNext(EM, Prev);
      return std::make_tuple(EM, true, AddAmbiguous);
    }
  } else {
    BL = &mBases.insert(std::make_pair(StrippedPtr, BaseList())).first->second;
  }
  auto Chain = new EstimateMemory(Base, AmbiguousRef::make(mAmbiguousPool));
  ++NumEstimateMemory;
  BL->push_back(Chain);
  return std::make_tuple(Chain, true, false);
}

char EstimateMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(EstimateMemoryPass, "estimate-mem",
  "Memory Estimator", true, true)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(EstimateMemoryPass, "estimate-mem",
  "Memory Estimator", true, true)

void EstimateMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createEstimateMemoryPass() {
  return new EstimateMemoryPass();
}

bool EstimateMemoryPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto M = F.getParent();
  auto &DL = M->getDataLayout();
  mAliasTree = new AliasTree(AA, DL);
  // TODO (kaniandr@gmail.com): implements evaluation of transfer intrinsics.
  // This should be also implemented in DefinedMemoryPass.
  // TODO (kaniandr@gmail.com): implements support for unknown memory access,
  // for example, in call and invoke instructions.
  uint64_t S;
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    switch (I->getOpcode()) {
      case Instruction::Load: case Instruction::Store: case Instruction::VAArg:
      case Instruction::AtomicRMW: case Instruction::AtomicCmpXchg:
        mAliasTree->add(MemoryLocation::get(&*I)); break;
    }
  }
  return false;
}
