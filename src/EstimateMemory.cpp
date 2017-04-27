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
Value * stripPointer(Value *Ptr, const DataLayout &DL) {
  assert(Ptr && "Pointer to memory location must not be null!");
  Ptr = GetUnderlyingObject(Ptr, DL);
  if (auto LI = dyn_cast<LoadInst>(Ptr))
    return stripPointer(LI->getPointerOperand(), DL);
  if (Operator::getOpcode(Ptr) == Instruction::IntToPtr) {
    return stripPointer(
      GetUnderlyingObject(cast<Operator>(Ptr)->getOperand(0), DL), DL);
  }
  return Ptr;
}

void stripToBase(MemoryLocation &Loc, const DataLayout &DL) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  // It seams that it is safe to strip 'inttoptr', 'addrspacecast' and that an
  // alias analysis works well in this case. LLVM IR specification requires that
  // if the address space conversion is legal then both result and operand refer
  // to the same memory location.
  if (Operator::getOpcode(Loc.Ptr) == Instruction::BitCast ||
      Operator::getOpcode(Loc.Ptr) == Instruction::AddrSpaceCast ||
      Operator::getOpcode(Loc.Ptr) == Instruction::IntToPtr) {
    Loc.Ptr = cast<const Operator>(Loc.Ptr)->getOperand(0);
    return stripToBase(Loc, DL);
  }
  if (auto GEPI = dyn_cast<const GetElementPtrInst>(Loc.Ptr)) {
    Type *Ty = GEPI->getSourceElementType();
    // TODO (kaniandr@gmail.com): it is possible that sequence of
    // 'getelmentptr' instructions is represented as a single instruction.
    // If the result of it is a member of a structure this case must be
    // evaluated separately. At this moment only individual access to
    // members is supported: for struct STy {int X;}; it is
    // %X = getelementptr inbounds %struct.STy, %struct.STy* %S, i32 0, i32 0
    // Also fix it in isSameBase().
    if (!isa<StructType>(Ty) || GEPI->getNumIndices() != 2) {
      Loc.Ptr = GEPI->getPointerOperand();
      Loc.Size = Ty->isArrayTy() ?
        DL.getTypeStoreSize(Ty) : MemoryLocation::UnknownSize;
      return stripToBase(Loc, DL);
    }
  }
}

bool isSameBase(const llvm::Value *BasePtr1, const llvm::Value *BasePtr2) {
  if (BasePtr1 == BasePtr2)
    return true;
  if (!BasePtr1 || !BasePtr2 ||
      BasePtr1->getValueID() != BasePtr2->getValueID())
    return false;
  if (auto LI = dyn_cast<const LoadInst>(BasePtr1))
    return isSameBase(LI->getPointerOperand(),
      cast<const LoadInst>(BasePtr2)->getPointerOperand());
  if (auto GEPI1 = dyn_cast<const GetElementPtrInst>(BasePtr1)) {
    auto GEPI2 = dyn_cast<const GetElementPtrInst>(BasePtr2);
    if (!isSameBase(GEPI1->getPointerOperand(), GEPI2->getPointerOperand()))
      return false;
    // TODO (kaniandr@gmail.com) : see stripToBase().
    Type *Ty1 = GEPI1->getSourceElementType();
    Type *Ty2 = GEPI2->getSourceElementType();
    if ((!isa<StructType>(Ty1) || GEPI1->getNumIndices() != 2) &&
        (!isa<StructType>(Ty2) || GEPI2->getNumIndices() != 2))
      return true;
    if (Ty1 != Ty2 || GEPI1->getNumIndices() != GEPI2->getNumIndices())
      return false;
    const Use *Idx1 = GEPI1->idx_begin();
    const Use *Idx2 = GEPI2->idx_begin();
    assert(isa<ConstantInt>(Idx1) && cast<ConstantInt>(Idx1)->isZero() &&
      "First index in getelemenptr for structure is not a zero value!");
    assert(isa<ConstantInt>(Idx2) && cast<ConstantInt>(Idx2)->isZero() &&
      "First index in getelemenptr for structure is not a zero value!");
    ++Idx1; ++Idx2;
    return cast<ConstantInt>(Idx1)->getZExtValue() ==
      cast<ConstantInt>(Idx2)->getZExtValue();
  }
  return false;
}

template<class ItrTy>
AliasRelation aliasRelation(AAResults &AA, const EstimateMemory &EM,
    const ItrTy &BeginItr, const ItrTy &EndItr) {
  bool IsAlias = false;
  bool IsLarger = false;
  bool IsSmaller = false;
  for (auto &TmpEM : make_range(BeginItr, EndItr))
    for (auto &TmpPtr : TmpEM)
      for (auto &Ptr : EM) {
        auto AR = AA.alias(
          MemoryLocation(Ptr, EM.getSize(), EM.getAAInfo()),
          MemoryLocation(TmpPtr, TmpEM.getSize(), TmpEM.getAAInfo()));
        switch (AR) {
        case NoAlias: continue;
        case MayAlias: return AliasRelation::MayAlias;
        case PartialAlias: return AliasRelation::PartialAlias;
        }
        IsAlias = true;
        IsSmaller = IsSmaller || EM.getSize() <  TmpEM.getSize();
        IsLarger = IsLarger || EM.getSize() > TmpEM.getSize();
        if (IsLarger && IsSmaller)
          return AliasRelation::MayAlias;
      }
  return !IsAlias ? AliasRelation::NoAlias :
    IsLarger ? AliasRelation::Cover :
    IsSmaller ? AliasRelation::Contained :
    AliasRelation::Coincide;
}
}

