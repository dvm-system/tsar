//===- DIEstimateMemory.cpp - Memory Hierarchy (Debug) ----------*- C++ -*-===//
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

#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "CorruptedMemory.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemorySetInfo.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Unparse/Utils.h"
#include <bcl/IteratorDataAdaptor.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <algorithm>
#include <memory>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "di-estimate-mem"

STATISTIC(NumAliasNode, "Number of alias nodes created");
STATISTIC(NumEstimateNode, "Number of estimate nodes created");
STATISTIC(NumUnknownNode, "Number of unknown nodes created");
STATISTIC(NumEstimateMemory, "Number of estimate memory created");
STATISTIC(NumUnknownMemory, "Number of unknown memory created");
STATISTIC(NumCorruptedMemory, "Number of corrupted memory created");

namespace tsar {
bool findBoundAliasNodes(const DIEstimateMemory &DIEM, AliasTree &AT,
    SmallPtrSetImpl<AliasNode *> &Nodes) {
  bool IsOk{false};
  for (auto &VH : DIEM) {
    if (!VH || isa<llvm::UndefValue>(VH))
      continue;
    auto EM = AT.find(MemoryLocation(VH, DIEM.getSize()));
    // Memory becomes unused after transformation and does not presented in
    // the alias tree.
    if (!EM)
      continue;
    IsOk = true;
    Nodes.insert(EM->getAliasNode(AT));
  }
  return IsOk;
}

bool findLowerBoundAliasNodes(const DIEstimateMemory &DIEM, AliasTree &AT,
                              llvm::SmallPtrSetImpl<AliasNode *> &Nodes) {
  bool IsOk{false};
  auto Size{DIEM.getSize()};
  for (auto &VH : DIEM) {
    if (!VH || isa<llvm::UndefValue>(VH))
      continue;
    auto EM = AT.find(MemoryLocation(VH, 0));
    // Memory becomes unused after transformation and does not presented in
    // the alias tree.
    using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
    for (;
         EM && MemorySetInfo<MemoryLocation>::sizecmp(Size, EM->getSize()) > 0;
         EM = CT::getNext(EM)) {
      IsOk = true;
      Nodes.insert(EM->getAliasNode(AT));
    }
  }
  return IsOk;
}

bool findBoundAliasNodes(const DIUnknownMemory &DIUM, AliasTree &AT,
    SmallPtrSetImpl<AliasNode *> &Nodes) {
  bool IsOk{false};
  for (auto &VH : DIUM) {
    if (!VH || isa<UndefValue>(*VH))
      continue;
    AliasNode *N = nullptr;
    if (auto Inst = dyn_cast<Instruction>(VH))
      if (auto *N = AT.findUnknown(cast<Instruction>(*VH))) {
        Nodes.insert(N);
        return true;
      }
    auto EM = AT.find(MemoryLocation(VH, 0));
    // Memory becomes unused after transformation and does not presented in
    // the alias tree.
    if (!EM)
      continue;
    IsOk = true;
    using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
    do {
      Nodes.insert(EM->getAliasNode(AT));
    } while (EM = CT::getNext(EM));
  }
  return IsOk;
}

bool findBoundAliasNodes(const DIMemory &DIM, AliasTree &AT,
    SmallPtrSetImpl<AliasNode *> &Nodes) {
  if (auto *EM = dyn_cast<DIEstimateMemory>(&DIM))
    return findBoundAliasNodes(*EM, AT, Nodes);
  else
    return findBoundAliasNodes(cast<DIUnknownMemory>(DIM), AT, Nodes);
}
}

DIMemory::~DIMemory() {
  if (hasMemoryHandle())
    DIMemoryHandleBase::memoryIsDeleted(this);
}

void DIMemory::replaceAllUsesWith(DIMemory *M) {
  assert(M && "New memory location must not be null!");
  assert(this != M && "Old and new memory must be differ!");
  if (hasMemoryHandle())
    DIMemoryHandleBase::memoryIsRAUWd(this, M);
}

std::unique_ptr<DIUnknownMemory> DIUnknownMemory::get(llvm::LLVMContext &Ctx,
    DIMemoryEnvironment &Env, DIUnknownMemory &UM) {
  ++NumUnknownMemory;
  return std::unique_ptr<DIUnknownMemory>(
    new DIUnknownMemory(Env, UM.getAsMDNode()));
}

static void serialize(DILocation &Loc, LLVMContext &Ctx,
  SmallVectorImpl<Metadata *> &MDs) {
  auto LineMD = ConstantAsMetadata::get(
    ConstantInt::get(Type::getInt32Ty(Ctx), Loc.getLine()));
  MDs.push_back(LineMD);
  auto ColumnMD = ConstantAsMetadata::get(
    ConstantInt::get(Type::getInt32Ty(Ctx), Loc.getColumn()));
  MDs.push_back(ColumnMD);
  if (auto *Scope = Loc.getRawScope())
    MDs.push_back(Scope);
  if (auto *InlineAt = Loc.getInlinedAt())
    serialize(*InlineAt, Ctx, MDs);
  else
    MDs.push_back(nullptr);
}

std::unique_ptr<DIUnknownMemory> DIUnknownMemory::get(llvm::LLVMContext &Ctx,
  DIMemoryEnvironment &Env, MDNode *MD, Flags F,
  ArrayRef<DILocation *> DbgLocs) {
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  SmallVector<Metadata *, 2> BasicMDs{ MD, FlagMD };
  auto BasicMD = llvm::MDNode::get(Ctx, BasicMDs);
  assert(BasicMD && "Can not create metadata node!");
  if (!MD)
    BasicMD->replaceOperandWith(0, BasicMD);
  SmallVector<Metadata *, 2> MDs{ BasicMD };
  for (auto DbgLoc : DbgLocs)
    if (DbgLoc)
      serialize(*DbgLoc, Ctx, MDs);
  auto NewMD = llvm::MDNode::get(Ctx, MDs);
  assert(NewMD && "Can not create metadata node!");
  ++NumUnknownMemory;
  return std::unique_ptr<DIUnknownMemory>(new DIUnknownMemory(Env, NewMD));
}

llvm::MDNode * DIUnknownMemory::getRawIfExists(llvm::LLVMContext &Ctx,
    llvm::MDNode *MD, Flags F, llvm::ArrayRef<llvm::DILocation *> DbgLocs ) {
  if (!MD)
    return nullptr;
  auto *FlagMD = llvm::ConstantAsMetadata::getIfExists(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  if (!FlagMD)
    return nullptr;
  SmallVector<Metadata *, 2> BasicMDs{ MD, FlagMD };
  auto BasicMD = llvm::MDNode::getIfExists(Ctx, BasicMDs);
  if (!BasicMD)
    return nullptr;
  SmallVector<Metadata *, 2> MDs{ BasicMD };
  for (auto DbgLoc : DbgLocs)
    if (DbgLoc)
      serialize(*DbgLoc, Ctx, MDs);
  return llvm::MDNode::getIfExists(Ctx, MDs);
}

std::unique_ptr<DIUnknownMemory> DIUnknownMemory::getIfExists(
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env, llvm::MDNode *MD,
    Flags F, llvm::ArrayRef<llvm::DILocation *> DbgLocs) {
  auto *Node = getRawIfExists(Ctx, MD, F, DbgLocs);
  if (!Node)
    return nullptr;
  ++NumUnknownMemory;
  return std::unique_ptr<DIUnknownMemory>(new DIUnknownMemory(Env, Node));
}

static inline ConstantInt *getConstantInt(const MDOperand &Op) {
  if (auto CMD = dyn_cast<ConstantAsMetadata>(Op))
    if (auto CInt = dyn_cast<ConstantInt>(CMD->getValue()))
      return CInt;
  return nullptr;
};

static std::pair<DILocation *, unsigned> getLocation(const MDNode *MD,
  unsigned I, unsigned EI) {
  ConstantInt *CInt = nullptr;
  if (!(CInt = getConstantInt(MD->getOperand(I))))
    return std::make_pair(nullptr, I);
  unsigned Line = CInt->getZExtValue();
  if (++I == EI)
    return std::make_pair(nullptr, I);
  if (!(CInt = getConstantInt(MD->getOperand(I))))
    return std::make_pair(nullptr, I);
  unsigned Column = CInt->getZExtValue();
  if (++I == EI)
    return std::make_pair(nullptr, I);
  auto *Scope = dyn_cast<DIScope>(MD->getOperand(I));
  if (!Scope)
    return std::make_pair(nullptr, I);
  if (++I == EI)
    return std::make_pair(nullptr, I);
  auto InlineAt =
    (MD->getOperand(I)) ? getLocation(MD, I, EI) : std::make_pair(nullptr, I);
  return std::make_pair(
    DILocation::get(MD->getContext(), Line, Column, Scope, InlineAt.first),
    InlineAt.second);
}

void DIMemory::getDebugLoc(SmallVectorImpl<DebugLoc> &DbgLocs) const {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    auto Res = getLocation(MD, I, EI);
    I = Res.second;
    if (Res.first)
      DbgLocs.emplace_back(Res.first);
  }
}

std::unique_ptr<DIEstimateMemory> DIEstimateMemory::get(
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F,
    ArrayRef<DILocation *> DbgLocs) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto DILoc{DIMemoryLocation::get(Var, Expr, nullptr, F & Template,
                                   F & AfterPointer)};
  if (DILoc.AfterPointer)
    F |= AfterPointer;
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  SmallVector<Metadata *, 3> BasicMDs{ Var, Expr, FlagMD};
  auto BasicMD = llvm::MDNode::get(Ctx, BasicMDs);
  assert(BasicMD && "Can not create metadata node!");
  SmallVector<Metadata *, 2> MDs{ BasicMD };
  for (auto DbgLoc : DbgLocs)
    if (DbgLoc)
      serialize(*DbgLoc, Ctx, MDs);
  auto MD = llvm::MDNode::get(Ctx, MDs);
  assert(MD && "Can not create metadata node!");
  ++NumEstimateMemory;
  return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(Env, MD));
}

std::unique_ptr<DIEstimateMemory>
DIEstimateMemory::getIfExists(
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F,
    ArrayRef<DILocation *> DbgLocs) {
  if (auto MD = getRawIfExists(Ctx, Var, Expr, F, DbgLocs)) {
    ++NumEstimateMemory;
    return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(Env, MD));
  }
  return nullptr;
}

llvm::MDNode * DIEstimateMemory::getRawIfExists(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F,
    ArrayRef<DILocation *> DbgLocs) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto DILoc{DIMemoryLocation::get(Var, Expr, nullptr, F & Template,
                                   F & AfterPointer)};
  if (DILoc.AfterPointer)
    F |= AfterPointer;
  auto *FlagMD = llvm::ConstantAsMetadata::getIfExists(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  if (!FlagMD)
    return nullptr;
  SmallVector<Metadata *, 3> BasicMDs{ Var, Expr, FlagMD};
  auto *BasicMD = llvm::MDNode::getIfExists(Ctx, BasicMDs);
  if (!BasicMD)
    return nullptr;
  SmallVector<Metadata *, 2> MDs{ BasicMD };
  for (auto DbgLoc : DbgLocs)
    if (DbgLoc)
      serialize(*DbgLoc, Ctx, MDs);
  return llvm::MDNode::getIfExists(Ctx, MDs);
}

std::unique_ptr<DIEstimateMemory> DIEstimateMemory::get(llvm::LLVMContext &Ctx,
    DIMemoryEnvironment &Env, DIEstimateMemory &EM) {
  SmallVector<DebugLoc, 1> DbgLocs;
  EM.getDebugLoc(DbgLocs);
  SmallVector<DILocation *, 1> DILocs(DbgLocs.size());
  std::size_t ToIdx = 0;
  for (std::size_t I = 0, EI = DbgLocs.size(); I < EI; ++I)
    if (DbgLocs[I])
      DILocs[ToIdx++] = DbgLocs[I].get();
  DILocs.resize(ToIdx);
  return get(
    Ctx, Env, EM.getVariable(), EM.getExpression(), EM.getFlags(), DILocs);
}

llvm::DIVariable * DIEstimateMemory::getVariable() {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Var = llvm::dyn_cast<llvm::DIVariable>(MD->getOperand(I)))
      return Var;
  llvm_unreachable("Variable must be specified!");
  return nullptr;
}

const llvm::DIVariable * DIEstimateMemory::getVariable() const {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Var = llvm::dyn_cast<llvm::DIVariable>(MD->getOperand(I)))
      return Var;
  llvm_unreachable("Variable must be specified!");
  return nullptr;
}

llvm::DIExpression * DIEstimateMemory::getExpression() {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Expr = llvm::dyn_cast<llvm::DIExpression>(MD->getOperand(I)))
      return Expr;
  llvm_unreachable("Expression must be specified!");
  return nullptr;
}

const llvm::DIExpression * DIEstimateMemory::getExpression() const {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Expr = llvm::dyn_cast<llvm::DIExpression>(MD->getOperand(I)))
      return Expr;
  llvm_unreachable("Expression must be specified!");
  return nullptr;
}

const MDNode * DIMemory::getBaseAsMDNode() const {
  auto MD = getAsMDNode();
  assert(MD->getNumOperands() > 0 && "At least one operand must exist!");
  return cast<MDNode>(MD->getOperand(0));
}

unsigned DIMemory::getFlagsOp() const {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    auto &Op = MD->getOperand(I);
    auto CMD = dyn_cast<ConstantAsMetadata>(Op);
    if (!CMD)
      continue;
    if (auto CInt = dyn_cast<ConstantInt>(CMD->getValue()))
      return I;
  }
  llvm_unreachable("Explicit flag must be specified!");
}

