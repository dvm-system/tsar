//===- DIEstimateMemory.cpp - Memory Hierarchy (Debug) ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file proposes functionality to construct a program alias tree.
//
//===----------------------------------------------------------------------===//

#include "DIEstimateMemory.h"
#include "DIMemoryEnvironment.h"
#include "DIMemoryHandle.h"
#include "CorruptedMemory.h"
#include "tsar_dbg_output.h"
#include "EstimateMemory.h"
#include "SpanningTreeRelation.h"
#include <bcl/IteratorDataAdaptor.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/PointerSumType.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/CallSite.h>
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
bool mayAliasFragments(const DIExpression &LHS, const DIExpression &RHS) {
  if (LHS.getNumElements() != 3 || RHS.getNumElements() != 3)
    return true;
  auto LHSFragment = LHS.getFragmentInfo();
  auto RHSFragment = RHS.getFragmentInfo();
  if (!LHSFragment || !RHSFragment)
    return true;
  if (LHSFragment->SizeInBits == 0 || RHSFragment->SizeInBits == 0)
    return false;
  return ((LHSFragment->OffsetInBits == RHSFragment->OffsetInBits) ||
          (LHSFragment->OffsetInBits < RHSFragment->OffsetInBits &&
          LHSFragment->OffsetInBits + LHSFragment->SizeInBits >
            RHSFragment->OffsetInBits) ||
          (RHSFragment->OffsetInBits < LHSFragment->OffsetInBits &&
          RHSFragment->OffsetInBits + RHSFragment->SizeInBits >
            LHSFragment->OffsetInBits));
}

void findBoundAliasNodes(const DIEstimateMemory &DIEM, AliasTree &AT,
    SmallPtrSetImpl<AliasNode *> &Nodes) {
  for (auto &VH : DIEM) {
    if (!VH || isa<llvm::UndefValue>(VH))
      continue;
    auto EM = AT.find(MemoryLocation(VH, DIEM.getSize()));
    // Memory becomes unused after transformation and does not presented in
    // the alias tree.
    if (!EM)
      continue;
    Nodes.insert(EM->getAliasNode(AT));
  }
}

void findBoundAliasNodes(const DIUnknownMemory &DIUM, AliasTree &AT,
    SmallPtrSetImpl<AliasNode *> &Nodes) {
  for (auto &VH : DIUM) {
    if (!VH || isa<UndefValue>(*VH))
      continue;
    AliasNode *N = nullptr;
    if (auto Inst = dyn_cast<Instruction>(VH))
      if (auto *N = AT.findUnknown(cast<Instruction>(*VH))) {
        Nodes.insert(N);
        return;
      }
    auto EM = AT.find(MemoryLocation(VH, 0));
    // Memory becomes unused after transformation and does not presented in
    // the alias tree.
    if (!EM)
      continue;
    using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
    do {
      Nodes.insert(EM->getAliasNode(AT));
    } while (EM = CT::getNext(EM));
  }
}

void findBoundAliasNodes(const DIMemory &DIM, AliasTree &AT,
    SmallPtrSetImpl<AliasNode *> &Nodes) {
  if (auto *EM = dyn_cast<DIEstimateMemory>(&DIM))
    findBoundAliasNodes(*EM, AT, Nodes);
  else
    findBoundAliasNodes(cast<DIUnknownMemory>(DIM), AT, Nodes);
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

std::unique_ptr<DIUnknownMemory> DIUnknownMemory::get(llvm::LLVMContext &Ctx,
    DIMemoryEnvironment &Env, MDNode *MD, DILocation *Loc, Flags F) {
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  auto NewMD = Loc ? llvm::MDNode::get(Ctx, { MD, FlagMD, Loc }) :
    llvm::MDNode::get(Ctx, { MD, FlagMD });
  assert(NewMD && "Can not create metadata node!");
  if (!MD)
    NewMD->replaceOperandWith(0, NewMD);
  ++NumUnknownMemory;
  return std::unique_ptr<DIUnknownMemory>(new DIUnknownMemory(Env, NewMD));
}

llvm::DebugLoc DIUnknownMemory::getDebugLoc() const {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Loc = llvm::dyn_cast<llvm::DILocation>(MD->getOperand(I)))
      return Loc;
  return DebugLoc();
}

std::unique_ptr<DIEstimateMemory> DIEstimateMemory::get(
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  auto MD = llvm::MDNode::get(Ctx, { Var, Expr, FlagMD });
  assert(MD && "Can not create metadata node!");
  ++NumEstimateMemory;
  return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(Env, MD));
}