const AliasNode * EstimateMemory::getAliasNode(const AliasTree &G) const {
  assert(mNode && "Alias not is not specified yet!");
  if (mNode->isForwarding()) {
    auto *OldNode = mNode;
    mNode = OldNode->getForwardedTarget(G);
    mNode->retain();
    OldNode->release(G);
  }
  return mNode;
}

void AliasTree::add(const MemoryLocation &Loc) {
  EstimateMemory *EM;
  bool IsNew, AddAmbiguous;
  std::tie(EM, IsNew, AddAmbiguous) = insert(Loc);
  assert(EM && "New estimate memory must not be null!");
  if (!IsNew && !AddAmbiguous)
    return;
  using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
  if (AddAmbiguous) {
    /// TODO (kaniandr@gmail.com): optimize duplicate search.
    if (IsNew) {
      auto Node = addEmptyNode(*EM, *getTopLevelNode());
      EM->setAliasNode(*Node);
    }
    while (CT::getPrev(EM))
      EM = CT::getPrev(EM);
    do {
      auto Node = addEmptyNode(*EM, *getTopLevelNode());
      auto Forward = EM->getAliasNode(*this);
      assert(Forward && "Alias node for memory location must not be null!");
      while (Forward != Node) {
        auto Parent = Forward->getParent();
        assert(Parent && "Parent node must not be null!");
        Parent->mergeNodeIn(*Forward), ++NumMergedNode;
        Forward = Parent;
      }
      EM = CT::getNext(EM);
    } while (EM);
  } else {
    auto *CurrNode =
      CT::getNext(EM) ? CT::getNext(EM)->getAliasNode(*this) : getTopLevelNode();
    auto Node = addEmptyNode(*EM, *CurrNode);
    EM->setAliasNode(*Node);
  }
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
      auto AR = aliasRelation(
        *mAA, NewEM, AliasNode::iterator(Aliases.front()), Node->end());
      if (AR == AliasRelation::MayAlias || AR == AliasRelation::PartialAlias)
        return Node;
      if (AR == AliasRelation::Cover) {
        auto *NewNode = newNode(*Current);
        Node->setParent(*NewNode);
        return NewNode;
      }
      Current = Node;
      continue;
    }
    for (auto EM : Aliases) {
      auto Node = EM->getAliasNode(*this);
      assert(Node && "Alias node for memory location must not be null!");
      auto AR = aliasRelation(*mAA, NewEM, Node->begin(), Node->end());
      if (AR != AliasRelation::Cover) {
        // If the new estimate location aliases with locations from different
        // alias nodes at the same level and does not cover memory described by
        // this nodes, this nodes should be merged.
        auto I = Aliases.begin(), EI = Aliases.end();
        auto ForwardNode = (*I)->getAliasNode(*this);
        for (++I; I != EI; ++I, ++NumMergedNode)
          ForwardNode->mergeNodeIn(*(*I)->getAliasNode(*this));
        return ForwardNode;
      }
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

std::tuple<EstimateMemory *, bool, bool>
AliasTree::insert(const MemoryLocation &Loc) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  MemoryLocation Base(Loc);
  stripToBase(Base, *mDL);
  Value *StrippedPtr = stripPointer(const_cast<Value *>(Base.Ptr), *mDL);
  BaseList *BL;
  auto I = mBases.find(StrippedPtr);
  if (I != mBases.end()) {
    BL = &I->second;
    for (auto *Chain : *BL) {
      if (!isSameBase(Chain->front(), Base.Ptr))
        continue;
      bool AddAmbiguous = false;
      switch (isSamePointer(*Chain, Base)) {
      case NoAlias: continue;
      case MayAlias:
        AddAmbiguous = true;
        Chain->getAmbiguousList()->push_back(Base.Ptr);
        break;
      }
      using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
      EstimateMemory *Prev = nullptr, *EM = nullptr;
      do {
        if (Base.Size == Chain->getSize()) {
          Chain->updateAAInfo(Base.AATags);
          return std::make_tuple(Chain, false, AddAmbiguous);
        }
        if (Base.Size < Chain->getSize()) {
          auto EM = new EstimateMemory(*Chain, Base.Size, Base.AATags);
          ++NumEstimateMemory;
          CT::setPrev(EM, Chain);
          return std::make_tuple(EM, true, AddAmbiguous);
        }
      } while (Prev = Chain, Chain = CT::getNext(Chain));
      Chain = new EstimateMemory(*Prev, Base.Size, Base.AATags);
      ++NumEstimateMemory;
      CT::setNext(Chain, Prev);
      return std::make_tuple(Chain, true, AddAmbiguous);
    }
  } else {
    BL = &mBases.insert(std::make_pair(StrippedPtr, BaseList())).first->second;
  }
  auto Chain = new EstimateMemory(
    std::move(Base), AmbiguousRef::make(mAmbiguousPool));
  ++NumEstimateMemory;
  BL->push_back(Chain);
  return std::make_tuple(Chain, true, false);
}

char EstimateMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(EstimateMemoryPass, "estimate-mem",
  "Memory Estimator", true, true)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(EstimateMemoryPass, "estimate-mem",
  "Memory Estimator", true, true)

void EstimateMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<AAResultsWrapperPass>();
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
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    switch (I->getOpcode()) {
      case Instruction::Load: case Instruction::Store: case Instruction::VAArg:
      case Instruction::AtomicRMW: case Instruction::AtomicCmpXchg:
        mAliasTree->add(MemoryLocation::get(&*I)); break;
    }
  }
  return false;
}