uint64_t DIMemory::getFlags() const {
  auto MD = getBaseAsMDNode();
  auto CMD = cast<ConstantAsMetadata>(MD->getOperand(getFlagsOp()));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  return CInt->getZExtValue();
}

void DIMemory::setFlags(uint64_t F) {
  auto MD = getBaseAsMDNode();
  auto OpIdx = getFlagsOp();
  auto CMD = cast<ConstantAsMetadata>(MD->getOperand(OpIdx));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  auto &Ctx = MD->getContext();
  auto *FlagMD = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
   Type::getInt64Ty(Ctx), CInt->getZExtValue() | F));
    MD->replaceOperandWith(OpIdx, FlagMD);
}

llvm::MDNode * DIUnknownMemory::getMetadata() {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    if (auto *Op = llvm::dyn_cast<llvm::MDNode>(MD->getOperand(I)))
      return Op;
  }
  llvm_unreachable("MDNode must be specified!");
  return nullptr;
}

const llvm::MDNode * DIUnknownMemory::getMetadata() const {
  auto MD = getBaseAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    if (auto *Op = llvm::dyn_cast<llvm::MDNode>(MD->getOperand(I)))
      return Op;
  }
  llvm_unreachable("MDNode must be specified!");
  return nullptr;
}

void DIMemoryHandleBase::addToUseList() {
  assert(mMemory && "Null pointer does not have handles!");
  auto &Env = mMemory->getEnv();
  if (mMemory->hasMemoryHandle()) {
    DIMemoryHandleBase *&Entry = Env[mMemory];
    assert(mMemory && "Memory does not have any handles?");
    addToExistingUseList(&Entry);
    return;
  }
  // Ok, it doesn't have any handles yet, so we must insert it into the
  // DenseMap. However, doing this insertion could cause the DenseMap to
  // reallocate itself, which would invalidate all of the PrevP pointers that
  // point into the old table. Handle this by checking for reallocation and
  // updating the stale pointers only if needed.
  auto &Handles = Env.getMemoryHandles();
  const void *OldBucketPtr = Handles.getPointerIntoBucketsArray();
  DIMemoryHandleBase *&Entry = Handles[mMemory];
  assert(!Entry && "Memory really did already have handles?");
  addToExistingUseList(&Entry);
  mMemory->setHasMemoryHandle(true);
  if (Handles.isPointerIntoBucketsArray(OldBucketPtr) || Handles.size() == 1)
    return;
  // Okay, reallocation did happen. Fix the Prev Pointers.
  for (auto I = Handles.begin(), E = Handles.end(); I != E; ++I) {
    assert(I->second && I->first == I->second->mMemory &&
      "List invariant broken!");
    I->second->setPrevPtr(&I->second);
  }
}

void DIMemoryHandleBase::removeFromUseList() {
  assert(mMemory && mMemory->hasMemoryHandle() &&
    "Null pointer does not have handles!");
  DIMemoryHandleBase **PrevPtr = getPrevPtr();
  assert(*PrevPtr == this && "List invariant broken");
  *PrevPtr = mNext;
  if (mNext) {
    assert(mNext->getPrevPtr() == &mNext && "List invariant broken!");
    mNext->setPrevPtr(PrevPtr);
    return;
  }
  // If the mNext pointer was null, then it is possible that this was the last
  // MemoryHandle watching memory. If so, delete its entry from
  // the MemoryHandles map.
  auto &Handles = mMemory->getEnv().getMemoryHandles();
  if (Handles.isPointerIntoBucketsArray(PrevPtr)) {
    Handles.erase(mMemory);
    mMemory->setHasMemoryHandle(false);
  }
}

void DIMemoryHandleBase::memoryIsDeleted(DIMemory *M) {
  assert(M->hasMemoryHandle() &&
    "Should only be called if DIMemoryHandles present!");
  DIMemoryHandleBase *Entry = M->getEnv()[M];
  assert(Entry && "Memory bit set but no entries exist");
  // We use a local DIMemoryHandleBase as an iterator so that
  // DIMemoryHandles can add and remove themselves from the list without
  // breaking our iteration. This is not really an AssertingDIMemoryHandle; we
  // just have to give DIMemoryHandleBase some kind.
  for (DIMemoryHandleBase Itr(Assert, *Entry); Entry; Entry = Itr.mNext) {
    Itr.removeFromUseList();
    Itr.addToExistingUseListAfter(Entry);
    assert(Entry->mNext == &Itr && "Loop invariant broken.");
    switch (Entry->getKind()) {
    default:
      llvm_unreachable("Unsupported DIMemoryHandle!");
    case Assert:
      break;
    case Weak:
      Entry->operator=(nullptr);
      break;
    case Callback:
      static_cast<CallbackDIMemoryHandle *>(Entry)->deleted();
      break;
    }
  }
  if (M->hasMemoryHandle()) {
#ifndef NDEBUG
    dbgs() << "While deleting: ";
    TSAR_LLVM_DUMP(M->getAsMDNode()->dump());
    TSAR_LLVM_DUMP(M->getBaseAsMDNode()->dump());
    if (M->getEnv()[M]->getKind() == Assert)
      llvm_unreachable("An asserting memory handle still pointed to this memory!");
#endif
    llvm_unreachable("All references to M were not removed?");
  }
}

void DIMemoryHandleBase::memoryIsRAUWd(DIMemory *Old, DIMemory *New) {
  assert(Old->hasMemoryHandle() &&
    "Should only be called if MemoryHandles present!");
  assert(Old != New && "Changing value into itself!");
  DIMemoryHandleBase *Entry = Old->getEnv()[Old];
  assert(Entry && "Memory bit set but no entries exist");
  // We use a local DIMemoryHandleBase as an iterator so that
  // DIMemoryHandles can add and remove themselves from the list without
  // breaking our iteration.  This is not really an AssertingDIMemoryHandle; we
  // just have to give DIMemoryHandleBase some kind.
  for (DIMemoryHandleBase Itr(Assert, *Entry); Entry; Entry = Itr.mNext) {
    Itr.removeFromUseList();
    Itr.addToExistingUseListAfter(Entry);
    assert(Entry->mNext == &Itr && "Loop invariant broken.");
    switch (Entry->getKind()) {
    default:
      llvm_unreachable("Unsupported DIMemoryHandle!");
    case Assert:
      break;
    case Weak:
      Entry->operator=(New);
      break;
    case Callback:
      static_cast<CallbackDIMemoryHandle *>(Entry)->allUsesReplacedWith(New);
      break;
    }
  }
#ifndef NDEBUG
  // If any new weak value handles were added while processing the
  // list, then complain about it now.
  if (Old->hasMemoryHandle())
    for (Entry = Old->getEnv()[Old]; Entry; Entry = Entry->mNext)
      switch (Entry->getKind()) {
      case Weak:
        dbgs() << "After RAUW from ";
        TSAR_LLVM_DUMP(Old->getAsMDNode()->dump());
        TSAR_LLVM_DUMP(Old->getBaseAsMDNode()->dump());
        dbgs() << "to ";
        TSAR_LLVM_DUMP(New->getAsMDNode()->dump());
        TSAR_LLVM_DUMP(New->getBaseAsMDNode()->dump());
        llvm_unreachable("A weak value handle still pointed to the"
                         " old value!\n");
      default:
        break;
      }
#endif
}

void DIMemoryHandleBase::addToExistingUseList(DIMemoryHandleBase **List) {
  assert(List && "Handle list must not be null!");
  mNext = *List;
  *List = this;
  setPrevPtr(List);
  if (mNext) {
    mNext->setPrevPtr(&mNext);
    assert(mMemory == mNext->mMemory && "Handle was added to a wrong list!");
  }
}

void DIMemoryHandleBase::addToExistingUseListAfter(DIMemoryHandleBase *Node) {
  assert(Node && "Must insert after existing node!");
  mNext = Node->mNext;
  setPrevPtr(&Node->mNext);
  Node->mNext = this;
  if (mNext)
    mNext->setPrevPtr(&mNext);
}

DIAliasTree::DIAliasTree(llvm::Function &F) :
    mTopLevelNode(new DIAliasTopNode(this)), mFunc(&F) {
  ++NumAliasNode;
  mNodes.push_back(mTopLevelNode);
}

std::pair<DIAliasTree::memory_iterator, std::unique_ptr<DIEstimateMemory>>
DIAliasTree::addNewNode(
    std::unique_ptr<DIEstimateMemory> &&EM, DIAliasNode &Parent) {
  assert(EM && "Memory must not be null!");
  auto *Tmp = EM.release();
  memory_iterator Itr = mFragments.end();
  bool IsInserted = false;
  std::tie(Itr, IsInserted) = mFragments.insert(Tmp);
  if (!IsInserted)
    return std::make_pair(Itr, std::unique_ptr<DIEstimateMemory>(Tmp));
  assert(!Itr->getAliasNode() &&
    "Memory location is already attached to a node!");
  ++NumAliasNode, ++NumEstimateNode;
  auto *N = new DIAliasEstimateNode(this);
  mNodes.push_back(N);
  N->setParent(Parent);
  Itr->setAliasNode(*N);
  N->push_back(*Itr);
  return std::make_pair(Itr, nullptr);
}

std::pair<DIAliasTree::memory_iterator, std::unique_ptr<DIMemory>>
DIAliasTree::addNewUnknownNode(
    std::unique_ptr<DIMemory> &&M, DIAliasNode &Parent) {
  assert(M && "Memory must not be null!");
  auto *Tmp = M.release();
  memory_iterator Itr = mFragments.end();
  bool IsInserted = false;
  std::tie(Itr, IsInserted) = mFragments.insert(Tmp);
  if (!IsInserted)
    return std::make_pair(Itr, std::unique_ptr<DIMemory>(Tmp));
  assert(!Itr->getAliasNode() &&
    "Memory location is already attached to a node!");
  ++NumAliasNode, ++NumUnknownNode;
  auto *N = new DIAliasUnknownNode(this);
  mNodes.push_back(N);
  N->setParent(Parent);
  Itr->setAliasNode(*N);
  N->push_back(*Itr);
  return std::make_pair(Itr, nullptr);
}

std::pair<DIAliasTree::memory_iterator, std::unique_ptr<DIMemory>>
DIAliasTree::addToNode(std::unique_ptr<DIMemory> &&M, DIAliasMemoryNode &N) {
  assert(M && "Memory must not be null!");
  assert((!isa<DIAliasEstimateNode>(N) || isa<DIEstimateMemory>(M)) &&
    "Alias estimate node may contain estimate memory locations only!");
  auto *Tmp = M.release();
  memory_iterator Itr = mFragments.end();
  bool IsInserted = false;
  std::tie(Itr, IsInserted) = mFragments.insert(Tmp);
  if (!IsInserted)
    return std::make_pair(Itr, std::unique_ptr<DIMemory>(Tmp));
  assert(!Itr->getAliasNode() &&
    "Memory location is already attached to a node!");
  Itr->setAliasNode(N);
  N.push_back(*Itr);
  return std::make_pair(Itr, nullptr);
}

void DIAliasTree::erase(DIAliasMemoryNode &N) {
  for (auto &M : N) {
    mFragments.erase(&M);
    delete &M;
  }
  N.remove();
  mNodes.erase(N);
}

std::pair<bool, bool> DIAliasTree::erase(DIMemory &M) {
  auto Itr = mFragments.find(&M);
  if (Itr == mFragments.end())
    return std::make_pair(false, false);
  auto Node = M.getAliasNode();
  assert(Node && "Alias node must not be null!");
  DIAliasMemoryNode::remove(M);
  mFragments.erase(Itr);
  delete &M;
  if (Node->empty()) {
    erase(*Node);
    return std::make_pair(true, true);
  }
  return std::make_pair(true, false);
}

