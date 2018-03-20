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
#include "CorruptedMemory.h"
#include "tsar_dbg_output.h"
#include "EstimateMemory.h"
#include "SpanningTreeRelation.h"
#include <IteratorDataAdaptor.h>
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
    assert(EM && "Estimate memory must be presented in the alias tree!");
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
      N = AT.findUnknown(cast<Instruction>(*VH));
    if (!N) {
      auto EM = AT.find(MemoryLocation(VH, 0));
      assert(EM && "Estimate memory must be presented in the alias tree!");
      N = EM->getAliasNode(AT);
    }
    assert(N && "Unknown memory must be presented in the alias tree!");
    Nodes.insert(N);
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

std::unique_ptr<DIUnknownMemory> DIUnknownMemory::get(llvm::LLVMContext &Ctx,
    DIUnknownMemory &UM) {
  ++NumUnknownMemory;
  return std::unique_ptr<DIUnknownMemory>(
    new DIUnknownMemory(UM.getAsMDNode()));
}

std::unique_ptr<DIUnknownMemory> DIUnknownMemory::get(llvm::LLVMContext &Ctx,
    llvm::MDNode *MD) {
  MDNode *NewMD = MD;
  if (&Ctx != &MD->getContext()) {
    SmallVector<Metadata *, 8> MDs;
    for (auto &Op : MD->operands())
      MDs.push_back(Op);
    auto NewMD = MDNode::get(Ctx, MDs);
    assert(NewMD && "Can not create metadata node!");
  }
  ++NumUnknownMemory;
  return std::unique_ptr<DIUnknownMemory>(new DIUnknownMemory(NewMD));
}

std::unique_ptr<DIEstimateMemory> DIEstimateMemory::get(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  auto MD = llvm::MDNode::get(Ctx, { Var, Expr, FlagMD });
  assert(MD && "Can not create metadata node!");
  ++NumEstimateMemory;
  return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(MD));
}

std::unique_ptr<DIEstimateMemory>
DIEstimateMemory::getIfExists(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  if (auto MD = llvm::MDNode::getIfExists(Ctx, { Var, Expr, FlagMD })) {
    ++NumEstimateMemory;
    return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(MD));
  }
  return nullptr;
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

unsigned DIEstimateMemory::getFlagsOp() const {
  auto MD = getAsMDNode();
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    auto &Op = MD->getOperand(I);
    if (isa<DIVariable>(Op) || isa<DIExpression>(Op))
      continue;
    auto CMD = dyn_cast<ConstantAsMetadata>(Op);
    if (!CMD)
      continue;
    if (auto CInt = dyn_cast<ConstantInt>(CMD->getValue()))
      return I;
  }
  llvm_unreachable("Explicit flag must be specified!");
}

DIEstimateMemory::Flags DIEstimateMemory::getFlags() const {
  auto MD = getAsMDNode();
  auto CMD = cast<ConstantAsMetadata>(MD->getOperand(getFlagsOp()));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  return static_cast<Flags>(CInt->getZExtValue());
}