std::unique_ptr<DIEstimateMemory>
DIEstimateMemory::getIfExists(
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  if (auto MD = llvm::MDNode::getIfExists(Ctx, { Var, Expr, FlagMD })) {
    ++NumEstimateMemory;
    return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(Env, MD));
  }
  return nullptr;
}

llvm::MDNode * DIEstimateMemory::getRawIfExists(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::getIfExists(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  if (!FlagMD)
    return nullptr;
  return llvm::MDNode::getIfExists(Ctx, { Var, Expr, FlagMD });
}

llvm::DIVariable * DIEstimateMemory::getVariable() {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Var = llvm::dyn_cast<llvm::DIVariable>(MD->getOperand(I)))
      return Var;
  llvm_unreachable("Variable must be specified!");
  return nullptr;
}

const llvm::DIVariable * DIEstimateMemory::getVariable() const {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Var = llvm::dyn_cast<llvm::DIVariable>(MD->getOperand(I)))
      return Var;
  llvm_unreachable("Variable must be specified!");
  return nullptr;
}

llvm::DIExpression * DIEstimateMemory::getExpression() {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Expr = llvm::dyn_cast<llvm::DIExpression>(MD->getOperand(I)))
      return Expr;
  llvm_unreachable("Expression must be specified!");
  return nullptr;
}

const llvm::DIExpression * DIEstimateMemory::getExpression() const {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
    if (auto *Expr = llvm::dyn_cast<llvm::DIExpression>(MD->getOperand(I)))
      return Expr;
  llvm_unreachable("Expression must be specified!");
  return nullptr;
}

unsigned DIMemory::getFlagsOp() const {
  auto MD = getAsMDNode();
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
  auto MD = getAsMDNode();
  auto CMD = cast<ConstantAsMetadata>(MD->getOperand(getFlagsOp()));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  return CInt->getZExtValue();
}

void DIMemory::setFlags(uint64_t F) {
  auto MD = getAsMDNode();
  auto OpIdx = getFlagsOp();
  auto CMD = cast<ConstantAsMetadata>(MD->getOperand(OpIdx));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  auto &Ctx = MD->getContext();
  auto *FlagMD = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
   Type::getInt64Ty(Ctx), CInt->getZExtValue() | F));
    MD->replaceOperandWith(OpIdx, FlagMD);
}

llvm::MDNode * DIUnknownMemory::getMetadata() {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    if (isa<DILocation>(MD->getOperand(I)))
      continue;
    if (auto *Op = llvm::dyn_cast<llvm::MDNode>(MD->getOperand(I)))
      return Op;
  }
  llvm_unreachable("MDNode must be specified!");
  return nullptr;
}