namespace {
#ifdef LLVM_DEBUG
template<class ItrTy>
void constantOffsetLog(const ItrTy &I, const ItrTy E, const DominatorTree &DT) {
  dbgs() <<
    "[DI ALIAS TREE]: find estimate memory locations with constant offset from root\n";
  for (auto &PtrToOffset : make_range(I, E)) {
    dbgs() << "[DI ALIAS TREE]: pointer to an estimate memory location ";
    printLocationSource(dbgs(), PtrToOffset.first, &DT);
    dbgs() << " with constant offset " << PtrToOffset.second << " from root\n";
  }
}

void ignoreEMLog(const EstimateMemory &EM, const DominatorTree &DT) {
  dbgs() << "[DI ALIAS TREE]: ignore estimate memory location: ";
  printLocationSource(dbgs(), MemoryLocation(EM.front(), EM.getSize()), &DT);
  dbgs() << " Parent={";
  if (auto Parent = EM.getParent())
    printLocationSource(dbgs(),
      MemoryLocation(Parent->front(), Parent->getSize()), &DT);
  else
    dbgs() << "NULL";
  dbgs() << "}";
  dbgs() << " Children={";
  for (auto &Ch : make_range(EM.child_begin(), EM.child_end())) {
    printLocationSource(dbgs(), MemoryLocation(Ch.front(), Ch.getSize()), &DT);
    dbgs() << " ";
  }
  dbgs() << "}\n";
}

void buildMemoryLog(Function &F, const DominatorTree &DT,
  DIMemory &DIM, EstimateMemory &EM) {
  dbgs() << "[DI ALIAS TREE]: build debug memory location ";
  printDILocationSource(*getLanguage(F), DIM, dbgs());
  dbgs() << " for ";
  printLocationSource(dbgs(), MemoryLocation{ EM.front(), EM.getSize() }, &DT);
  dbgs() << "\n";
}

void buildMemoryLog(Function &F, DIMemory &DIM, Value &V) {
  dbgs() << "[DI ALIAS TREE]: build debug memory location ";
  printDILocationSource(*getLanguage(F), DIM, dbgs());
  dbgs() << " for ";
  if (auto Callee = [&V]() -> llvm::Function * {
        if (auto *Call = dyn_cast<CallBase>(&V))
          return dyn_cast<Function>(
              Call->getCalledOperand()->stripPointerCasts());
        return nullptr;
      }())
    Callee->printAsOperand(dbgs(), false);
  else
    V.printAsOperand(dbgs(), false);
  dbgs() << "\n";
}

void addCorruptedLog(Function &F, DIMemory &DIM) {
  dbgs() << "[DI ALIAS TREE]: add corrupted memory location ";
  printDILocationSource(*getLanguage(F), DIM, dbgs());
  dbgs() << " to the node\n";
}

void addMemoryLog(Function &F, DIMemory &EM) {
  dbgs() << "[DI ALIAS TREE]: add memory location ";
  printDILocationSource(*getLanguage(F), EM, dbgs());
  dbgs() << " to the node \n";
}
#endif

/// If a specified value is 'inttoptr' operator then it is stripped
/// otherwise original value is returned.
///
/// In case of 'inttoptr' different values may have the same base.
/// The following values will be binded to the same DIMemory:
/// 'inttoptr i64 %Y to i64*'
/// 'inttoptr i64 %Y to %struct.S*'
/// So, we use %Y to represent corresponding DIUnknownMemory location
const Value *stripIntToPtr(const Value *V) {
  if (Operator::getOpcode(V) != Instruction::IntToPtr)
    return V;
  V = cast<Operator>(V)->getOperand(0);
  if (auto LI = dyn_cast<LoadInst>(V))
    return LI->getPointerOperand();
  return V;
}

/// If a specified value is 'inttoptr' operator then it is stripped
/// otherwise original value is returned.
Value *stripIntToPtr(Value *V) {
  return const_cast<Value *>(stripIntToPtr(static_cast<const Value *>(V)));
}

/// This is a map from list of corrupted memory locations to an unknown
/// node in a debug alias tree.
using CorruptedMap = DenseMap<CorruptedMemoryItem *, DIAliasNode *>;

/// \brief Adds an unknown node (if necessary) into the tree and returns it.
///
/// If some node already contains list of specified corrupted locations it
/// will be returned. If a specified parent is unknown node than a new node
/// will not be created. Memory locations from `Corrupted` will be moved to
/// the unknown node and `CorruptedNodes` map will be updated.
DIAliasNode * addCorruptedNode(
    DIAliasTree &DIAT, CorruptedMemoryItem *Corrupted, DIAliasNode *DIParent,
    CorruptedMap &CorruptedNodes) {
  assert(Corrupted && "List of corrupted memory location must not be null!");
  auto Itr = CorruptedNodes.find(Corrupted);
  if (Itr != CorruptedNodes.end()) {
    assert((DIParent == Itr->second || Itr->second->getParent() == DIParent) &&
      "Unknown node does not linked with currently processed node!");
    return Itr->second;
  }
  if (!isa<DIAliasUnknownNode>(DIParent)) {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias unknown node\n");
    auto DIM = Corrupted->pop();
    LLVM_DEBUG(addCorruptedLog(DIAT.getFunction(), *DIM));
    auto Info = DIAT.addNewUnknownNode(std::move(DIM), *DIParent);
    /// TODO (kaniandr@gmail.com): implement construction of duplicates.
    /// For example, it is possible to add count of duplicates into memory
    /// representation and reference to the first location.
    if (Info.second)
      llvm_unreachable("Construction of duplicates is not supported yet!");
    DIParent = Info.first->getAliasNode();
  }
  CorruptedNodes.try_emplace(Corrupted, cast<DIAliasUnknownNode>(DIParent));
  for (CorruptedMemoryItem::size_type I = 0, E = Corrupted->size(); I < E; ++I) {
    auto DIM = Corrupted->pop();
    LLVM_DEBUG(addCorruptedLog(DIAT.getFunction(), *DIM));
    auto Info =
      DIAT.addToNode(std::move(DIM), cast<DIAliasUnknownNode>(*DIParent));
    if (Info.second) {
      if (Info.first->getAliasNode() == DIParent) {
        for (auto &VH : *Info.second)
          Info.first->bindValue(VH);
        // Mark location as merged in RAUW.
        Info.first->setProperties(DIMemory::Merged);
        Info.second->replaceAllUsesWith(&*Info.first);
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: merge location with existent "
          "one in the current node\n");
      } else {
        /// TODO (kaniandr@gmail.com): implement construction of duplicates.
        /// For example, it is possible to add count of duplicates into memory
        /// representation and reference to the first location.
        llvm_unreachable("Construction of duplicates is not supported yet!");
      }
    }
  }
  return DIParent;
}

/// \brief Traverses alias tree to build its debug version.
///
/// \param [in] RootOffsets A list of locations which are used to build
/// debug memory locations. Other locations will be ignored.
/// \param [in, out] CMR This contains information about corrupted memory
/// locations. These locations will be inserted in unknown nodes. This
/// locations will be moved outside appropriated lists. Correspondence
/// between lists of corrupted locations (already empty) and unknown nodes will
/// be stored in `CorruptedNodes` map.
/// \param [in, out] DIAT Constructed debug alias tree.
/// \param [in] Parent Currently processed alias node. This function performs
/// depth first traversal of alias tree recursively.
/// \param [in, out] DIParent A node in the constructed debug alias tree which
/// is used as a root for new nodes.
/// \param [in, out] Map from lists of corrupted locations to unknown nodes.
void buildDIAliasTree(const DataLayout &DL, const DominatorTree &DT,
    DIMemoryEnvironment &Env,
    const DenseMap<const Value *, int64_t> &RootOffsets,
    CorruptedMemoryResolver &CMR, DIAliasTree &DIAT,
    AliasNode &Parent, DIAliasNode &DIParent, CorruptedMap &Nodes) {
  for (auto &Child : make_range(Parent.child_begin(), Parent.child_end())) {
    auto DIN = &DIParent;
    if (auto Corrupted = CMR.hasUnknownParent(Child))
      DIN = addCorruptedNode(DIAT, Corrupted, DIN, Nodes);
    SmallVector<std::unique_ptr<DIEstimateMemory>, 8> Known;
    SmallVector<std::unique_ptr<DIMemory>, 8> Unknown;
    if (auto N = dyn_cast<AliasEstimateNode>(&Child)) {
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: process alias estimate node\n");
      for (auto &EM : *N) {
        auto Root = EM.getTopLevelParent();
        if (&EM != Root && !RootOffsets.count(EM.front())) {
          LLVM_DEBUG(ignoreEMLog(EM, DT));
          continue;
        }
        std::unique_ptr<DIMemory> DIM;
        if (!(DIM = CMR.popFromCache(&EM))) {
          DIM = buildDIMemory(EM, DIAT.getFunction().getContext(), Env, DL, DT);
          LLVM_DEBUG(buildMemoryLog(DIAT.getFunction(), DT, *DIM, EM));
        }
        if (CMR.isCorrupted(DIM->getAsMDNode()).first) {
          LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: ignore corrupted memory location\n");
          continue;
        }
        if (isa<DIEstimateMemory>(*DIM))
          Known.emplace_back(cast<DIEstimateMemory>(DIM.release()));
        else
          Unknown.push_back(std::move(DIM));
      }
    } else {
      assert(isa<AliasUnknownNode>(Child) && "It must be alias unknown node!");
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: process alias unknown node\n");
      for (auto Inst : cast<AliasUnknownNode>(Child)) {
        std::unique_ptr<DIMemory> DIM;
        if (!(DIM = CMR.popFromCache(Inst, true))) {
          DIM = buildDIMemory(*Inst, DIAT.getFunction().getContext(), Env, DT);
          LLVM_DEBUG(buildMemoryLog(DIAT.getFunction(), *DIM, *Inst));
        }
        if (CMR.isCorrupted(DIM->getAsMDNode()).first) {
          LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: ignore corrupted memory location\n");
          continue;
        }
        Unknown.push_back(std::move(DIM));
      }
    }
    if (!Unknown.empty()) {
      if (!isa<DIAliasUnknownNode>(DIN)) {
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias unknown node\n");
        LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *Unknown.back()));
        auto Info = DIAT.addNewUnknownNode(std::move(Unknown.back()), *DIN);
        /// TODO (kaniandr@gmail.com): implement construction of duplicates.
        /// For example, it is possible to add count of duplicates into memory
        /// representation and reference to the first location.
        if (Info.second)
          llvm_unreachable("Construction of duplicates is not supported yet!");
        DIN = Info.first->getAliasNode();
        Unknown.pop_back();
      }
      for (auto &M : Unknown) {
        LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *M));
        auto Info = DIAT.addToNode(std::move(M), cast<DIAliasMemoryNode>(*DIN));
        if (Info.second) {
          if (Info.first->getAliasNode() == DIN) {
            for (auto &VH : *Info.second)
              Info.first->bindValue(VH);
            // Mark location as merged in RAUW.
            Info.first->setProperties(DIMemory::Merged);
            Info.second->replaceAllUsesWith(&*Info.first);
            LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: merge location with existent "
              "one in the current node\n");
          } else {
            /// TODO (kaniandr@gmail.com): implement construction of duplicates.
            /// For example, it is possible to add count of duplicates into memory
            /// representation and reference to the first location.
            llvm_unreachable("Construction of duplicates is not supported yet!");
          }
        }
      }
    }
    if (!Known.empty()) {
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias estimate node\n");
      LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *Known.back()));
      auto Info = DIAT.addNewNode(std::move(Known.back()), *DIN);
      /// TODO (kaniandr@gmail.com): implement construction of duplicates.
      /// For example, it is possible to add count of duplicates into memory
      /// representation and reference to the first location.
      if (Info.second)
        llvm_unreachable("Construction of duplicates is not supported yet!");
      DIN = Info.first->getAliasNode();
      Known.pop_back();
      for (auto &M : Known) {
        LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *M));
        auto Info = DIAT.addToNode(std::move(M), cast<DIAliasMemoryNode>(*DIN));
        if (Info.second) {
          if (Info.first->getAliasNode() == DIN) {
            for (auto &VH : *Info.second)
              Info.first->bindValue(VH);
            // Mark location as merged in RAUW.
            Info.first->setProperties(DIMemory::Merged);
            Info.second->replaceAllUsesWith(&*Info.first);
            LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: merge location with existent "
              "one in the current node\n");
          } else {
            /// TODO (kaniandr@gmail.com): implement construction of duplicates.
            /// For example, it is possible to add count of duplicates into memory
            /// representation and reference to the first location.
            llvm_unreachable("Construction of duplicates is not supported yet!");
          }
        }
      }
    }
    buildDIAliasTree(DL, DT, Env, RootOffsets, CMR, DIAT, Child, *DIN, Nodes);
  }
}

/// This class inserts new subtree into an alias tree.
///
/// The subtree will contains a list of specified fragments of a specified
/// variable. This fragments should not alias with other memory location in the
/// alias tree. This is a precondition and is not checked by a builder.
/// It also tries to build fragments which extends specified fragments.
/// For example for the fragment S.X[2] the following locations will be
/// constructed S -> S.X -> S.X[2]. However, the last one only will be marked as
/// explicitly accessed memory location.
class DIAliasTreeBuilder {
  /// Projection of an array to an array with less number of dimensions.
  ///
  /// In case of array A[2][3][4] there are two levels of planes.
  /// First level contains 2 planes of size 3 * 4 = 12.
  /// The second level contains 3 planes of size 4.
  struct PlaneTy {
    uint64_t NumberOfPlanes;
    uint64_t SizeOfPlane;
  };
public:
  /// Creates a builder of subtree which contains specified fragments of a
  /// variable.
  DIAliasTreeBuilder(DIAliasTree &DIAT, LLVMContext &Ctx,
      DIMemoryEnvironment &Env,
      CorruptedMemoryResolver &CMR, CorruptedMap &CorruptedNodes,
      DIVariable *Var, DILocation *DbgLoc,
      const TinyPtrVector<DIExpression *> &Fragments) :
      mDIAT(&DIAT), mContext(&Ctx), mEnv(&Env),
      mCMR(&CMR), mCorruptedNodes(&CorruptedNodes),
      mVar(Var),  mSortedFragments(Fragments) {
    assert(mDIAT && "Alias tree must not be null!");
    assert(mVar && "Variable must not be null!");
    assert(!Fragments.empty() && "At least one fragment must be specified!");
    if (DbgLoc)
      mDbgLocs.push_back(DbgLoc);
    std::sort(mSortedFragments.begin(), mSortedFragments.end(),
      [this](DIExpression *LHS, DIExpression *RHS) {
        assert(!mayAliasFragments(*LHS, *RHS) && "Fragments must not be alias!");
        auto InfoLHS = LHS->getFragmentInfo();
        auto InfoRHS = RHS->getFragmentInfo();
        // Empty expression is no possible here because the whole location is
        // alias with any fragment. In this case list of fragments will contain
        // only single expression and this function will not be called.
        assert(LHS->getNumElements() == 3 && InfoLHS.hasValue() &&
          "Expression may contain dwarf::DW_OP_LLVM_fragment only!");
        assert(RHS->getNumElements() == 3 && InfoRHS.hasValue() &&
          "Expression may contain dwarf::DW_OP_LLVM_fragment only!");
        return InfoLHS->OffsetInBits < InfoRHS->OffsetInBits;
    });
  }