void DIEstimateMemory::setFlags(Flags F) {
  auto MD = getAsMDNode();
  auto OpIdx = getFlagsOp();
  auto CMD = cast<ConstantAsMetadata>(MD->getOperand(OpIdx));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  auto &Ctx = MD->getContext();
  auto *FlagMD = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
   Type::getInt16Ty(Ctx), static_cast<Flags>(CInt->getZExtValue()) | F));
    MD->replaceOperandWith(OpIdx, FlagMD);
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
    DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias unknown node\n");
    auto DIM = Corrupted->pop();
    DEBUG(addCorruptedLog(DIAT.getFunction(), *DIM));
    DIParent = DIAT.addNewUnknownNode(std::move(DIM), *DIParent).getAliasNode();
  }
  CorruptedNodes.try_emplace(Corrupted, cast<DIAliasUnknownNode>(DIParent));
  for (CorruptedMemoryItem::size_type I = 0, E = Corrupted->size(); I < E; ++I) {
    auto DIM = Corrupted->pop();
    DEBUG(addCorruptedLog(DIAT.getFunction(), *DIM));
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
      DEBUG(dbgs() << "[DI ALIAS TREE]: process alias estimate node\n");
      for (auto &EM : *N) {
        auto Root = EM.getTopLevelParent();
        if (&EM != Root && !RootOffsets.count(EM.front())) {
          DEBUG(ignoreEMLog(EM, DT));
          continue;
        }
        std::unique_ptr<DIMemory> DIM;
        if (!(DIM = CMR.popFromCash(&EM))) {
          DIM = buildDIMemory(EM, DIAT.getFunction().getContext(), DL, DT);
          DEBUG(buildMemoryLog(DIAT.getFunction(), DT, *DIM, EM));
        }
        if (CMR.isCorrupted(*DIM).first) {
          DEBUG(dbgs() << "[DI ALIAS TREE]: ignore corrupted memory location\n");
          continue;
        }
        if (isa<DIEstimateMemory>(*DIM))
          Known.emplace_back(cast<DIEstimateMemory>(DIM.release()));
        else
          Unknown.push_back(std::move(DIM));
      }
    } else {
      assert(isa<AliasUnknownNode>(Child) && "It must be alias unknown node!");
      DEBUG(dbgs() << "[DI ALIAS TREE]: process alias unknown node\n");
      for (auto Inst : cast<AliasUnknownNode>(Child)) {
        std::unique_ptr<DIMemory> DIM;
        if (!(DIM = CMR.popFromCash(Inst))) {
          DIM = buildDIMemory(*Inst, DIAT.getFunction().getContext());
          DEBUG(buildMemoryLog(DIAT.getFunction(), *DIM, *Inst));
        }
        if (CMR.isCorrupted(*DIM).first) {
          DEBUG(dbgs() << "[DI ALIAS TREE]: ignore corrupted memory location\n");
          continue;
        }
        Unknown.push_back(std::move(DIM));
      }
    }
    if (!Unknown.empty()) {
      if (!isa<DIAliasUnknownNode>(DIN)) {
        DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias unknown node\n");
        DEBUG(addMemoryLog(DIAT.getFunction(), *Unknown.back()));
        DIN = DIAT.addNewUnknownNode(
          std::move(Unknown.back()), *DIN).getAliasNode();
        Unknown.pop_back();
      }
      for (auto &M : Unknown) {
        DEBUG(addMemoryLog(DIAT.getFunction(), *M));
        DIAT.addToNode(std::move(M), cast<DIAliasMemoryNode>(*DIN));
      }
    }
    if (!Known.empty()) {
      DEBUG(dbgs() << "[DI ALIAS TREE]: add new debug alias estimate node\n");
      DEBUG(addMemoryLog(DIAT.getFunction(), *Known.back()));
      DIN = DIAT.addNewNode(std::move(Known.back()), *DIN).getAliasNode();
      Known.pop_back();
      for (auto &M : Known) {
        DEBUG(addMemoryLog(DIAT.getFunction(), *M));
        DIAT.addToNode(std::move(M), cast<DIAliasMemoryNode>(*DIN));
      }
    }
    buildDIAliasTree(DL, DT, RootOffsets, CMR, DIAT, Child, *DIN, Nodes);
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
      CorruptedMemoryResolver &CMR, CorruptedMap &CorruptedNodes,
      DIVariable *Var, const TinyPtrVector<DIExpression *> &Fragments) :
      mDIAT(&DIAT), mContext(&Ctx),
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
    DEBUG(dbgs() << "[DI ALIAS TREE]: build subtree for a variable "
      << mVar->getName() << "\n");
    DIAliasNode *Parent = mDIAT->getTopLevelNode();
    if (mCorrupted = mCMR->hasUnknownParent(*mVar))
      Parent = mCorruptedNode =
        addCorruptedNode(*mDIAT, mCorrupted, Parent, *mCorruptedNodes);
    auto Ty = stripDIType(mVar->getType()).resolve();
    if (!Ty) {
      addFragments(Parent, 0, mSortedFragments.size());
      return;
    }
    if (mSortedFragments.front()->getNumElements() == 0) {
      DEBUG(addFragmentLog(mSortedFragments.front()));
      auto &DIM = mDIAT->addNewNode(
        DIEstimateMemory::get(*mContext, mVar, mSortedFragments.front()),
        *Parent);
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
      DEBUG(addFragmentLog(mSortedFragments[I]));
      if (auto *Corrupted = mCMR->hasUnknownParent(
            DIMemoryLocation(mVar, mSortedFragments[I])))
        Parent = addCorruptedNode(*mDIAT, Corrupted, Parent, *mCorruptedNodes);
      auto &DIM = mDIAT->addNewNode(
        DIEstimateMemory::get(*mContext, mVar, mSortedFragments[I]), *Parent);
      DIM.setProperties(DIMemory::Explicit);
    }
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
    auto DIMTmp = DIEstimateMemory::get(*mContext, mVar, Expr);
    auto IsCorrupted = mCMR->isCorrupted(*DIMTmp);
    if (IsCorrupted.first) {
      if (!IsCorrupted.second) {
        DEBUG(dbgs() << "[DI ALIAS TREE]: erase corrupted\n");
        assert(mCorruptedNode && "Corrupted node must not be null!");
        for (auto &M : cast<DIAliasUnknownNode>(*mCorruptedNode))
          if (M.getAsMDNode() == DIMTmp->getAsMDNode()) {
            --NumCorruptedMemory;
            isa<DIEstimateMemory>(M) ? --NumEstimateMemory : --NumUnknownMemory;
            if (mDIAT->erase(M).second) {
              auto &CN = (*mCorruptedNodes)[mCorrupted];
              Parent = (CN == Parent) ? CN = mDIAT->getTopLevelNode() : Parent;
              --NumUnknownNode;
              --NumAliasNode;
            }
            break;
          }
        DEBUG(addFragmentLog(DIMTmp->getExpression()));
        Parent = mDIAT->addNewNode(std::move(DIMTmp), *Parent).getAliasNode();
      }
    } else {
      DEBUG(addFragmentLog(DIMTmp->getExpression()));
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
      dwarf::DW_OP_LLVM_fragment, Offset, Ty->getSizeInBits()});
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
      // fragments in [mFirstFragment, FragmentIdx).
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
  CorruptedMemoryResolver *mCMR;
  CorruptedMap *mCorruptedNodes;
  DIVariable *mVar;
  TinyPtrVector<DIExpression *> mSortedFragments;
  CorruptedMemoryItem *mCorrupted;
  DIAliasNode *mCorruptedNode;
};