const llvm::MDNode * DIUnknownMemory::getMetadata() const {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    if (isa<DILocation>(MD->getOperand(I)))
      continue;
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
        dbgs() << "to ";
        TSAR_LLVM_DUMP(New->getAsMDNode()->dump());
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
    mTopLevelNode(new DIAliasTopNode), mFunc(&F) {
  ++NumAliasNode;
  mNodes.push_back(mTopLevelNode);
}

DIEstimateMemory & DIAliasTree::addNewNode(
    std::unique_ptr<DIEstimateMemory> &&EM, DIAliasNode &Parent) {
  ++NumAliasNode, ++NumEstimateNode;
  auto *N = new DIAliasEstimateNode;
  mNodes.push_back(N);
  N->setParent(Parent);
  return cast<DIEstimateMemory>(addToNode(std::move(EM), *N));
}

DIMemory & DIAliasTree::addNewUnknownNode(
    std::unique_ptr<DIMemory> &&M, DIAliasNode &Parent) {
  ++NumAliasNode, ++NumUnknownNode;
  auto *N = new DIAliasUnknownNode;
  mNodes.push_back(N);
  N->setParent(Parent);
  return addToNode(std::move(M), *N);
}

DIMemory & DIAliasTree::addToNode(
    std::unique_ptr<DIMemory> &&M, DIAliasMemoryNode &N) {
  auto Pair = mFragments.insert(M.release());
  assert(Pair.second && !(*Pair.first)->getAliasNode() &&
    "Memory location is already attached to a node!");
  (*Pair.first)->setAliasNode(N);
  N.push_back(**Pair.first);
  return **Pair.first;
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
#ifndef NDEBUG
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
  ImmutableCallSite CS(&V);
  if (auto Callee = [CS]() {
    return !CS ? nullptr : dyn_cast<Function>(
      CS.getCalledValue()->stripPointerCasts());
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
    DIParent = DIAT.addNewUnknownNode(std::move(DIM), *DIParent).getAliasNode();
  }
  CorruptedNodes.try_emplace(Corrupted, cast<DIAliasUnknownNode>(DIParent));
  for (CorruptedMemoryItem::size_type I = 0, E = Corrupted->size(); I < E; ++I) {
    auto DIM = Corrupted->pop();
    LLVM_DEBUG(addCorruptedLog(DIAT.getFunction(), *DIM));
    DIAT.addToNode(std::move(DIM), cast<DIAliasUnknownNode>(*DIParent));
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
        if (!(DIM = CMR.popFromCash(&EM))) {
          DIM = buildDIMemory(EM, DIAT.getFunction().getContext(), Env, DL, DT);
          LLVM_DEBUG(buildMemoryLog(DIAT.getFunction(), DT, *DIM, EM));
        }
        if (CMR.isCorrupted(*DIM).first) {
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
        if (!(DIM = CMR.popFromCash(Inst, true))) {
          DIM = buildDIMemory(*Inst, DIAT.getFunction().getContext(), Env, DT);
          LLVM_DEBUG(buildMemoryLog(DIAT.getFunction(), *DIM, *Inst));
        }
        if (CMR.isCorrupted(*DIM).first) {
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
        DIN = DIAT.addNewUnknownNode(
          std::move(Unknown.back()), *DIN).getAliasNode();
        Unknown.pop_back();
      }
      for (auto &M : Unknown) {
        LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *M));
        DIAT.addToNode(std::move(M), cast<DIAliasMemoryNode>(*DIN));
      }
    }
    if (!Known.empty()) {
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias estimate node\n");
      LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *Known.back()));
      DIN = DIAT.addNewNode(std::move(Known.back()), *DIN).getAliasNode();
      Known.pop_back();
      for (auto &M : Known) {
        LLVM_DEBUG(addMemoryLog(DIAT.getFunction(), *M));
        DIAT.addToNode(std::move(M), cast<DIAliasMemoryNode>(*DIN));
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
public:
  /// Creates a builder of subtree which contains specified fragments of a
  /// variable.
  DIAliasTreeBuilder(DIAliasTree &DIAT, LLVMContext &Ctx,
      DIMemoryEnvironment &Env,
      CorruptedMemoryResolver &CMR, CorruptedMap &CorruptedNodes,
      DIVariable *Var, const TinyPtrVector<DIExpression *> &Fragments) :
      mDIAT(&DIAT), mContext(&Ctx), mEnv(&Env),
      mCMR(&CMR), mCorruptedNodes(&CorruptedNodes),
      mVar(Var), mSortedFragments(Fragments) {
    assert(mDIAT && "Alias tree must not be null!");
    assert(mVar && "Variable must not be null!");
    assert(!Fragments.empty() && "At least one fragment must be specified!");
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
    if (mCorrupted = mCMR->hasUnknownParent(*mVar)) {
      Parent = addCorruptedNode(*mDIAT, mCorrupted, Parent, *mCorruptedNodes);
      if (auto N = dyn_cast<DIAliasUnknownNode>(Parent))
        mVisitedCorrupted.insert(N);
    }
    auto Ty = stripDIType(mVar->getType()).resolve();
    if (!Ty) {
      addFragments(Parent, 0, mSortedFragments.size());
      return;
    }
    if (mSortedFragments.front()->getNumElements() == 0) {
      LLVM_DEBUG(addFragmentLog(mSortedFragments.front()));
      auto &DIM = mDIAT->addNewNode(
        DIEstimateMemory::get(*mContext, *mEnv, mVar, mSortedFragments.front()),
        *Parent);
      if (auto M = mCMR->beforePromotion(
            DIMemoryLocation(mVar, mSortedFragments.front())))
        M->replaceAllUsesWith(&DIM);
      DIM.setProperties(DIMemory::Explicit);
      return;
    }
    auto LastInfo = mSortedFragments.back()->getFragmentInfo();
    if (LastInfo->OffsetInBits / 8 + (LastInfo->SizeInBits + 7) / 8 >
        getSize(Ty)) {
      addFragments(Parent, 0, mSortedFragments.size());
      return;
    }
    evaluateTy(DIExpression::get(*mContext, {}),
      Ty, 0, std::make_pair(0, mSortedFragments.size()), Parent);
  }

private:
#ifndef NDEBUG
  void addFragmentLog(llvm::DIExpression *Expr) {
    dbgs() << "[DI ALIAS TREE]: add a new node and a new fragment ";
    auto DWLang = getLanguage(mDIAT->getFunction()).getValue();
    printDILocationSource(DWLang, { mVar, Expr }, dbgs());
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
      auto &DIM = mDIAT->addNewNode(
        DIEstimateMemory::get(*mContext, *mEnv, mVar, mSortedFragments[I]),
        *Parent);
      if (auto M = mCMR->beforePromotion(
            DIMemoryLocation(mVar, mSortedFragments[I])))
        M->replaceAllUsesWith(&DIM);
      DIM.setProperties(DIMemory::Explicit);
    }
  }

  /// Inserts corrupted node which is a parent for a node with a specified
  /// fragment (`mVar`, `Expr`) if this is necessary.
  DIAliasNode * addUnknownParentIfNecessary(
      DIAliasNode *Parent, DIExpression *Expr) {
    auto *Corrupted = mCMR->hasUnknownParent(DIMemoryLocation(mVar, Expr));
    if (!Corrupted)
      return Parent;
    Corrupted->erase(
      mCorruptedReplacement.begin(), mCorruptedReplacement.end());
    if (Corrupted->empty())
      return mCorruptedNodes->try_emplace(Corrupted, Parent).first->second;
    return addCorruptedNode(*mDIAT, Corrupted, Parent, *mCorruptedNodes);
  }

  /// \brief Removes corrupted node equal to a specified node.
  ///
  /// \return `True` if corrupted node has been found in `mVisitedCorrupted`
  /// list and removed.
  /// \post If a removed corrupted has been the last one location in the node
  /// the node will be removed. If `Current`equal to a removed node it will be
  /// set to it's parent.
  bool eraseCorrupted(MDNode *MD, DIAliasNode *&Current) {
    for (auto *N : mVisitedCorrupted)
      for (auto &M : *N)
        if (M.getAsMDNode() == MD) {
          --NumCorruptedMemory;
          isa<DIEstimateMemory>(M) ? --NumEstimateMemory : --NumUnknownMemory;
          auto Parent = Current->getParent();
          if (mDIAT->erase(M).second) {
            auto &CN = (*mCorruptedNodes)[mCorrupted];
            Current = (CN == Current) ? Parent : Current;
            mVisitedCorrupted.erase(N);
            --NumUnknownNode;
            --NumAliasNode;
          }
          return true;
        }
    return false;
  }

  /// \brief Add subtypes of a specified type into the alias tree.
  ///
  /// A pair of `mVar` and `Expr` will represent a specified type in the tree.
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateTy(DIExpression *Expr, DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Expr && "Expression must not be null!");
    assert(Ty && "Type must not be null!");
    assert(Parent && "Alias node must not be null!");
    auto DIMTmp = DIEstimateMemory::get(*mContext, *mEnv, mVar, Expr);
    auto IsCorrupted = mCMR->isCorrupted(*DIMTmp);
    if (IsCorrupted.first) {
      if (!IsCorrupted.second) {
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: replace corrupted\n");
        bool IsErased = eraseCorrupted(DIMTmp->getAsMDNode(), Parent);
        LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add internal fragment\n");
        LLVM_DEBUG(addFragmentLog(DIMTmp->getExpression()));
        auto &NewM = mDIAT->addNewNode(std::move(DIMTmp), *Parent);
        // The corrupted has not been inserted into alias tree yet.
        // So it should be remembered to prevent such insertion later.
        if (!IsErased)
          mCorruptedReplacement.insert(NewM.getAsMDNode());
        Parent = NewM.getAliasNode();
      }
    } else {
      Parent = addUnknownParentIfNecessary(Parent, Expr);
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: add internal fragment\n");
      LLVM_DEBUG(addFragmentLog(DIMTmp->getExpression()));
      Parent = mDIAT->addNewNode(std::move(DIMTmp), *Parent).getAliasNode();
    }
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

  /// \brief Build coverage of fragments from a specified range
  /// [Fragments.first, Fragments.second) and update alias tree.
  ///
  /// The range of elements will be spitted into subranges. Each subrange
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
    bool IsFragmentsCoverage = false;
    for (; FragmentIdx < Fragments.second;
         ++FragmentIdx, IsFragmentsCoverage = true) {
      auto FInfo = mSortedFragments[FragmentIdx]->getFragmentInfo();
      auto FOffset = FInfo->OffsetInBits / 8;
      auto FSize = (FInfo->SizeInBits + 7) / 8;
      // If condition is true than elements in [FirstElIdx, ElIdx] cover
      // fragments in [FirstFragmentIdx, FragmentIdx).
      if (FOffset >= Offset + ElOffset + ElSize)
        break;
      // If condition is true than FragmentIdx cross the border of ElIdx and
      // it is necessary include the next element in coverage.
      if (FOffset + FSize > Offset + ElOffset + ElSize) {
        IsFragmentsCoverage = false;
        break;
      }
    }
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
    auto ElTy = stripDIType(DICTy->getBaseType()).resolve();
    auto ElSize = getSize(ElTy);
    unsigned FirstElIdx = 0;
    unsigned ElIdx = 0;
    auto FirstFragmentIdx = Fragments.first;
    auto FragmentIdx = Fragments.first;
    for (uint64_t ElOffset = 0, E = DICTy->getSizeInBits();
         ElOffset < E && FragmentIdx < Fragments.second;
         ElOffset += ElSize, ElIdx++) {
      evaluateElement(Offset, Fragments, Parent, ElTy,
        ElIdx, ElOffset, ElSize, FirstElIdx, FirstFragmentIdx, FragmentIdx);
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
      evaluateElement(Offset, Fragments, Parent, ElTy->getBaseType().resolve(),
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
  TinyPtrVector<DIExpression *> mSortedFragments;
  CorruptedMemoryItem *mCorrupted;
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
/// - There is no 'inttoptr' cast in the root.
/// the root.
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
      int64_t Offset;
      auto Base = GetPointerBaseWithConstantOffset(EM.front(), Offset, DL);
      auto CurrEM = &EM;
      while (Base == Root->front() && CurrEM->front() != Root->front()) {
        auto PtrTy =
          dyn_cast_or_null<PointerType>(CurrEM->front()->getType());
        if ((!PtrTy || !isa<ArrayType>(PtrTy->getPointerElementType())) &&
            (Offset != 0 || CurrEM->getSize() != Root->getSize())) {
          RootOffsets.try_emplace(CurrEM->front(), Offset);
        }
        CurrEM = CurrEM->getParent();
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
/// is unsuccessful.
DIVariable * buildDIExpression(const DataLayout &DL, const DominatorTree &DT,
    const Value *V, SmallVectorImpl<uint64_t> &Expr, bool &IsTemplate) {
  auto *Ty = V->getType();
  // If type is not a pointer then `GetPointerBaseWithConstantOffset` can
  // not be called.
  if (!Ty || !Ty->isPtrOrPtrVectorTy())
    return nullptr;
  int64_t Offset = 0;
  const Value *Curr = V, *Base = nullptr;
  while (true) {
    int64_t CurrOffset;
    Base = GetPointerBaseWithConstantOffset(Curr, CurrOffset, DL);
    Offset += CurrOffset;
    if (Curr == Base) {
      Base = GetUnderlyingObject(const_cast<Value *>(Curr), DL, 1);
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
  auto DILoc = findMetadata(Base, DILocs, &DT);
  if (!DILoc || (DILoc->Expr && !DILoc->Expr->isValid()))
    return nullptr;
  if (!isa<AllocaInst>(Base) && !isa<GlobalVariable>(Base))
    Expr.push_back(dwarf::DW_OP_deref);
  if (DILoc->Expr)
    Expr.append(DILoc->Expr->elements_begin(), DILoc->Expr->elements_end());
  return DILoc->Var;
};
}

std::unique_ptr<DIMemory> CorruptedMemoryResolver::popFromCash(
    const Value *V, bool IsExec) {
  auto Itr = mCashedUnknownMemory.find({ stripIntToPtr(V), IsExec });
  if (Itr == mCashedUnknownMemory.end())
    return nullptr;
  auto M = std::move(Itr->second);
  mCashedUnknownMemory.erase(Itr);
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
    if (!isa<DbgValueInst>(I))
      continue;
    auto Loc = DIMemoryLocation::get(&I);
    if (mSmallestFragments.count(Loc))
      continue;
    // Ignore expressions other than empty or fragment. Such expressions may
    // be created after some transform passes when llvm.dbg.declare is
    // replaced with llvm.dbg.value.
    if (!(Loc.Expr->getNumElements() == 0 ||
          (Loc.Expr->getNumElements() == 3 && Loc.Expr->getFragmentInfo()))) {
      auto VarFragments = mVarToFragments.find(Loc.Var);
      if (VarFragments == mVarToFragments.end())
        continue;
      for (auto *EraseExpr : VarFragments->get<DIExpression>())
        mSmallestFragments.erase(DIMemoryLocation{ Loc.Var, EraseExpr });
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
    auto VarFragments = mVarToFragments.try_emplace(Loc.Var, Loc.Expr);
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
            mSmallestFragments.erase(DIMemoryLocation{ Loc.Var, EraseExpr });
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
    Item = Pair.first->second = copyToCorrupted(Info.CorruptedWL, nullptr);
    Info.ParentOfUnknown = mAT->getTopLevelNode();
  } else {
    if (!Info.Items.empty() &&
        Info.ParentOfUnknown == mAT->getTopLevelNode()) {
      mergeRange(Info.Items.begin(), Info.Items.end());
      merge(*Info.Items.begin(), Info.PromotedWL.front().Var);
      Item = copyToCorrupted(Info.CorruptedWL, *Info.Items.begin());
    } else {
      Info.ParentOfUnknown = mAT->getTopLevelNode();
      auto Pair = mVarChildOfUnknown.try_emplace(
        Info.PromotedWL.front().Var);
      Item = Pair.first->second = copyToCorrupted(
        Info.CorruptedWL, Pair.second ? nullptr : Pair.first->second);
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
    Item = copyToCorrupted(Info.CorruptedWL, *Info.Items.begin());
  } else {
    auto P = *Info.NodesWL.begin();
    if (P != mAT->getTopLevelNode())
      while(P->getParent(*mAT) != Info.ParentOfUnknown)
        P = P->getParent(*mAT);
    auto Pair = mChildOfUnknown.try_emplace(P);
    Item = Pair.first->second = copyToCorrupted(
        Info.CorruptedWL, Pair.second ? nullptr : Pair.first->second);
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
    Item = copyToCorrupted(Info.CorruptedWL, *Info.Items.begin());
  } else {
    Info.ParentOfUnknown = mAT->getTopLevelNode();
    Item = copyToCorrupted(Info.CorruptedWL, nullptr);
    mDistinctUnknown.push_back(Item);
  }
  Info.CorruptedWL.clear();
  Info.Items.clear();
  Info.Items.push_back(Item);
}

CorruptedMemoryItem * CorruptedMemoryResolver::copyToCorrupted(
    const SmallVectorImpl<DIMemory *> &WL, CorruptedMemoryItem *Item) {
  if (!Item) {
    mCorrupted.push_back(make_unique<CorruptedMemoryItem>());
    Item = mCorrupted.back().get();
  }
  for (auto *M : WL) {
    ++NumCorruptedMemory;
    auto NewM = DIMemory::get(mFunc->getContext(), M->getEnv(), *M);
    for (auto &VH : *M) {
      if (!VH || isa<UndefValue>(VH))
        continue;
      NewM->bindValue(VH);
    }
    M->replaceAllUsesWith(NewM.get());
    Item->push(std::move(NewM));
  }
  return Item;
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
  for (auto &M : N) {
    LLVM_DEBUG(checkLog(M));
    auto Binding = M.getBinding();
    if (auto *DIEM = dyn_cast<DIEstimateMemory>(&M)) {
      DIMemoryLocation Loc(
        DIEM->getVariable(), DIEM->getExpression(), DIEM->isTemplate());
      auto FragmentItr = mSmallestFragments.find(Loc);
      if (FragmentItr != mSmallestFragments.end()) {
        if (Binding == DIMemory::Destroyed ||
            Binding == DIMemory::Empty) {
          Info.PromotedWL.push_back(Loc);
          FragmentItr->second = DIEM;
          continue;
        } else {
          LLVM_DEBUG(corruptedFoundLog(M));
          LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: safely promoted candidate is discarded\n");
          Info.CorruptedWL.push_back(&M);
          mCorruptedSet.insert({ M.getAsMDNode(), true });
          // This is rare case, so we do not take care about overheads and
          // remove an expression from a vector.
          auto VToF = mVarToFragments.find(Loc.Var);
          auto ExprItr = std::find(
            VToF->second.begin(), VToF->second.end(), Loc.Expr);
          VToF->second.erase(ExprItr);
          mSmallestFragments.erase(Loc);
        }
      } else if (Binding != DIMemory::Consistent ||
          !isSameAfterRebuild(*DIEM)) {
        LLVM_DEBUG(corruptedFoundLog(M));
        Info.CorruptedWL.push_back(&M);
        mCorruptedSet.insert({
          M.getAsMDNode(),
          Binding == DIMemory::Empty ||
            Binding == DIMemory::Destroyed ? false : true
        });
      }
    } else if (Binding != DIMemory::Consistent ||
        !isSameAfterRebuild(cast<DIUnknownMemory>(M))) {
      LLVM_DEBUG(corruptedFoundLog(M));
      LLVM_DEBUG(dbgs() << "[DI ALIAS TREE]: unknown corrupted is found\n");
      Info.CorruptedWL.push_back(&M);
      mCorruptedSet.insert({
        M.getAsMDNode(),
        Binding == DIMemory::Empty ||
          Binding == DIMemory::Destroyed ? false : true
      });
    }
    findBoundAliasNodes(M, *mAT, Info.NodesWL);
  }
}

bool CorruptedMemoryResolver::isSameAfterRebuild(DIEstimateMemory &M) {
  assert(M.getBinding() == DIMemory::Consistent &&
    "Inconsistent memory is always corrupted and can not be the same after rebuild!");
  DIMemory *RAUWd = nullptr;
  for (auto &VH : M) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    auto EM = mAT->find(MemoryLocation(VH, M.getSize()));
    assert(EM && "Estimate memory must be presented in the alias tree!");
    auto Cashed = mCashedMemory.try_emplace(EM);
    if (Cashed.second) {
      Cashed.first->second =
        tsar::buildDIMemory(*EM, mFunc->getContext(), M.getEnv(), *mDL, *mDT);
      LLVM_DEBUG(buildMemoryLog(
        mDIAT->getFunction(), *mDT, *Cashed.first->second, *EM));
    }
    assert(Cashed.first->second || "Debug memory location must not be null!");
    LLVM_DEBUG(afterRebuildLog(*Cashed.first->second));
    if (Cashed.first->second->getAsMDNode() != M.getAsMDNode())
      return false;
    assert((!RAUWd || RAUWd == Cashed.first->second.get()) &&
      "Different estimate memory locations produces the same debug-level memory!");
    RAUWd = Cashed.first->second.get();
  }
  assert(RAUWd && "Must not be null for consistent memory location!");
  M.replaceAllUsesWith(RAUWd);
  return true;
}

bool CorruptedMemoryResolver::isSameAfterRebuild(DIUnknownMemory &M) {
  assert(M.getBinding() == DIMemory::Consistent &&
    "Inconsistent memory is always corrupted and can not be the same after rebuild!");
  if (M.isDistinct())
    return false;
  DIMemory *RAUWd = nullptr;
  for (auto &VH : M) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    // In case of 'inttoptr' different values may have the same base.
    // The following values will be binded to the same DIMemory:
    // 'inttoptr i64 %Y to i64*'
    // 'inttoptr i64 %Y to %struct.S*'
    // So, we use %Y as a key in cash instead of each of values. Otherwise, cash
    // will be contained different keys (Value) with the same value (DIMemory).
    auto Cashed =
      mCashedUnknownMemory.try_emplace({ stripIntToPtr(VH), M.isExec() });
    if (Cashed.second) {
      Cashed.first->second = tsar::buildDIMemory(*VH, mFunc->getContext(),
        M.getEnv(), *mDT, M.getProperies(), M.getFlags());
      LLVM_DEBUG(buildMemoryLog(mDIAT->getFunction(), *Cashed.first->second, *VH));
    }
    assert(Cashed.first->second || "Debug memory location must not be null!");
    LLVM_DEBUG(afterRebuildLog(*Cashed.first->second));
    if (Cashed.first->second->getAsMDNode() != M.getAsMDNode())
      return false;
    assert((!RAUWd || RAUWd == Cashed.first->second.get()) &&
      "Different estimate memory locations produces the same debug-level memory!");
    RAUWd = Cashed.first->second.get();
  }
  assert(RAUWd && "Must not be null for consistent memory location!");
  M.replaceAllUsesWith(RAUWd);
  return true;
}

namespace tsar {
Optional<DIMemoryLocation> buildDIMemory(const MemoryLocation &Loc,
    LLVMContext &Ctx, const DataLayout &DL, const DominatorTree &DT) {
  assert(Loc.Ptr && "Pointer to memory location must not be null!");
  SmallVector<uint64_t, 8> ReverseExpr;
  bool IsTemplate = false;
  auto DIV = buildDIExpression(DL, DT, Loc.Ptr, ReverseExpr, IsTemplate);
  if (!DIV)
    return None;
  SmallVector<uint64_t, 8> Expr(ReverseExpr.rbegin(), ReverseExpr.rend());
  if (Expr.empty()) {
    auto DIE = DIExpression::get(Ctx, Expr);
    DIMemoryLocation DIL(DIV, DIE, IsTemplate);
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
  if (Loc.Size != MemoryLocation::UnknownSize) {
    if (Expr.empty () || LastDwarfOp == dwarf::DW_OP_deref ||
        LastDwarfOp == dwarf::DW_OP_minus || LastDwarfOp == dwarf::DW_OP_plus) {
      Expr.append({ dwarf::DW_OP_LLVM_fragment, 0, Loc.Size * 8});
    } else {
      assert(LastDwarfOp == dwarf::DW_OP_plus_uconst && "Unknown DWARF operand!");
      Expr[Expr.size() - 2] = dwarf::DW_OP_LLVM_fragment;
      Expr.push_back(Loc.Size * 8);
    }
  } else {
    if (!Expr.empty() && LastDwarfOp == dwarf::DW_OP_LLVM_fragment)
      Expr.back() = 0;
    else
      Expr.append({ dwarf::DW_OP_LLVM_fragment, 0, 0 });
  }
  auto DIE = DIExpression::get(Ctx, Expr);
  return DIMemoryLocation(DIV, DIE, IsTemplate);
}

llvm::MDNode * getRawDIMemoryIfExists(llvm::LLVMContext &Ctx,
    DIMemoryLocation DILoc) {
  auto Flags = DILoc.Template ?
    DIEstimateMemory::Template : DIEstimateMemory::NoFlags;
  return DIEstimateMemory::getRawIfExists(Ctx, DILoc.Var, DILoc.Expr, Flags);
}

std::unique_ptr<DIMemory> buildDIMemory(const EstimateMemory &EM,
    LLVMContext &Ctx, DIMemoryEnvironment &Env,
    const DataLayout &DL, const DominatorTree &DT) {
  auto DILoc = buildDIMemory(
    MemoryLocation(EM.front(), EM.getSize()), Ctx, DL, DT);
  std::unique_ptr<DIMemory> DIM;
  auto VItr = EM.begin();
  auto Properties = EM.isExplicit() ? DIMemory::Explicit : DIMemory::NoProperty;
  if (!DILoc) {
    auto F = ImmutableCallSite(*VItr) ? DIUnknownMemory::Result
                                      : DIUnknownMemory::Object;
    DIM =
      buildDIMemory(const_cast<Value &>(**VItr), Ctx, Env, DT, Properties, F);
    ++VItr;
  } else {
    auto Flags = DILoc->Template ?
      DIEstimateMemory::Template : DIEstimateMemory::NoFlags;
    DIM = DIEstimateMemory::get(Ctx, Env, DILoc->Var, DILoc->Expr, Flags);
    DIM->setProperties(Properties);
  }
  for (auto EItr = EM.end(); VItr != EItr; ++VItr)
    DIM->bindValue(const_cast<Value *>(*VItr));
  return DIM;
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
    } else if (auto DILoc = findMetadata(IntExpr, DILocs, &DT)) {
      MD = DILoc->Var;
    } else if (isa<Instruction>(V)) {
      Loc = cast<Instruction>(V).getDebugLoc().get();
    }
  } else {
    CallSite CS(&V);
    auto Callee = !CS ? dyn_cast_or_null<Function>(&V)
      : dyn_cast<Function>(CS.getCalledValue()->stripPointerCasts());
    if (Callee)
      MD = findMetadata(Callee);
    if (isa<Instruction>(V))
      Loc = cast<Instruction>(V).getDebugLoc().get();
  }
  auto DIM = DIUnknownMemory::get(Ctx, Env, MD, Loc, F);
  DIM->bindValue(&V);
  DIM->setProperties(P);
  return std::move(DIM);
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
  auto NewDIAT = make_unique<DIAliasTree>(F);
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
        CMR, CorruptedNodes, VToF.get<DIVariable>(), VToF.get<DIExpression>());
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