  /// Builds a subtree of an alias tree.
  void buildSubtree() {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: build subtree for a variable "
      << mVar->getName() << "\n");
    DIAliasNode *Parent = mDIAT->getTopLevelNode();
    if (auto *Corrupted = mCMR->hasUnknownParent(*mVar)) {
      Parent = addCorruptedNode(*mDIAT, Corrupted, Parent, *mCorruptedNodes);
      if (auto N = dyn_cast<DIAliasUnknownNode>(Parent))
        mVisitedCorrupted.insert(N);
    }
    auto Ty = stripDIType(mVar->getType());
    if (!Ty) {
      addFragments(Parent, 0, mSortedFragments.size());
      return;
    }
    auto DIEmptyExpr = DIExpression::get(*mContext, {});
    if (mSortedFragments.front()->getNumElements() == 0) {
      auto DIMVar = DIEstimateMemory::get(*mContext, *mEnv, mVar, DIEmptyExpr,
        DIEstimateMemory::AfterPointer, mDbgLocs);
      auto IsCorruptedRoot = mCMR->isCorrupted(DIMVar->getBaseAsMDNode());
      if (IsCorruptedRoot.first) {
        if (IsCorruptedRoot.second)
          return;
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: replace corrupted\n");
        eraseAndReplaceCorrupted(DIMVar.get(), Parent);
      }
      LLVM_DEBUG(addFragmentLog(DIEmptyExpr));
      auto DIM = mDIAT->addNewNode(std::move(DIMVar), *Parent);
      assert(!DIM.second && "Memory location is already attached to a node!");
      if (auto M =
              mCMR->beforePromotion(DIMemoryLocation::get(mVar, DIEmptyExpr)))
        M->replaceAllUsesWith(&*DIM.first);
      DIM.first->setProperties(DIMemory::Explicit);
      return;
    }
    auto LastInfo = mSortedFragments.back()->getFragmentInfo();
    if (LastInfo->OffsetInBits / 8 + (LastInfo->SizeInBits + 7) / 8 >
        getSize(Ty)) {
      addFragments(Parent, 0, mSortedFragments.size());
      return;
    }
    evaluateTy(
      DIEmptyExpr, Ty, 0, std::make_pair(0, mSortedFragments.size()), Parent);
  }

private:
#ifdef LLVM_DEBUG
  void addFragmentLog(llvm::DIExpression *Expr) {
    dbgs() << "[DI ALIAS TREE]: add a new node and a new fragment ";
    auto DWLang = getLanguage(mDIAT->getFunction()).getValue();
    printDILocationSource(DWLang, DIMemoryLocation::get(mVar, Expr), dbgs());
    dbgs() << "\n";
  }
#endif

  /// \brief Add fragments from a specified range [BeginIdx, EndIdx)
  /// into an alias tree.
  ///
  /// Each fragments will be stored in a separate node with a parent `Parent`.
  void addFragments(DIAliasNode *Parent, unsigned BeginIdx, unsigned EndIdx) {
    assert(Parent && "Alias node must not be null!");
    for (unsigned I = BeginIdx; I < EndIdx; ++I) {
      LLVM_DEBUG(addFragmentLog(mSortedFragments[I]));
      Parent = addUnknownParentIfNecessary(Parent, mSortedFragments[I]);
      auto DIM = mDIAT->addNewNode(
        DIEstimateMemory::get(*mContext, *mEnv, mVar, mSortedFragments[I],
          DIEstimateMemory::NoFlags, mDbgLocs),
        *Parent);
      assert(!DIM.second && "Memory location is already attached to a node!");
      if (auto M = mCMR->beforePromotion(
            DIMemoryLocation::get(mVar, mSortedFragments[I])))
        M->replaceAllUsesWith(&*DIM.first);
      DIM.first->setProperties(DIMemory::Explicit);
    }
  }

  /// Inserts corrupted node which is a parent for a node with a specified
  /// fragment (`mVar`, `Expr`) if this is necessary.
  DIAliasNode * addUnknownParentIfNecessary(
      DIAliasNode *Parent, DIExpression *Expr) {
    auto *Corrupted = mCMR->hasUnknownParent(DIMemoryLocation::get(mVar, Expr));
    if (!Corrupted)
      return Parent;
    Corrupted->erase(
      mCorruptedReplacement.begin(), mCorruptedReplacement.end());
    // Node may already exist in the set, however it may be invalid. So, we
    // update it.
    // void foo() { for (int I = 0; I < 10; ++I) { int X[10]; X[0] = I; } }
    // At first, unknown node for corrupted <X,40> would be created. However,
    // corrupted <X,40> will be replaced with a fragment and unknown node will
    // be removed. After this mCorruptedNodes contains an empty 'Corrupted'
    // which points to a deleted pointer.
    if (Corrupted->empty())
      return mCorruptedNodes->try_emplace(Corrupted).first->second = Parent;
    Parent = addCorruptedNode(*mDIAT, Corrupted, Parent, *mCorruptedNodes);
    if (auto N = dyn_cast<DIAliasUnknownNode>(Parent))
      mVisitedCorrupted.insert(N);
    return Parent;
  }

  /// \brief Removes corrupted node equal to a specified node and replace its
  /// uses with a specified one.
  ///
  /// \return `True` if corrupted node has been found in `mVisitedCorrupted`
  /// list and removed.
  /// \post If a removed corrupted has been the last one location in the node
  /// the node will be removed. If `Current`equal to a removed node it will be
  /// set to it's parent.
  bool eraseAndReplaceCorrupted(DIMemory *What, DIAliasNode *&Current) {
    auto *MD = What->getBaseAsMDNode();
    for (auto *N : mVisitedCorrupted)
      for (auto &M : *N)
        if (M.getBaseAsMDNode() == MD) {
          --NumCorruptedMemory;
          isa<DIEstimateMemory>(M) ? --NumEstimateMemory : --NumUnknownMemory;
          auto Parent = Current->getParent();
          M.replaceAllUsesWith(What);
          if (mDIAT->erase(M).second) {
            Current = (N == Current) ? Parent : Current;
            mVisitedCorrupted.erase(N);
            --NumUnknownNode;
            --NumAliasNode;
          }
          return true;
        }
    return false;
  }

  /// Add new node with a specified parent into alias tree.
  ///
  /// A specified 'Expr' determines location which will contain a node.
  /// If appropriate memory location is corrupted new node is not created.
  DIAliasNode * addNewNode(DIExpression *Expr, DIAliasNode *Parent) {
    assert(Expr && "Expression must not be null!");
    assert(Parent && "Alias node must not be null!");
    auto DIMTmp = DIEstimateMemory::get(*mContext, *mEnv, mVar, Expr,
      DIEstimateMemory::NoFlags, mDbgLocs);
    auto IsCorrupted = mCMR->isCorrupted(DIMTmp->getBaseAsMDNode());
    if (IsCorrupted.first) {
      if (!IsCorrupted.second) {
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: replace corrupted\n");
        bool IsErased = eraseAndReplaceCorrupted(DIMTmp.get(), Parent);
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add internal fragment\n");
        LLVM_DEBUG(addFragmentLog(DIMTmp->getExpression()));
        auto NewM = mDIAT->addNewNode(std::move(DIMTmp), *Parent);
        assert(!NewM.second && "Memory location is already attached to a node!");
        // The corrupted has not been inserted into alias tree yet.
        // So it should be remembered to prevent such insertion later.
        if (!IsErased)
          mCorruptedReplacement.insert(NewM.first->getBaseAsMDNode());
        Parent = NewM.first->getAliasNode();
      }
    } else {
      Parent = addUnknownParentIfNecessary(Parent, Expr);
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add internal fragment\n");
      LLVM_DEBUG(addFragmentLog(DIMTmp->getExpression()));
      auto Info = mDIAT->addNewNode(std::move(DIMTmp), *Parent);
      assert(!Info.second && "Memory location is already attached to a node!");
      Parent = Info.first->getAliasNode();
    }
    return Parent;
  }

  /// \brief Add subtypes of a specified type into the alias tree.
  ///
  /// A pair of `mVar` and `Expr` will represent a specified type in the tree.
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateTy(DIExpression *Expr, DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    Parent = addNewNode(Expr, Parent);
    assert(Ty && "Type must not be null!");
    switch (Ty->getTag()) {
    default:
      addFragments(Parent, Fragments.first, Fragments.second);
      break;
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
      evaluateStructureTy(Ty, Offset, Fragments, Parent);
      break;
    case dwarf::DW_TAG_array_type:
      evaluateArrayTy(Ty, Offset, Fragments, Parent);
      break;
    }
  }


  /// \brief Add subtypes of a specified type into the alias tree.
  ///
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateTy(DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Ty && "Type must not be null!");
    assert(Parent && "Alias node must not be null!");
    auto Size = getSize(Ty);
    auto FInfo = mSortedFragments[Fragments.first]->getFragmentInfo();
    if (Fragments.first + 1 == Fragments.second &&
        FInfo->OffsetInBits / 8 == Offset &&
        (FInfo->SizeInBits + 7) / 8 == Size) {
      addFragments(Parent, Fragments.first, Fragments.second);
      return;
    }
    auto Expr = DIExpression::get(*mContext, {
      dwarf::DW_OP_LLVM_fragment, Offset * 8, Ty->getSizeInBits()});
    evaluateTy(Expr, Ty, Offset, Fragments, Parent);
  }

  /// Determine fragments covered by a specified range [Offset, Offset+Size].
  ///
  /// \return True if at least one covered fragment has been found. The second
  /// returned value as an index of a fragment after the last covered.
  /// If returned value is `false` then second value contains an index of
  /// fragment which cross border of a checked range.
  std::pair<bool, unsigned> findCoveredFragments(uint64_t Offset, uint64_t Size,
    unsigned FragmentIdx, unsigned FragmentIdxE) {
    bool IsFragmentsCoverage = false;
    for (; FragmentIdx < FragmentIdxE;
      ++FragmentIdx, IsFragmentsCoverage = true) {
      auto FInfo = mSortedFragments[FragmentIdx]->getFragmentInfo();
      auto FOffset = FInfo->OffsetInBits / 8;
      auto FSize = (FInfo->SizeInBits + 7) / 8;
      // If condition is true than fragments in
      // [FirstFragmentIdx, FragmentIdx) is covered.
      if (FOffset >= Offset + Size)
        return std::make_pair(IsFragmentsCoverage, FragmentIdx);
      // If condition is true than FragmentIdx cross the border of a checked
      // range and it is necessary to extend coverage.
      if (FOffset + FSize > Offset + Size)
        return std::make_pair(false, FragmentIdx);
    }
    return std::make_pair(IsFragmentsCoverage, FragmentIdx);
  }

  /// \brief Build coverage of fragments from a specified range
  /// [Fragments.first, Fragments.second) and update alias tree.
  ///
  /// The range of elements will be splitted into subranges. Each subrange
  /// is covered by some subsequence of elements.
  /// Adds a specified element into alias tree if it full covers some fragments
  /// from a specified range and evaluates its base type recursively.
  /// If fragments cross border of elements before the current one then elements
  /// will not be added into the alias tree. In this case only fragments will
  /// be inserted after full coverage will be constructed (sequence of elements
  /// which full covers a subrange of fragments).
  /// \param [in, out] FragmentIdx Index of a fragment in the range. It will be
  /// increased after function call.
  /// \param [in, out] FirstElIdx Index of a first element in a coverage which
  /// is under construction.
  /// \param [in, out] FirstFragmentIdx Index of a first fragment in a range
  /// which is not covered yet.
  void evaluateElement(uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent,
      DIType *ElTy, unsigned ElIdx, uint64_t ElOffset, uint64_t ElSize,
      unsigned &FirstElIdx, unsigned &FirstFragmentIdx, unsigned &FragmentIdx) {
    bool IsFragmentsCoverage;
    std::tie(IsFragmentsCoverage, FragmentIdx) = findCoveredFragments(
      Offset + ElOffset, ElSize, FragmentIdx, Fragments.second);
    if (!IsFragmentsCoverage)
      return;
    if (FirstElIdx == ElIdx) {
      evaluateTy(ElTy, Offset + ElOffset,
        std::make_pair(FirstFragmentIdx, FragmentIdx), Parent);
    } else {
      // A single element of a aggregate type does not cover set of fragments.
      // In this case elements that comprises a coverage will not be added
      // into alias tree. Instead only fragments will be inserted.
      addFragments(Parent, FirstFragmentIdx, FragmentIdx);
    }
    FirstElIdx = ElIdx + 1;
    FirstFragmentIdx = FragmentIdx;
  }

  /// Determine number of projections, number of planes at each projection and
  /// number of each planes.
  bool buildPlanes(const DICompositeType &DICTy, uint64_t ElSize,
    SmallVectorImpl<PlaneTy> &Planes) {
    assert(DICTy.getTag() == dwarf::DW_TAG_array_type &&
      "Type to evaluate must be a structure or class!");
    auto ArrayDims = DICTy.getElements();
    Planes.resize(ArrayDims.size(), { 0, ElSize });
    auto processDim = [&ArrayDims, &Planes](unsigned DimIdx) {
      auto *DIDim = dyn_cast<DISubrange>(ArrayDims[DimIdx]);
      if (!DIDim)
        return;
      auto Size = getConstantCount(*DIDim);
      if (!Size)
        return;
      Planes[DimIdx].NumberOfPlanes = *Size;
      for (unsigned I = 0; I < DimIdx; ++I)
        Planes[I].SizeOfPlane *= *Size;
    };
    auto DWLang = getLanguage(mDIAT->getFunction());
    if (!DWLang)
      return false;
    if (isForwardDim(*DWLang)) {
      for (unsigned DimIdx = 0, DimIdxE = ArrayDims.size();
           DimIdx < DimIdxE; ++DimIdx)
        processDim(DimIdx);
    } else {
      for (unsigned DimIdxE = 0, DimIdx = ArrayDims.size();
           DimIdx > DimIdxE; ++DimIdx)
        processDim(DimIdx - 1);
    }
    Planes.pop_back();
    return !Planes.empty();
  }

  /// Recursively add projection of an array to an array with less number
  /// of dimensions.
  ///
  /// The main goal is to build coverage of fragments from a specified range
  /// [Fragments.first, Fragments.second) and update alias tree. Add a plane
  /// (projection) into alias tree if it full covers some fragments from a
  /// specified range.
  /// If fragments cross border of a plane then this plane will not be added
  /// into the alias tree. In this case only fragments will be inserted.
  /// \param [in] Planes List of pairs: number of planes, size of each plane.
  /// \param [in] Level Currently processed projection.
  /// In case of array A[2][3][4] there are two levels. First level contains
  /// 2 planes of size 3 * 4 = 12. The second level contains 3 planes of size 4.
  /// \param [in, out] FragmentIdx Index of a fragment in the range. It will be
  /// increased after function call.
  /// \param [in, out] FirstFragmentIdx Index of a first fragment in a range
  /// which is not covered yet.
  void evaluatePlaneLevel(uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent,
      DIType *ElTy, uint64_t ElSize, ArrayRef<PlaneTy> Planes, unsigned Level,
      unsigned &FirstFragmentIdx, unsigned &FragmentIdx) {
    assert(Level < Planes.size() && "Level does not exist!");
    for (uint64_t  CoverageSize = 0, PrevPlaneIdx = 0, PlaneIdx = 0,
         PlaneIdxE = Planes[Level].NumberOfPlanes; PlaneIdx < PlaneIdxE; ++PlaneIdx) {
      CoverageSize += Planes[Level].SizeOfPlane;
      bool IsFragmentsCoverage;
      std::tie(IsFragmentsCoverage, FragmentIdx) = findCoveredFragments(
        Offset, CoverageSize, FragmentIdx, Fragments.second);
      if (!IsFragmentsCoverage)
        continue;
      // Check that fragments is covered by a single plane.
      if (PrevPlaneIdx == PlaneIdx) {
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: build plane at level " << Level << "\n");
        auto Expr = DIExpression::get(*mContext, {
          dwarf::DW_OP_LLVM_fragment, Offset * 8, CoverageSize * 8 });
        auto Plane = addNewNode(Expr, Parent);
        auto CoveredFragments = std::make_pair(FirstFragmentIdx, FragmentIdx);
        FragmentIdx = FirstFragmentIdx;
        if (Level + 1 == Planes.size()) {
          // All planes have been processed. So, add elements of an array.
          unsigned FirstElIdx = 0, ElIdx = 0;
          for (uint64_t ElOffset = 0;
               ElOffset < CoverageSize && FragmentIdx < CoveredFragments.second;
               ElOffset += ElSize, ElIdx++)
            evaluateElement(Offset, CoveredFragments, Plane, ElTy, ElIdx,
              ElOffset, ElSize, FirstElIdx, FirstFragmentIdx, FragmentIdx);
        } else {
          evaluatePlaneLevel(Offset, CoveredFragments, Plane, ElTy, ElSize,
            Planes, Level + 1, FirstFragmentIdx, FragmentIdx);
        }
        addFragments(Plane, FirstFragmentIdx, FragmentIdx);
      } else {
        // A single plane does not cover set of fragments.
        // In this case planes that comprises a coverage will not be added
        // into alias tree. Instead only fragments will be inserted.
        addFragments(Parent, FirstFragmentIdx, FragmentIdx);
      }
      PrevPlaneIdx = PlaneIdx + 1;
      Offset += CoverageSize;
      CoverageSize = 0;
      FirstFragmentIdx = FragmentIdx;
    }
  }

  /// \brief Splits array type into its elements and adds them into
  /// the alias tree.
  ///
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateArrayTy(DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Ty && Ty->getTag() == dwarf::DW_TAG_array_type &&
      "Type to evaluate must be a structure or class!");
    assert(Parent && "Alias node must not be null!");
    auto DICTy = cast<DICompositeType>(Ty);
    auto ElTy = stripDIType(DICTy->getBaseType());
    auto ElSize = getSize(ElTy);
    auto FirstFragmentIdx = Fragments.first;
    auto FragmentIdx = Fragments.first;
    SmallVector<PlaneTy, 4> Planes;
    if (buildPlanes(*DICTy, ElSize, Planes)) {
      evaluatePlaneLevel(Offset, Fragments, Parent, ElTy, ElSize, Planes, 0,
        FirstFragmentIdx, FragmentIdx);
    } else {
      unsigned FirstElIdx = 0;
      unsigned ElIdx = 0;
      for (uint64_t ElOffset = 0, E = DICTy->getSizeInBits();
           ElOffset < E && FragmentIdx < Fragments.second;
           ElOffset += ElSize, ElIdx++) {
        evaluateElement(Offset, Fragments, Parent, ElTy,
          ElIdx, ElOffset, ElSize, FirstElIdx, FirstFragmentIdx, FragmentIdx);
      }
    }
    addFragments(Parent, FirstFragmentIdx, FragmentIdx);
  }

  /// \brief Splits structure type into its elements and adds them into
  /// the alias tree.
  ///
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateStructureTy(DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Ty &&
      (Ty->getTag() == dwarf::DW_TAG_structure_type ||
       Ty->getTag() == dwarf::DW_TAG_class_type) &&
      "Type to evaluate must be a structure or class!");
    assert(Parent && "Alias node must not be null!");
    auto DICTy = cast<DICompositeType>(Ty);
    auto FirstFragmentIdx = Fragments.first;
    auto FragmentIdx = Fragments.first;
    unsigned  FirstElIdx = 0;
    for (unsigned ElIdx = 0, ElEnd = DICTy->getElements().size();
         ElIdx < ElEnd && FragmentIdx < Fragments.second; ++ElIdx) {
      auto ElTy = cast<DIDerivedType>(
        stripDIType(cast<DIType>(DICTy->getElements()[ElIdx])));
      auto ElOffset = ElTy->getOffsetInBits() / 8;
      auto ElSize = getSize(ElTy);
      evaluateElement(Offset, Fragments, Parent, ElTy->getBaseType(),
        ElIdx, ElOffset, ElSize, FirstElIdx, FirstFragmentIdx, FragmentIdx);
    }
    addFragments(Parent, FirstFragmentIdx, FragmentIdx);
  }

  DIAliasTree *mDIAT;
  LLVMContext *mContext;
  DIMemoryEnvironment *mEnv;
  CorruptedMemoryResolver *mCMR;
  CorruptedMap *mCorruptedNodes;
  DIVariable *mVar;
  SmallVector<DILocation *, 1> mDbgLocs;
  TinyPtrVector<DIExpression *> mSortedFragments;
  SmallPtrSet<DIAliasUnknownNode *, 2> mVisitedCorrupted;
  SmallPtrSet<MDNode *, 4> mCorruptedReplacement;
};