/// \brief Returns list of estimate memory locations which should be inserted
/// into a constructed DIAliasTree.
///
/// The following location and their offsets are returned:
/// - It has a known offset from its root in estimate memory tree.
/// - All descendant locations in estimate memory tree have known offsets from
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
      auto Root = EM.getTopLevelParent()->front();
      int64_t Offset;
      auto Base = GetPointerBaseWithConstantOffset(EM.front(), Offset, DL);
      auto CurrEM = &EM;
      while (Base == Root && CurrEM->front() != Root) {
        auto PtrTy =
          dyn_cast_or_null<PointerType>(CurrEM->front()->getType());
        if (!PtrTy || !isa<ArrayType>(PtrTy->getPointerElementType())) {
          RootOffsets.try_emplace(CurrEM->front(), Offset);
        }
        CurrEM = CurrEM->getParent();
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
      Expr.push_back(dwarf::DW_OP_plus);
  } else if (Offset < 0) {
    Expr.push_back(-Offset);
    Expr.push_back(dwarf::DW_OP_minus);
  }
  if (Offset != 0)
    Expr[Expr.size() - 2] *= 8;
  if (auto LI = dyn_cast<LoadInst>(Base)) {
    Expr.push_back(dwarf::DW_OP_deref);
    return buildDIExpression(DL, DT, LI->getPointerOperand(), Expr, IsTemplate);
  }
  SmallVector<DIVariable *, 4> Vars;
  auto DIVar = findMetadata(Base, Vars, &DT);
  if (DIVar && !isa<AllocaInst>(Base) && !isa<GlobalVariable>(Base))
    Expr.push_back(dwarf::DW_OP_deref);
  return DIVar;
};
}