/// \brief Returns list of estimate memory locations which should be inserted
/// into a constructed DIAliasTree.
///
/// The following location and their offsets are returned:
/// - It has a known offset from it's root in estimate memory tree.
/// - It's offset is not zero or it's size is not equal to size of root.
/// - All descendant locations in estimate memory tree have known offsets from
///   the root.
/// - There is no 'inttoptr' cast in the root.
DenseMap<const Value *, int64_t>
findLocationToInsert(const AliasTree &AT, const DataLayout &DL) {
  DenseMap<const Value *, int64_t> RootOffsets;
  for (auto AN : depth_first(&AT)) {
    if (!isa<AliasEstimateNode>(AN))
      continue;
    for (auto &EM : *cast<AliasEstimateNode>(AN)) {
      if (!EM.isLeaf())
        continue;
      auto Root = EM.getTopLevelParent();
      if (Operator::getOpcode(Root->front()) == Instruction::IntToPtr)
        continue;
      if (isa<CallBase>(Root->front()))
        continue;
      int64_t Offset;
      auto Base = GetPointerBaseWithConstantOffset(EM.front(), Offset, DL);
      auto CurrEM = &EM;
      while (isSameBase(DL, Base, Root->front()) && CurrEM != Root) {
        auto ParentEM = CurrEM->getParent();
        if (Offset != 0 || CurrEM->getSize() != ParentEM->getSize())
          RootOffsets.try_emplace(CurrEM->front(), Offset);
        CurrEM = ParentEM;
        auto *CurrTy = CurrEM->front()->getType();
        Base = GetPointerBaseWithConstantOffset(CurrEM->front(), Offset, DL);
      }
    }
  }
  return RootOffsets;
}

/// \brief Builds for a specified pointer to memory `V` appropriate debug-level
/// representation.
///
/// \param [out] Expr List of operands for DIExpression.
/// \param [out] IsTemplate False if accurate representation is build and
/// false otherwise (see DIMemoryLocation for details).
/// \return Pointer to appropriate variable or nullptr if building
/// is unsuccessful. The second value in a pair is a location of a variable
/// declaration in a source code (if known).
std::pair<DIVariable *, DILocation *> buildDIExpression(
    const DataLayout &DL, const DominatorTree &DT,
    const Value *V, SmallVectorImpl<uint64_t> &Expr, bool &IsTemplate) {
  auto *Ty = V->getType();
  // If type is not a pointer then `GetPointerBaseWithConstantOffset` can
  // not be called.
  if (!Ty || !Ty->isPtrOrPtrVectorTy())
    return std::make_pair(nullptr, nullptr);
  int64_t Offset = 0;
  const Value *Curr = V, *Base = nullptr;
  while (true) {
    int64_t CurrOffset;
    Base = GetPointerBaseWithConstantOffset(Curr, CurrOffset, DL);
    Offset += CurrOffset;
    if (Curr == Base) {
      Base = getUnderlyingObject(const_cast<Value *>(Curr), 1);
      if (Curr == Base)
        break;
      IsTemplate = true;
    }
    Curr = Base;
  };
  if (Offset > 0) {
      Expr.push_back(Offset);
      Expr.push_back(dwarf::DW_OP_plus_uconst);
  } else if (Offset < 0) {
    Expr.push_back(dwarf::DW_OP_minus);
    Expr.push_back(-Offset);
    Expr.push_back(dwarf::DW_OP_constu);
  }
  if (Offset != 0)
    Expr[Expr.size() - 2] *= 8;
  if (auto LI = dyn_cast<LoadInst>(Base)) {
    Expr.push_back(dwarf::DW_OP_deref);
    return buildDIExpression(DL, DT, LI->getPointerOperand(), Expr, IsTemplate);
  }
  SmallVector<DIMemoryLocation, 4> DILocs;
  MDSearch Status;
  auto Info = findMetadata(Base, DILocs, &DT, MDSearch::Any, &Status);
  auto DILoc = findMetadata(Base, DILocs, &DT);
  if (!DILoc || (DILoc->Expr && !DILoc->Expr->isValid()))
    return std::make_pair(nullptr, nullptr);
  if (Status != MDSearch::AddressOfVariable)
    Expr.push_back(dwarf::DW_OP_deref);
  if (DILoc->Expr)
    Expr.append(DILoc->Expr->elements_begin(), DILoc->Expr->elements_end());
  return std::make_pair(DILoc->Var, DILoc->Loc);
};
}

std::unique_ptr<DIMemory> CorruptedMemoryResolver::popFromCache(
    const Value *V, bool IsExec) {
  auto Itr = mCachedUnknownMemory.find({ V, IsExec });
  if (Itr == mCachedUnknownMemory.end())
    return nullptr;
  auto M = std::move(Itr->second);
  mCachedUnknownMemory.erase(Itr);
  return M;
}

void CorruptedMemoryResolver::resolve() {
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: determine safely promoted memory locations\n");
  findNoAliasFragments();
  LLVM_DEBUG(pomotedCandidatesLog());
  if (!mDIAT)
    return;
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: resolve corrupted memory locations\n");
  SpanningTreeRelation<AliasTree *> AliasSTR(mAT);
  for (auto *N : post_order(mDIAT)) {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: visit debug alias node\n");
    if (isa<DIAliasTopNode>(N))
      continue;
    determineCorruptedInsertionHint(cast<DIAliasMemoryNode>(*N), AliasSTR);
  }
  // Now all corrupted items will be forward to the root of theirs graph.
  // Each graph consists of connected items.
  for (auto &Root : mCorrupted) {
    if (Root->getForward())
      continue;
#ifdef LLVM_DEBUG
      CorruptedPool::size_type GraphSize = 0;
#endif
      for (auto *Item : depth_first(Root.get())) {
        LLVM_DEBUG(++GraphSize);
        Item->setForward(Root.get());
      }
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: forward corrupted graph of size "
        << GraphSize << "\n");
  }
}

void CorruptedMemoryResolver::findNoAliasFragments() {
  for (auto &I : make_range(inst_begin(mFunc), inst_end(mFunc))) {
    // We ignore 'undef' values, so if there are no other registers associated
    // with this variable, then the variable becomes corrupted and may be
    // considered as redundant.
    if (!isa<DbgValueInst>(I) ||
        isa<UndefValue>(cast<DbgValueInst>(I).getValue()))
      continue;
    auto Loc = DIMemoryLocation::get(&I);
    if (mSmallestFragments.count(Loc))
      continue;
    // Ignore expression if it represent address of a variable instead of
    // a value: call void @llvm.dbg.value(
    //    metadata i32* %X, metadata !20, metadata !DIExpression(DW_OP_deref)).
    // %X stores an address of a variable !20, so there is no insurance that
    // !20 is not overlapped with some other memory locations.
    // Such expressions may be created after some transform passes when
    // llvm.dbg.declare is replaced with llvm.dbg.value.
    if (hasDeref(*cast<DbgValueInst>(I).getExpression())) {
      auto VarFragments = mVarToFragments.find(Loc.Var);
      if (VarFragments == mVarToFragments.end())
        continue;
      for (auto *EraseExpr : VarFragments->get<DIExpression>())
        mSmallestFragments.erase(DIMemoryLocation::get(Loc.Var, EraseExpr));
      VarFragments->get<DIExpression>().clear();
      continue;
    }
    if (auto *V = MetadataAsValue::getIfExists(I.getContext(), Loc.Var)) {
      // If llvm.dbg.declare exists it means that alloca has not been fully
      // promoted to registers, so we can not analyze it here due to
      // this pass ignores alias analysis results.
      //
      /// TODO (kaniandr@gmail.com): It is planed to deprecate
      /// llvm.dbg.declare intrinsic in the future releases. This check
      /// should be changed if this occurs.
      bool HasDbgDeclare = false;
      for (User *U : V->users())
        if (HasDbgDeclare =
              (isa<DbgDeclareInst>(U) &&
              !isa<UndefValue>(cast<DbgDeclareInst>(U)->getAddress())))
          break;
      if (HasDbgDeclare)
        continue;
    }
    auto VarFragments = mVarToFragments.try_emplace(Loc.Var, Loc.Loc, Loc.Expr);
    if (VarFragments.second) {
      mSmallestFragments.try_emplace(std::move(Loc), nullptr);
      continue;
    }
    // Empty list of fragments means that this variable should be ignore due
    // to some reason, for example, existence of llvm.dbg.declare.
    // The reason has been discovered on the previous iterations.
    if (VarFragments.first->get<DIExpression>().empty())
        continue;
    if ([this, &Loc, &VarFragments]() {
      for (auto *Expr : VarFragments.first->get<DIExpression>()) {
        if (mayAliasFragments(*Expr, *Loc.Expr)) {
          for (auto *EraseExpr : VarFragments.first->get<DIExpression>())
            mSmallestFragments.erase(DIMemoryLocation::get(Loc.Var, EraseExpr));
          VarFragments.first->get<DIExpression>().clear();
          return true;
        }
      }
      return false;
    }())
      continue;
    VarFragments.first->get<DIExpression>().push_back(Loc.Expr);
    mSmallestFragments.try_emplace(std::move(Loc), nullptr);
  }
}

void CorruptedMemoryResolver::determineCorruptedInsertionHint(DIAliasMemoryNode &N,
    const SpanningTreeRelation<AliasTree *> &AliasSTR) {
  collapseChildStack(N, AliasSTR);
  auto &Info = mChildStack.back();
  updateWorkLists(N, Info);
  if (Info.CorruptedWL.empty()) {
    LLVM_DEBUG(dbgs() <<
      "[DI ALIAS TREE]: go through node without corrupted memory\n");
    return;
  }
  if (!Info.PromotedWL.empty())
    promotedBasedHint(Info);
  else if (!Info.NodesWL.empty())
    aliasTreeBasedHint(AliasSTR, Info);
  else if (N.getParent() == mDIAT->getTopLevelNode())
    distinctUnknownHint(Info);
}

void CorruptedMemoryResolver::collapseChildStack(const DIAliasNode &Parent,
    const SpanningTreeRelation<AliasTree *> &AliasSTR) {
  NodeInfo Info;
  if (Parent.child_empty()) {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: push node information to stack\n");
    LLVM_DEBUG(Info.sizeLog());
    mChildStack.push_back(std::move(Info));
    return;
  }
  auto copyCPN = [](NodeInfo &From, NodeInfo &To) {
    To.CorruptedWL.append(From.CorruptedWL.begin(), From.CorruptedWL.end());
    To.PromotedWL.append(From.PromotedWL.begin(), From.PromotedWL.end());
    To.NodesWL.insert(From.NodesWL.begin(), From.NodesWL.end());
  };
  Info.ParentOfUnknown = mChildStack.back().ParentOfUnknown;
  copyCPN(mChildStack.back(), Info);
  mChildStack.pop_back();
  size_t ChildIdx = 1, ChildSize = Parent.child_size();
  for (; ChildIdx < ChildSize; ++ChildIdx) {
    auto &CI = mChildStack.back();
    copyCPN(CI, Info);
    if (!CI.ParentOfUnknown) {
      Info.Items.append(CI.Items.begin(), CI.Items.end());
    } else if (!Info.ParentOfUnknown) {
      Info.ParentOfUnknown = CI.ParentOfUnknown;
      Info.Items.append(CI.Items.begin(), CI.Items.end());
      continue;
    } else {
      switch (AliasSTR.compare(Info.ParentOfUnknown, CI.ParentOfUnknown)) {
      case TreeRelation::TR_DESCENDANT:
        Info.ParentOfUnknown = CI.ParentOfUnknown;
        Info.Items.clear();
      case TreeRelation::TR_EQUAL:
        Info.Items.append(CI.Items.begin(), CI.Items.end());
      }
    }
    mChildStack.pop_back();
  }
  if (ChildIdx == ChildSize) {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: push node information to stack\n");
    LLVM_DEBUG(Info.sizeLog());
    mChildStack.push_back(std::move(Info));
    return;
  }
  Info.Items.clear();
  SmallVector<AliasNode *, 4> Nodes;
  for (; ChildIdx < ChildSize; ++ChildIdx) {
    auto &CI = mChildStack.back();
    copyCPN(CI, Info);
    Nodes.push_back(CI.ParentOfUnknown);
    mChildStack.pop_back();
  }
  using NodeItr = bcl::IteratorDataAdaptor<
    SmallVectorImpl<AliasNode *>::iterator,
    AliasTree *, GraphTraits<Inverse<AliasTree *>>::NodeRef>;
  Info.ParentOfUnknown = *findLCA(AliasSTR,
    NodeItr{ Nodes.begin(), mAT }, NodeItr{ Nodes.end(), mAT });
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: push node information to stack\n");
  LLVM_DEBUG(Info.sizeLog());
  mChildStack.push_back(std::move(Info));
}

void CorruptedMemoryResolver::promotedBasedHint(NodeInfo &Info) {
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: determine promoted based hint\n");
  CorruptedMemoryItem *Item;
  if (!Info.ParentOfUnknown && Info.NodesWL.empty() &&
      Info.PromotedWL.size() == 1 &&
      Info.PromotedWL.front().Expr->getNumElements() > 0 ) {
    auto Pair = mFragChildOfUnknown.try_emplace(Info.PromotedWL.front());
    assert(Pair.second && "Hint must not be inserted yet!");
    Item = Pair.first->second = newCorrupted();
    copyToCorrupted(Info.CorruptedWL, Item);
    Info.ParentOfUnknown = mAT->getTopLevelNode();
  } else {
    if (!Info.Items.empty() &&
        Info.ParentOfUnknown == mAT->getTopLevelNode()) {
      mergeRange(Info.Items.begin(), Info.Items.end());
      merge(*Info.Items.begin(), Info.PromotedWL.front().Var);
      copyToCorrupted(Info.CorruptedWL, Item = *Info.Items.begin());
    } else {
      Info.ParentOfUnknown = mAT->getTopLevelNode();
      auto Pair = mVarChildOfUnknown.try_emplace(
        Info.PromotedWL.front().Var);
      if (Pair.second)
        Pair.first->second = newCorrupted();
      copyToCorrupted(Info.CorruptedWL, Item = Pair.first->second);
    }
    for (auto I = Info.PromotedWL.begin() + 1, E = Info.PromotedWL.end();
         I != E; ++I)
      merge(Item, I->Var);
    for (auto *Node : Info.NodesWL)
      mergeChild(Item, Info.ParentOfUnknown, Node);
  }
  Info.CorruptedWL.clear();
  Info.Items.clear();
  Info.Items.push_back(Item);
}

void CorruptedMemoryResolver::aliasTreeBasedHint(
    const SpanningTreeRelation<AliasTree *> &AliasSTR, NodeInfo &Info) {
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: determine alias tree based hint\n");
  auto PrevParentOfUnknown = Info.ParentOfUnknown;
  if (Info.ParentOfUnknown != mAT->getTopLevelNode()) {
    using NodeItr = bcl::IteratorDataAdaptor<
      SmallPtrSetImpl<AliasNode *>::iterator,
      AliasTree *, GraphTraits<Inverse<AliasTree *>>::NodeRef>;
    AliasNode *LCA = Info.NodesWL.count(mAT->getTopLevelNode()) ?
      mAT->getTopLevelNode() :
      *findLCA(AliasSTR,
        NodeItr{ Info.NodesWL.begin(), mAT },
        NodeItr{ Info.NodesWL.end(), mAT });
    if (Info.ParentOfUnknown) {
      switch (AliasSTR.compare(Info.ParentOfUnknown, LCA)) {
      default:
      {
        using NodeItr = bcl::IteratorDataAdaptor<
          std::array<AliasNode *, 2>::iterator,
          AliasTree *, GraphTraits<Inverse<AliasTree *>>::NodeRef>;
        std::array<AliasNode *, 2> NodePair{ LCA, Info.ParentOfUnknown };
        Info.ParentOfUnknown = *findLCA(AliasSTR,
          NodeItr{ NodePair.begin(), mAT }, NodeItr{ NodePair.end(), mAT });
        break;
      }
      case TreeRelation::TR_DESCENDANT: Info.ParentOfUnknown = LCA; break;
      case TreeRelation::TR_EQUAL: case TreeRelation::TR_ANCESTOR: break;
      }
    } else {
      Info.ParentOfUnknown = LCA;
    }
  }
  assert(Info.ParentOfUnknown && "Parent of unknown must not be null!");
  CorruptedMemoryItem *Item;
  if (!Info.Items.empty() && Info.ParentOfUnknown == PrevParentOfUnknown) {
    mergeRange(Info.Items.begin(), Info.Items.end());
    copyToCorrupted(Info.CorruptedWL, Item = *Info.Items.begin());
  } else {
    auto P = *Info.NodesWL.begin();
    if (P != mAT->getTopLevelNode())
      while(P->getParent(*mAT) != Info.ParentOfUnknown)
        P = P->getParent(*mAT);
    auto Pair = mChildOfUnknown.try_emplace(P);
    if (Pair.second)
      Pair.first->second = newCorrupted();
    copyToCorrupted(Info.CorruptedWL, Item = Pair.first->second);
  }
  auto NodeItr = Info.NodesWL.begin(), NodeItrE = Info.NodesWL.end();
  for (++NodeItr; NodeItr != NodeItrE; ++NodeItr)
    mergeChild(Item, Info.ParentOfUnknown, *NodeItr);
  Info.CorruptedWL.clear();
  Info.Items.clear();
  Info.Items.push_back(Item);
}

void CorruptedMemoryResolver::distinctUnknownHint(NodeInfo &Info) {
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: determine distinct unknown hint\n");
  CorruptedMemoryItem *Item;
  if (!Info.Items.empty() &&
      Info.ParentOfUnknown == mAT->getTopLevelNode()) {
    mergeRange(Info.Items.begin(), Info.Items.end());
    copyToCorrupted(Info.CorruptedWL, Item = *Info.Items.begin());
  } else {
    Info.ParentOfUnknown = mAT->getTopLevelNode();
    copyToCorrupted(Info.CorruptedWL, Item = newCorrupted());
    mDistinctUnknown.push_back(Item);
  }
  Info.CorruptedWL.clear();
  Info.Items.clear();
  Info.Items.push_back(Item);
}

void CorruptedMemoryResolver::copyToCorrupted(
    const SmallVectorImpl<DIMemory *> &WL, CorruptedMemoryItem *Item) {
  assert(Item && "List of corrupted memory must not be null!");
  for (auto *M : WL) {
    ++NumCorruptedMemory;
    auto NewM = DIMemory::get(mFunc->getContext(), M->getEnv(), *M);
    NewM->setProperties(DIMemory::Original);
    for (auto &VH : *M) {
      if (!VH || isa<UndefValue>(VH))
        continue;
      NewM->bindValue(VH);
    }
    M->replaceAllUsesWith(NewM.get());
    if (auto DIEM = dyn_cast<DIEstimateMemory>(NewM.get()))
      if (!DIEM->isTemplate() && DIEM->getExpression()->getNumElements() == 0) {
        // Now, we process a corrupted location which is a root of subtree
        // that contains promoted locations (fragments). We specify unknown node
        // which must contain root of subtree.
        auto Info = mVarChildOfUnknown.try_emplace(DIEM->getVariable(), Item);
        // If Info.first->second is null it means that it is under construction
        // at this moment. Note, that Item is constructing now, so it replace
        // null later.
        if (!Info.second && Info.first->second)
          merge(Info.first->second, Item);
      }
    Item->push(std::move(NewM));
  }
}