void CorruptedMemoryResolver::resolve() {
  DEBUG(dbgs() << "[DI ALIAS TREE]: determine safely promoted memory locations\n");
  findNoAliasFragments();
  DEBUG(pomotedCandidatesLog());
  if (!mDIAT)
    return;
  DEBUG(dbgs() << "[DI ALIAS TREE]: resolve corrupted memory locations\n");
  SpanningTreeRelation<AliasTree *> AliasSTR(mAT);
  for (auto *N : post_order(mDIAT)) {
    DEBUG(dbgs() << "[DI ALIAS TREE]: visit debug alias node\n");
    if (isa<DIAliasTopNode>(N))
      continue;
    determineCorruptedInsertionHint(cast<DIAliasMemoryNode>(*N), AliasSTR);
  }
  // Now all corrupted items will be forward to the root of theirs graph.
  // Each graph consists of connected items.
  for (auto &Root : mCorrupted) {
    if (Root->getForward())
      continue;
#ifdef DEBUG
      CorruptedPool::size_type GraphSize = 0;
#endif
      for (auto *Item : depth_first(Root.get())) {
        DEBUG(++GraphSize);
        Item->setForward(Root.get());
      }
      DEBUG(dbgs() << "[DI ALIAS TREE]: forward corrupted graph of size "
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
      mSmallestFragments.insert(std::move(Loc));
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
    mSmallestFragments.insert(std::move(Loc));
  }
}

void CorruptedMemoryResolver::determineCorruptedInsertionHint(DIAliasMemoryNode &N,
    const SpanningTreeRelation<AliasTree *> &AliasSTR) {
  collapseChildStack(N, AliasSTR);
  auto &Info = mChildStack.back();
  updateWorkLists(N, Info);
  if (Info.CorruptedWL.empty()) {
    DEBUG(dbgs() <<
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
    DEBUG(dbgs() << "[DI ALIAS TREE]: push node information to stack\n");
    DEBUG(Info.sizeLog());
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
    DEBUG(dbgs() << "[DI ALIAS TREE]: push node information to stack\n");
    DEBUG(Info.sizeLog());
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
  DEBUG(dbgs() << "[DI ALIAS TREE]: push node information to stack\n");
  DEBUG(Info.sizeLog());
  mChildStack.push_back(std::move(Info));
}

void CorruptedMemoryResolver::promotedBasedHint(NodeInfo &Info) {
  DEBUG(dbgs() << "[DI ALIAS TREE]: determine promoted based hint\n");
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
  DEBUG(dbgs() << "[DI ALIAS TREE]: determine alias tree based hint\n");
  auto PrevParentOfUnknown = Info.ParentOfUnknown;
  if (Info.ParentOfUnknown != mAT->getTopLevelNode()) {
    using NodeItr = bcl::IteratorDataAdaptor<
      SmallPtrSetImpl<AliasNode *>::iterator,
      AliasTree *, GraphTraits<Inverse<AliasTree *>>::NodeRef>;
    AliasNode *LCA = *findLCA(AliasSTR,
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
  DEBUG(dbgs() << "[DI ALIAS TREE]: determine distinct unknown hint\n");
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
    auto NewM = DIMemory::get(mFunc->getContext(), *M);
    for (auto &VH : *M) {
      if (!VH || isa<UndefValue>(VH))
        continue;
      NewM->bindValue(VH);
    }
    Item->push(std::move(NewM));
  }
  return Item;
}

void CorruptedMemoryResolver::merge(
    CorruptedMemoryItem *LHS, CorruptedMemoryItem *RHS) {
  assert(LHS && RHS && "Merged items must not be null!");
  DEBUG("[DI ALIAS TREE]: merge two corrupted lists");
  if (LHS == RHS)
    return;
  LHS->addSuccessor(RHS);
  RHS->addPredecessor(LHS);
}

void CorruptedMemoryResolver::updateWorkLists(
    DIAliasMemoryNode &N, NodeInfo &Info) {
  for (auto &M : N) {
    DEBUG(checkLog(M));
    auto Binding = M.getBinding();
    if (auto *DIEM = dyn_cast<DIEstimateMemory>(&M)) {
      DIMemoryLocation Loc(
        DIEM->getVariable(), DIEM->getExpression(), DIEM->isTemplate());
      if (mSmallestFragments.count(Loc)) {
        if (Binding == DIMemory::Destroyed ||
            Binding == DIMemory::Empty) {
          Info.PromotedWL.push_back(Loc);
          continue;
        } else {
          DEBUG(corruptedFoundLog(M));
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
        DEBUG(corruptedFoundLog(M));
        Info.CorruptedWL.push_back(&M);
        mCorruptedSet.insert({
          M.getAsMDNode(),
          Binding == DIMemory::Empty ||
            Binding == DIMemory::Destroyed ? false : true
        });
      }
    } else if (Binding != DIMemory::Consistent ||
        !isSameAfterRebuild(cast<DIUnknownMemory>(M))) {
      DEBUG(corruptedFoundLog(M));
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
  for (auto &VH : M) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    auto EM = mAT->find(MemoryLocation(VH, M.getSize()));
    assert(EM && "Estimate memory must be presented in the alias tree!");
    auto Cashed = mCashedMemory.try_emplace(EM);
    if (Cashed.second) {
      Cashed.first->second =
        buildDIMemory(*EM, mFunc->getContext(), *mDL, *mDT);
      DEBUG(buildMemoryLog(
        mDIAT->getFunction(), *mDT, *Cashed.first->second, *EM));
    }
    assert(Cashed.first->second || "Debug memory location must not be null!");
    DEBUG(afterRebuildLog(*Cashed.first->second));
    if (Cashed.first->second->getAsMDNode() != M.getAsMDNode())
      return false;
  }
  return true;
}

bool CorruptedMemoryResolver::isSameAfterRebuild(DIUnknownMemory &M) {
  for (auto &VH : M) {
    if (!VH || isa<UndefValue>(VH))
      continue;
    auto Cashed = mCashedUnknownMemory.try_emplace(VH);
    if (Cashed.second) {
      Cashed.first->second = buildDIMemory(*VH, mFunc->getContext());
      DEBUG(buildMemoryLog(mDIAT->getFunction(), *Cashed.first->second, *VH));
    }
    assert(Cashed.first->second || "Debug memory location must not be null!");
    DEBUG(afterRebuildLog(*Cashed.first->second));
    if (Cashed.first->second->getAsMDNode() != M.getAsMDNode())
      return false;
  }
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
  if (Loc.Size != MemoryLocation::UnknownSize) {
    uint64_t LastDwarfOp;
    for (size_t I = 0, E = Expr.size(); I < E; ++I) {
      LastDwarfOp = Expr[I];
      if (Expr[I] != dwarf::DW_OP_deref)
        ++I;
    }
    if (Expr.empty () || LastDwarfOp == dwarf::DW_OP_deref ||
        LastDwarfOp == dwarf::DW_OP_minus) {
      Expr.append({ dwarf::DW_OP_LLVM_fragment, 0, Loc.Size * 8});
    } else {
      Expr[Expr.size() - 2] = dwarf::DW_OP_LLVM_fragment;
      Expr.push_back(Loc.Size * 8);
    }
  }
  auto DIE = DIExpression::get(Ctx, Expr);
  return DIMemoryLocation(DIV, DIE, IsTemplate);
}

std::unique_ptr<DIMemory> buildDIMemory(const EstimateMemory &EM,
    LLVMContext &Ctx, const DataLayout &DL, const DominatorTree &DT) {
  // Do not add size to expression for root. Size can be obtained from variable
  // type. If variable will be promoted it will be represented without this
  // size. So there are will be different locations for the same memory before
  // and after promotion.
  auto Size = (&EM == EM.getTopLevelParent()) ?
    MemoryLocation::UnknownSize : EM.getSize();
  auto DILoc = buildDIMemory(
    MemoryLocation(EM.front(), Size), Ctx, DL, DT);
  std::unique_ptr<DIMemory> DIM;
  if (DILoc) {
    auto Flags = DILoc->Template ?
      DIEstimateMemory::Template : DIEstimateMemory::NoFlags;
    DIM = DIEstimateMemory::get(Ctx, DILoc->Var, DILoc->Expr, Flags);
  } else {
    auto MD = MDNode::get(Ctx, { nullptr });
    MD->replaceOperandWith(0, MD);
    DIM = DIUnknownMemory::get(Ctx, MD);
  }
  auto Properties = EM.isExplicit() ? DIMemory::Explicit : DIMemory::NoProperty;
  DIM->setProperties(Properties);
  for (auto &V : EM) {
    DIM->bindValue(const_cast<Value *>(V));
  }
  return DIM;
}

std::unique_ptr<DIMemory> buildDIMemory(Value &V, LLVMContext &Ctx) {
  CallSite CS(&V);
  auto Callee = !CS ? nullptr : dyn_cast<Function>(
      CS.getCalledValue()->stripPointerCasts());
  MDNode *MD;
  if (Callee) {
    /// TODO (kaniandr@gmail.com): Clang do not add metadata for function
    /// prototypes. May be some special pass should be added to insert such
    /// metadata.
    if (!(MD = Callee->getSubprogram())) {
      MD = MDNode::get(Ctx, { nullptr });
      MD->replaceOperandWith(0, MD);
    }
  } else {
    MD = MDNode::get(Ctx, { nullptr });
    MD->replaceOperandWith(0, MD);
  }
  auto DIM = DIUnknownMemory::get(Ctx, MD);
  DIM->bindValue(&V);
  DIM->setProperties(DIMemory::Explicit);
  return DIM;
}
}

namespace llvm {
/// Storage for debug alias trees for all analyzed functions.
class DIAliasTreeImmutableStorage :
  public ImmutablePass, private bcl::Uncopyable {
  /// \brief This defines callback that run when underlying function has RAUW
  /// called on it or destroyed.
  ///
  /// This updates map from function to its debug alias tree.
  class FunctionCallbackVH final : public CallbackVH {
    DIAliasTreeImmutableStorage *mStorage;
    void deleted() override {
      mStorage->erase(cast<Function>(*getValPtr()));
    }
    void allUsesReplacedWith(Value *V) override {
      if (auto F = dyn_cast<Function>(V))
        mStorage->reset(*F, mStorage->release(cast<Function>(*getValPtr())));
      else
        mStorage->erase(cast<Function>(*getValPtr()));
    }
  public:
    FunctionCallbackVH(Value *V, DIAliasTreeImmutableStorage *S = nullptr) :
      CallbackVH(V), mStorage(S) {}
    FunctionCallbackVH & operator=(Value *V) {
      return *this = FunctionCallbackVH(V, mStorage);
    }
  };

  struct FunctionCallbackVHDenseMapInfo : public DenseMapInfo<Value *> {};

  /// Map from a function to its debug alias tree.
  using FunctionToTreeMap =
    DenseMap<FunctionCallbackVH, std::unique_ptr<DIAliasTree>,
    FunctionCallbackVHDenseMapInfo>;

public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DIAliasTreeImmutableStorage() : ImmutablePass(ID) {}

  /// Resets alias tree for a specified function with a specified alias tree
  /// and returns pointer to a new tree.
  DIAliasTree * reset(Function &F, std::unique_ptr<DIAliasTree> &&AT) {
    auto Itr = mTrees.try_emplace(FunctionCallbackVH(&F, this)).first;
    Itr->second = std::move(AT);
    return Itr->second.get();
  }

  /// Extracts alias tree for a specified function from storage and returns it.
  std::unique_ptr<DIAliasTree> release(Function &F) {
    auto Itr = mTrees.find_as(&F);
    if (Itr != mTrees.end()) {
      auto AT = std::move(Itr->second);
      mTrees.erase(Itr);
      return AT;
    }
    return nullptr;
  }

  /// Erases alias tree for a specified function from the storage.
  void erase(Function &F) {
    auto Itr = mTrees.find_as(&F);
    if (Itr != mTrees.end())
      mTrees.erase(Itr);
  }

  /// Returns alias tree for a specified function or nullptr.
  DIAliasTree * get(Function &F) const {
    auto Itr = mTrees.find_as(&F);
    return Itr == mTrees.end() ? nullptr : Itr->second.get();
  }

  /// Returns alias tree for a specified function or nullptr.
  DIAliasTree * operator[](Function &F) const { return get(F); }

private:
  FunctionToTreeMap mTrees;
};
}

char DIAliasTreeImmutableStorage::ID = 0;
INITIALIZE_PASS(DIAliasTreeImmutableStorage, "di-estimate-mem-is",
  "Memory Estimator (Debug, Immutable Storage)", true, true)

char DIEstimateMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(DIEstimateMemoryPass, "di-estimate-mem",
  "Memory Estimator (Debug)", false, true)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DIAliasTreeImmutableStorage)
INITIALIZE_PASS_END(DIEstimateMemoryPass, "di-estimate-mem",
  "Memory Estimator (Debug)", false, true)

void DIEstimateMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<DIAliasTreeImmutableStorage>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createDIEstimateMemoryPass() {
  return new DIEstimateMemoryPass();
}

bool DIEstimateMemoryPass::runOnFunction(Function &F) {
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &Storage = getAnalysis<DIAliasTreeImmutableStorage>();
  auto &DL = F.getParent()->getDataLayout();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  CorruptedMemoryResolver CMR(F, &DL, &DT, Storage[F], &AT);
  CMR.resolve();
  mDIAliasTree = Storage.reset(F, make_unique<DIAliasTree>(F));
  CorruptedMap CorruptedNodes;
  DEBUG(dbgs() <<
    "[DI ALIAS TREE]: add distinct unknown nodes for corrupted memory\n");
  for (size_t Idx = 0,IdxE = CMR.distinctUnknownNum(); Idx < IdxE; ++Idx)
    addCorruptedNode(*mDIAliasTree, CMR.distinctUnknown(Idx),
      mDIAliasTree->getTopLevelNode(), CorruptedNodes);
  for (auto &VToF : CMR.getFragments()) {
    if (VToF.get<DIExpression>().empty())
      continue;
    DIAliasTreeBuilder Builder(*mDIAliasTree, F.getContext(),
      CMR, CorruptedNodes, VToF.get<DIVariable>(), VToF.get<DIExpression>());
    Builder.buildSubtree();
  }
  auto RootOffsets = findLocationToInsert(AT, DL);
  DEBUG(dbgs() <<
    "[DI ALIAS TREE]: use an existing alias tree to add new nodes\n");
  DEBUG(constantOffsetLog(RootOffsets.begin(), RootOffsets.end(), DT));
  buildDIAliasTree(DL, DT, RootOffsets, CMR, *mDIAliasTree,
    *AT.getTopLevelNode(), *mDIAliasTree->getTopLevelNode(), CorruptedNodes);
  std::vector<Metadata *> MemoryNodes(mDIAliasTree->memory_size());
  std::transform(mDIAliasTree->memory_begin(), mDIAliasTree->memory_end(),
    MemoryNodes.begin(), [](DIMemory &EM) { return EM.getAsMDNode(); });
  auto AliasTreeMDKind = F.getContext().getMDKindID("alias.tree");
  auto MD = MDNode::get(F.getContext(), MemoryNodes);
  F.setMetadata(AliasTreeMDKind, MD);
  return false;
}