void CorruptedMemoryResolver::merge(
    CorruptedMemoryItem *LHS, CorruptedMemoryItem *RHS) {
  assert(LHS && RHS && "Merged items must not be null!");
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: merge two corrupted lists\n");
  if (LHS == RHS)
    return;
  LHS->addSuccessor(RHS);
  RHS->addPredecessor(LHS);
}

void CorruptedMemoryResolver::updateWorkLists(
  DIAliasMemoryNode &N, NodeInfo &Info) {
  llvm::SmallVector<std::pair<DIMemory *, DIMemory *>, 16> Replacement;
  Replacement.reserve(N.size());
  for (auto &M : N) {
    LLVM_DEBUG(checkLog(M));
    auto Binding = M.getBinding();
    if (auto *DIEM = dyn_cast<DIEstimateMemory>(&M)) {
      auto Loc{DIMemoryLocation::get(DIEM->getVariable(), DIEM->getExpression(),
                                     nullptr, DIEM->isTemplate(),
                                     DIEM->isAfterPointer())};
      auto FragmentItr = mSmallestFragments.find(Loc);
      if (FragmentItr != mSmallestFragments.end()) {
        if (Binding == DIMemory::Destroyed ||
          Binding == DIMemory::Empty) {
          Info.PromotedWL.push_back(Loc);
          FragmentItr->second = DIEM;
          continue;
        } else {
          //LLVM_DEBUG(corruptedFoundLog(M));
          LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: safely promoted candidate is discarded\n");
          Replacement.emplace_back(&M, nullptr);
          // This is rare case, so we do not take care about overheads and
          // remove an expression from a vector.
          auto VToF = mVarToFragments.find(Loc.Var);
          auto ExprItr = std::find(
            VToF->get<DIExpression>().begin(),
            VToF->get<DIExpression>().end(), Loc.Expr);
          VToF->get<DIExpression>().erase(ExprItr);
          mSmallestFragments.erase(Loc);
        }
      } else if (Binding != DIMemory::Consistent) {
        Replacement.emplace_back(&M, nullptr);
      } else {
        isSameAfterRebuildEstimate(*DIEM, Replacement);
      }
    } else if (Binding != DIMemory::Consistent) {
      Replacement.emplace_back(&M, nullptr);
    } else {
      isSameAfterRebuild(cast<DIUnknownMemory>(M), Replacement);
    }
    if (!findBoundAliasNodes(M, *mAT, Info.NodesWL) &&
        isa<DIEstimateMemory>(M) && Binding != DIMemory::Empty)
      findLowerBoundAliasNodes(cast<DIEstimateMemory>(M), *mAT, Info.NodesWL);
  }
  replaceAllMemoryUses(Replacement, Info);
}

void CorruptedMemoryResolver::replaceAllMemoryUses(
    llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement,
    NodeInfo &Info) {
  DenseMap<MDNode*, std::size_t> MemoryToNode;
  std::vector<ReplacementNode> NodePool;
  NodePool.reserve(Replacement.size());
  for (auto &FromTo : Replacement) {
    auto Info = MemoryToNode.try_emplace(FromTo.first->getAsMDNode());
    ReplacementNode *Node = nullptr;
    if (Info.second) {
      Info.first->second = NodePool.size();
      NodePool.emplace_back(getLanguage(*mFunc));
    }
    Node = &NodePool[Info.first->second];
    Node->setSuccessorFrom(FromTo.first);
    if (!FromTo.second)
      continue;
    Info = MemoryToNode.try_emplace(FromTo.second->getAsMDNode());
    if (Info.second) {
      Info.first->second = NodePool.size();
      NodePool.emplace_back(getLanguage(*mFunc));
      Node = &NodePool.back();
      Node->setPredecessorTo(FromTo.second);
    } else {
      Node = &NodePool[Info.first->second];
      if (Node->getPredecessorTo()) {
        if (Node->getPredecessorTo() != FromTo.second)
          for (auto &VH : *FromTo.second)
            Node->getPredecessorTo()->bindValue(VH);
      } else {
        Node->setPredecessorTo(FromTo.second);
      }
    }
  }
  // Insert entry node which is necessary for search of SCCs.
  NodePool.emplace_back(getLanguage(*mFunc));
  auto *Root = &NodePool.back();
  // WARNING: do not insert new nodes after construction of edges, because
  // insertion of a new element inside a std::vector may lead to memory
  // reallocation. In this case pointers to nodes are going to be changed.
  // So, edges become invalid.
  for (auto &FromTo : Replacement) {
    if (!FromTo.second)
      continue;
    auto *PredNode = &NodePool[MemoryToNode[FromTo.first->getAsMDNode()]];
    auto *SuccNode = &NodePool[MemoryToNode[FromTo.second->getAsMDNode()]];
    PredNode->addSuccessor(SuccNode);
    SuccNode->addPredecessor(PredNode);
  }
  for (auto I = NodePool.begin(), EI = NodePool.end() - 1; I != EI; ++I)
    if (I->numberOfPredecessors() == 0 ||
        I->numberOfSuccessors() == 1 && I->isSelfReplacement()) {
      I->addPredecessor(Root);
      Root->addSuccessor(&*I);
    }
  LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: size of replacement graph "
                    << NodePool.size() << "\n");
  // Now, we process all subgraphs of replacement tree and perform replacement
  // in down-top order (or mark nodes in a whole subtree as corrupted).
  bool IsCorrupted = false;
  for (scc_iterator<ReplacementNode *> I = scc_begin(Root); !I.isAtEnd(); ++I) {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: size of SCC in replacement subgraph "
                      << I->size() << "\n");
    // Check whether all subtrees have been processed.
    if (I->front()->isRoot())
      break;
    // Reset IsCorrupted flag if we start to process a new replacement subgraph
    // without a corrupted leaf an SCC with multiple nodes.
    if (I->size() == 1 &&
        (I->front()->isSelfReplacement() ||
         I->front()->numberOfSuccessors() == 0 && !I->front()->isCorrupted()))
      IsCorrupted = false;
    if (IsCorrupted || I->size() > 1 || I->front()->isCorrupted()) {
      IsCorrupted = true;
      for (auto *N : *I) {
        auto *M = N->getSuccessorFrom();
        assert(M &&
               "Corrupted memory from previous alias tree must not be null!");
        LLVM_DEBUG(corruptedFoundLog(*M));
        LLVM_DEBUG(if (isa<DIUnknownMemory>(M)) dbgs()
                   << "[DI ALIAS TREE]: unknown corrupted is found\n");
        Info.CorruptedWL.push_back(M);
        auto Binding = M->getBinding();
        auto Pair = mCorruptedSet.insert({
          N->getSuccessorFrom()->getAsMDNode(),
          Binding == DIMemory::Empty ||
            Binding == DIMemory::Destroyed ? false : true
        });
        mCorruptedSet.insert({ M->getBaseAsMDNode(), (*Pair.first).getInt() });
      }
    } else {
      if (I->front()->numberOfSuccessors() == 0)
        continue;
      auto *ToReplace = *I->front()->succ_begin();
      auto *NewM = ToReplace->getPredecessorTo();
      if (ToReplace->isActiveReplacement())
        NewM->setProperties(DIMemory::Merged);
      ToReplace->activateReplacement();
      I->front()->getSuccessorFrom()->replaceAllUsesWith(NewM);
    }
  }
}

bool CorruptedMemoryResolver::isSameAfterRebuildEstimate(DIMemory &M,
  llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement) {
  assert(M.getBinding() == DIMemory::Consistent &&
    "Inconsistent memory is always corrupted and can not be the same after rebuild!");
  assert(isa<DIEstimateMemory>(M) ||
    isa<DIUnknownMemory>(M) && !cast<DIUnknownMemory>(M).isExec() &&
    "Bound memory location must be estimate!");
  Replacement.emplace_back(&M, nullptr);
  DIMemory *RAUWd = nullptr;
  DIMemoryCache LocalCache;
  auto Size = isa<DIEstimateMemory>(M) ?
    cast<DIEstimateMemory>(M).getSize() : LocationSize::precise(0);
  for (auto &VH : M) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    auto EM = mAT->find(MemoryLocation(VH, Size));
    // Memory becomes unused after transformation and is not presented in alias
    // tree.
    if (!EM)
      return false;
    std::pair<DIMemoryCache::iterator, bool> Cached;
    if (isa<DIEstimateMemory>(M)) {
      // If size of IR-level location is larger then size of metadata-level
      // location then original metadata-level location will not be added
      // to metadata alias tree. However, it is safe do not consider it as a
      // corrupted memory because there is other location differs only in
      // size argument. Such case is a result of instcombine pass.
      // Originally X[0][1] is represented with 2 GEPs. However, after instcombine
      // there is a single GEP with multiple operands and <X[0], size of dim>
      // will not be constructed. So, we ignore this case if <X[0], array size>
      // exist.
      if (!EM->getSize().hasValue() && Size.hasValue() ||
           EM->getSize().hasValue() && Size.hasValue() &&
             EM->getSize().getValue() > Size.getValue())
        Cached = LocalCache.try_emplace(EM);
      else if (EM->getSize() != Size)
        return false;
      else
        Cached = mCachedMemory.try_emplace(EM);
    } else {
      // Unknown node does not contain size, so use the largest possible size.
      using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
      while (auto Next = CT::getNext(EM))
        EM = Next;
      Cached = mCachedMemory.try_emplace(EM);
      Size = EM->getSize();
    }
    if (Cached.second) {
      Cached.first->second = tsar::buildDIMemoryWithNewSize(
        *EM, Size, mFunc->getContext(), M.getEnv(), *mDL, *mDT);
      LLVM_DEBUG(buildMemoryLog(
        mDIAT->getFunction(), *mDT, *Cached.first->second, *EM));
    }
    assert(Cached.first->second || "Debug memory location must not be null!");
    LLVM_DEBUG(afterRebuildLog(*Cached.first->second));
    if (Cached.first->second->getBaseAsMDNode() != M.getBaseAsMDNode())
      return false;
    // Different estimate memory locations produce the same debug-level memory.
    // For example, P = ...; ... P = ...; ... .
    if (RAUWd && RAUWd != Cached.first->second.get())
      return false;
    RAUWd = Cached.first->second.get();
  }
  assert(RAUWd && "Must not be null for consistent memory location!");
  // It may be unsafe to replace all uses of a memory location M with a new one.
  // If raw representation of replacer has been presented in previous alias
  // tree and it was located in a node differs from a node which owns a location
  // M. For example, existed raw representation may be corrupted.
  // TODO (kaniandr@gmail.com): is it possible to relax this condition, for
  // example, whether the knowledge of alias tree traversal is important here.
  if (RAUWd->getAsMDNode() != M.getAsMDNode()) {
    auto PrevDIMItr = mDIAT->find(*RAUWd->getAsMDNode());
    if (PrevDIMItr != mDIAT->memory_end() &&
        PrevDIMItr->getAliasNode() != M.getAliasNode()) {
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: replacement has been located in a "
                           "distinct node in a previous tree\n");
      return false;
    }
  }
  // The new alias tree will not contain RAUWd (see discussion above).
  // This means that a new representation will be deleted soon and appropriate
  // handles will be invoked. So, remove it from replacement and replace all
  // uses (the new memory will be deleted on exist from this function, so
  // corresponding handles will be also invoked for replacement).
  if (!LocalCache.empty()) {
    LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: ignore covered original location\n");
    Replacement.pop_back();
    M.replaceAllUsesWith(RAUWd);
    return true;
  }
  Replacement.back().second = RAUWd;
  return true;
}

bool CorruptedMemoryResolver::isSameAfterRebuildUnknown(DIUnknownMemory &M,
    llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement) {
  assert(M.getBinding() == DIMemory::Consistent &&
    "Inconsistent memory is always corrupted and can not be the same after rebuild!");
  assert(M.isExec() && "Bound memory location must be unknown!");
  Replacement.emplace_back(&M, nullptr);
  if (M.isDistinct())
    return false;
  DIMemory *RAUWd = nullptr;
  for (auto &VH : M) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    auto Cached = mCachedUnknownMemory.try_emplace({ VH, M.isExec() });
    if (Cached.second) {
      Cached.first->second = tsar::buildDIMemory(*VH, mFunc->getContext(),
        M.getEnv(), *mDT, M.getProperies(), M.getFlags());
      LLVM_DEBUG(buildMemoryLog(mDIAT->getFunction(), *Cached.first->second, *VH));
    }
    assert(Cached.first->second || "Debug memory location must not be null!");
    LLVM_DEBUG(afterRebuildLog(*Cached.first->second));
    if (Cached.first->second->getAsMDNode() != M.getAsMDNode())
      return false;
    // Different memory locations produce the same debug-level memory.
    if (RAUWd && RAUWd != Cached.first->second.get())
      return false;
    RAUWd = Cached.first->second.get();
  }
  assert(RAUWd && "Must not be null for consistent memory location!");
  Replacement.back().second = RAUWd;
  return true;
}

namespace tsar {
Optional<DIMemoryLocation> buildDIMemory(const MemoryLocation &Loc,
    LLVMContext &Ctx, const DataLayout &DL, const DominatorTree &DT) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  SmallVector<uint64_t, 8> ReverseExpr;
  bool IsTemplate = false;
  auto DIInfo = buildDIExpression(DL, DT, Loc.Ptr, ReverseExpr, IsTemplate);
  if (!DIInfo.first)
    return None;
  SmallVector<uint64_t, 8> Expr(ReverseExpr.rbegin(), ReverseExpr.rend());
  if (Expr.empty()) {
    auto DIE = DIExpression::get(Ctx, Expr);
    auto DIL{DIMemoryLocation::get(DIInfo.first, DIE, DIInfo.second, IsTemplate,
                                   !Loc.Size.mayBeBeforePointer())};
    // If expression is empty and  size can be obtained from a variable than
    // DW_OP_LLVM_fragment should not be added. If variable will be promoted it
    // will be represented without this size. So there will be different
    // locations for the same memory before and after promotion.
    // Let us consider some examples:
    // struct P { char X;};
    //   struct P P1; P1.X => {P1, DIExpression()}
    // struct S { char Y; struct P Z;};
    //   sturct S S1; S1.Z.X => {S1, DIExpression(DW_OP_LLVM_fragment, 8, 8)}
    // struct Q { struct P Z;};
    //   struct Q Q1; Q1.Z.X => {Q1, DIEspression()}
    if (DIL.getSize() == Loc.Size)
      return DIL;
  }
  uint64_t LastDwarfOp = 0;
  for (size_t I = 0, E = Expr.size(); I < E; ++I) {
    LastDwarfOp = Expr[I];
    if (Expr[I] != dwarf::DW_OP_deref)
      ++I;
  }
  if (Loc.Size.isPrecise()) {
    if (Expr.empty () || LastDwarfOp == dwarf::DW_OP_deref ||
        LastDwarfOp == dwarf::DW_OP_minus || LastDwarfOp == dwarf::DW_OP_plus) {
      Expr.append({ dwarf::DW_OP_LLVM_fragment, 0, Loc.Size.getValue() * 8});
    } else {
      assert(LastDwarfOp == dwarf::DW_OP_plus_uconst && "Unknown DWARF operand!");
      Expr[Expr.size() - 2] = dwarf::DW_OP_LLVM_fragment;
      Expr.push_back(Loc.Size.getValue() * 8);
    }
  } else {
    if (!Expr.empty() && LastDwarfOp == dwarf::DW_OP_LLVM_fragment)
      Expr.back() = 0;
    else
      Expr.append({ dwarf::DW_OP_LLVM_fragment, 0, 0 });
  }
  auto DIE = DIExpression::get(Ctx, Expr);
  return DIMemoryLocation::get(DIInfo.first, DIE, DIInfo.second, IsTemplate,
                               !Loc.Size.mayBeBeforePointer());
}

llvm::MDNode * getRawDIMemoryIfExists(llvm::LLVMContext &Ctx,
    DIMemoryLocation DILoc) {
  auto F = DILoc.Template ?
    DIEstimateMemory::Template : DIEstimateMemory::NoFlags;
  if (DILoc.AfterPointer)
    F |= DIEstimateMemory::AfterPointer;
  SmallVector<DILocation *, 1> Dbgs;
  if (DILoc.Loc)
    Dbgs.push_back(DILoc.Loc);
  return DIEstimateMemory::getRawIfExists(Ctx, DILoc.Var, DILoc.Expr, F, Dbgs);
}

std::unique_ptr<DIMemory> buildDIMemory(const EstimateMemory &EM,
    LLVMContext &Ctx, DIMemoryEnvironment &Env,
    const DataLayout &DL, const DominatorTree &DT) {
  return buildDIMemoryWithNewSize(EM, EM.getSize(), Ctx, Env, DL, DT);
}

std::unique_ptr<DIMemory> buildDIMemoryWithNewSize(const EstimateMemory &EM,
  LocationSize Size, LLVMContext &Ctx, DIMemoryEnvironment &Env,
  const DataLayout &DL, const DominatorTree &DT) {
  auto DILoc = buildDIMemory(
    MemoryLocation(EM.front(), Size), Ctx, DL, DT);
  std::unique_ptr<DIMemory> DIM;
  auto VItr = EM.begin();
  auto Properties = EM.isExplicit() ? DIMemory::Explicit : DIMemory::NoProperty;
  if (!DILoc) {
    auto F = isa<CallBase>(*VItr) ? DIUnknownMemory::Result
                                  : DIUnknownMemory::Object;
    DIM =
      buildDIMemory(const_cast<Value &>(**VItr), Ctx, Env, DT, Properties, F);
    ++VItr;
  } else {
    auto Flags = DILoc->Template ?
      DIEstimateMemory::Template : DIEstimateMemory::NoFlags;
    if (DILoc->AfterPointer)
      Flags |= DIEstimateMemory::AfterPointer;
    SmallVector<DILocation *, 1> Dbgs;
    for (auto *V : EM)
      if (auto *I = dyn_cast_or_null<Instruction>(V))
        if (auto DbgLoc = I->getDebugLoc())
          Dbgs.push_back(DbgLoc.get());
    if (DILoc->Loc)
      Dbgs.push_back(DILoc->Loc);
    DIM = DIEstimateMemory::get(Ctx, Env, DILoc->Var, DILoc->Expr, Flags, Dbgs);
    DIM->setProperties(Properties);
  }
  for (auto EItr = EM.end(); VItr != EItr; ++VItr)
    DIM->bindValue(const_cast<Value *>(*VItr));
  return DIM;
}

llvm::MDNode * getRawDIMemoryIfExists(const EstimateMemory &EM,
  llvm::LLVMContext &Ctx, const llvm::DataLayout &DL,
  const llvm::DominatorTree &DT) {
  auto DILoc = buildDIMemory(
    MemoryLocation(EM.front(), EM.getSize()), Ctx, DL, DT);
  if (!DILoc) {
    auto F = isa<CallBase>(EM.front()) ? DIUnknownMemory::Result
      : DIUnknownMemory::Object;
    return getRawDIMemoryIfExists(const_cast<Value &>(*EM.front()), Ctx, DT, F);
  } else {
    auto Flags = DILoc->Template ?
      DIEstimateMemory::Template : DIEstimateMemory::NoFlags;
    if (DILoc->AfterPointer)
      Flags |= DIEstimateMemory::AfterPointer;
    SmallVector<DILocation *, 1> Dbgs;
    for (auto *V : EM)
      if (auto *I = dyn_cast_or_null<Instruction>(V))
        if (auto DbgLoc = I->getDebugLoc())
          Dbgs.push_back(DbgLoc.get());
    if (DILoc->Loc)
      Dbgs.push_back(DILoc->Loc);
    return DIEstimateMemory::getRawIfExists(
      Ctx, DILoc->Var, DILoc->Expr, Flags, Dbgs);
  }
}

std::unique_ptr<DIMemory> buildDIMemory(Value &V, LLVMContext &Ctx,
    DIMemoryEnvironment &Env, const DominatorTree &DT,
    DIMemory::Property P, DIUnknownMemory::Flags F) {
  MDNode *MD = nullptr;
  DILocation *Loc = nullptr;
  auto IntExpr = stripIntToPtr(&V);
  if (IntExpr != &V) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    if (auto ConstInt = dyn_cast<ConstantInt>(IntExpr)) {
      auto ConstV = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(Type::getInt64Ty(Ctx), ConstInt->getValue()));
      MD = MDNode::get(Ctx, { ConstV });
    } else if (auto DILoc = findMetadata(
        IntExpr, DILocs, &DT, MDSearch::ValueOfVariable)) {
      MD = DILoc->Var;
    } else if (isa<Instruction>(V)) {
      Loc = cast<Instruction>(V).getDebugLoc().get();
    }
  } else {
    auto *Call = dyn_cast<CallBase>(&V);
    auto Callee = !Call ? dyn_cast_or_null<Function>(&V)
      : dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
    if (Callee)
      MD = findMetadata(Callee);
    if (isa<Instruction>(V))
      Loc = cast<Instruction>(V).getDebugLoc().get();
  }
  auto DIM = DIUnknownMemory::get(Ctx, Env, MD, F, Loc);
  DIM->bindValue(&V);
  DIM->setProperties(P);
  return std::move(DIM);
}

llvm::MDNode * getRawDIMemoryIfExists(llvm::Value &V, llvm::LLVMContext &Ctx,
    const llvm::DominatorTree &DT, DIUnknownMemory::Flags F) {
  MDNode *MD = nullptr;
  DILocation *Loc = nullptr;
  auto IntExpr = stripIntToPtr(&V);
  if (IntExpr != &V) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    if (auto ConstInt = dyn_cast<ConstantInt>(IntExpr)) {
      auto ConstV = llvm::ConstantAsMetadata::getIfExists(
        llvm::ConstantInt::get(Type::getInt64Ty(Ctx), ConstInt->getValue()));
      return ConstV ? MDNode::getIfExists(Ctx, { ConstV }) : nullptr;
    } else if (auto DILoc = findMetadata(
      IntExpr, DILocs, &DT, MDSearch::ValueOfVariable)) {
      MD = DILoc->Var;
    } else if (isa<Instruction>(V)) {
      Loc = cast<Instruction>(V).getDebugLoc().get();
    }
  } else {
    auto *Call = dyn_cast<CallBase>(&V);
    auto Callee = !Call ? dyn_cast_or_null<Function>(&V)
      : dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
    if (Callee)
      MD = findMetadata(Callee);
    if (isa<Instruction>(V))
      Loc = cast<Instruction>(V).getDebugLoc().get();
  }
  return DIUnknownMemory::getRawIfExists(Ctx, MD, F, Loc);
}
}

namespace {
/// Storage for debug-level memory and alias trees environment.
class DIMemoryEnvironmentStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DIMemoryEnvironmentStorage() : ImmutablePass(ID) {
    initializeDIMemoryEnvironmentStoragePass(*PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    getAnalysis<DIMemoryEnvironmentWrapper>().set(mEnv);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DIMemoryEnvironmentWrapper>();
  }

  /// Returns debug-level memory environment.
  DIMemoryEnvironment & getEnv() noexcept { return mEnv; }

  /// Returns debug-level memory environment.
  const DIMemoryEnvironment & getEnv() const noexcept { return mEnv; }

private:
  DIMemoryEnvironment mEnv;
};
}

char DIMemoryEnvironmentStorage::ID = 0;
INITIALIZE_PASS_BEGIN(DIMemoryEnvironmentStorage, "di-mem-is",
  "Memory Estimator (Debug, Environment Immutable Storage)", true, true)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_END(DIMemoryEnvironmentStorage, "di-mem-is",
  "Memory Estimator (Debug, Environment Immutable Storage)", true, true)

template<> char DIMemoryEnvironmentWrapper::ID = 0;
INITIALIZE_PASS(DIMemoryEnvironmentWrapper, "di-mem-iw",
  "Memory Estimator (Debug, Environment Immutable Wrapper)", true, true)

char DIEstimateMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(DIEstimateMemoryPass, "di-estimate-mem",
  "Memory Estimator (Debug)", false, true)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_END(DIEstimateMemoryPass, "di-estimate-mem",
  "Memory Estimator (Debug)", false, true)

void DIEstimateMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createDIEstimateMemoryPass() {
  return new DIEstimateMemoryPass();
}

ImmutablePass * llvm::createDIMemoryEnvironmentStorage() {
  return new DIMemoryEnvironmentStorage();
}

bool DIEstimateMemoryPass::runOnFunction(Function &F) {
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &EnvWrapper = getAnalysis<DIMemoryEnvironmentWrapper>();
  mDIAliasTree = nullptr;
  if (!EnvWrapper)
    return false;
  auto &Env = *EnvWrapper;
  auto &DL = F.getParent()->getDataLayout();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto NewDIAT = std::make_unique<DIAliasTree>(F);
  {
    // This scope is necessary to drop of memory asserting handles in CMR
    // before previous alias tree destruction.
    CorruptedMemoryResolver CMR(F, &DL, &DT, Env[F], &AT);
    CMR.resolve();
    CorruptedMap CorruptedNodes;
    LLVM_DEBUG(dbgs() <<
      "[DI ALIAS TREE]: add distinct unknown nodes for corrupted memory\n");
    for (size_t Idx = 0, IdxE = CMR.distinctUnknownNum(); Idx < IdxE; ++Idx)
      addCorruptedNode(*NewDIAT, CMR.distinctUnknown(Idx),
        NewDIAT->getTopLevelNode(), CorruptedNodes);
    for (auto &VToF : CMR.getFragments()) {
      if (VToF.get<DIExpression>().empty())
        continue;
      DIAliasTreeBuilder Builder(*NewDIAT, F.getContext(), Env,
        CMR, CorruptedNodes, VToF.get<DIVariable>(), VToF.get<DILocation>(),
        VToF.get<DIExpression>());
      Builder.buildSubtree();
    }
    auto RootOffsets = findLocationToInsert(AT, DL);
    LLVM_DEBUG(dbgs() <<
      "[DI ALIAS TREE]: use an existing alias tree to add new nodes\n");
    LLVM_DEBUG(constantOffsetLog(RootOffsets.begin(), RootOffsets.end(), DT));
    buildDIAliasTree(DL, DT, Env, RootOffsets, CMR, *NewDIAT,
      *AT.getTopLevelNode(), *NewDIAT->getTopLevelNode(), CorruptedNodes);
  }
  std::vector<Metadata *> MemoryNodes(NewDIAT->memory_size());
  std::transform(NewDIAT->memory_begin(), NewDIAT->memory_end(),
    MemoryNodes.begin(), [](DIMemory &EM) { return EM.getAsMDNode(); });
  auto AliasTreeMDKind = F.getContext().getMDKindID("alias.tree");
  auto MD = MDNode::get(F.getContext(), MemoryNodes);
  F.setMetadata(AliasTreeMDKind, MD);
  mDIAliasTree = Env.reset(F, std::move(NewDIAT));
  return false;
}

// Pin the vtable to this file.
void CallbackDIMemoryHandle::anchor() {}
