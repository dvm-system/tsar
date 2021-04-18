//=== PointerScalarizer.cpp - Promoting certain values after SROA * C++ *-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements functional pass that tries to promote both pointers
// and values which address is taken to registers.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"

#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Transform/IR/InterprocAttr.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/InitializePasses.h>
#include <llvm/Transforms/Scalar.h>
#include <tsar/Analysis/Memory/Utils.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "ptr-red"

using namespace tsar;
using namespace llvm;

namespace {
class PointerScalarizerPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  PointerScalarizerPass() : FunctionPass(ID) {
    initializePointerScalarizerPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DIMemoryTraitPoolWrapper>();
    AU.addRequired<LoopAttributesDeductionPass>();
    AU.addRequired<EstimateMemoryPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<DIEstimateMemoryPass>();
  }
};

struct phiNodeLink {
  PHINode *phiNode;
  phiNodeLink *parent;

  explicit phiNodeLink(phiNodeLink *node) : phiNode(nullptr), parent(node) {}

  explicit phiNodeLink(PHINode *phi) : phiNode(phi), parent(nullptr) {}

  PHINode *getPhi() const {
    if (phiNode) {
      return phiNode;
    }
    return parent->getPhi();
  }
};

struct ScalarizerContext {
  explicit ScalarizerContext(Value *V, Function &F, Loop *L, DIBuilder *DIB, bool Changed)
      : V(V), DbgVar(), DbgLoc(), F(F), L(L), ValueChanged(Changed), DIB(DIB) {}

  Value *V;
  DIVariable *DbgVar;
  DILocation *DbgLoc;
  Function &F;
  Loop *L;
  SmallVector<LoadInst *, 2> InsertedLoads;
  DenseMap<BasicBlock *, phiNodeLink *> PhiLinks;
  DenseSet<PHINode *> UniqueNodes;
  DenseMap<BasicBlock *, Value *> LastValues;
  DenseSet<BasicBlock *> ChangedLastInst;
  bool ValueChanged;
  DIBuilder *DIB;
};
}

char PointerScalarizerPass::ID = 0;

INITIALIZE_PASS_BEGIN(PointerScalarizerPass, "ptr-scalar",
    "Pointer Scalarizer Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass);
INITIALIZE_PASS_DEPENDENCY(LoopAttributesDeductionPass);
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass);
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass);
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass);
INITIALIZE_PASS_END(PointerScalarizerPass, "ptr-scalar",
    "Pointer Scalarizer Pass", false, false)

FunctionPass * llvm::createPointerScalarizerPass() {
  return new PointerScalarizerPass();
}

static inline bool hasVolatileInstInLoop(ScalarizerContext &Ctx) {
  for (auto *User : Ctx.V->users()) {
    if (auto *LI = dyn_cast<LoadInst>(User))
      if (LI->isVolatile() && Ctx.L->contains(LI))
        return true;
    if (auto *SI = dyn_cast<StoreInst>(User))
      if (SI->isVolatile() && Ctx.L->contains(SI))
        return true;
  }
  return false;
}

static inline bool validateValue(const ScalarizerContext &Ctx) {
  for (auto *User : Ctx.V->users()) {
    auto *Store = dyn_cast<StoreInst>(User);
    if (Ctx.ValueChanged &&
        Store && Ctx.L->contains(Store) &&
        Store->getPointerOperand() == Ctx.V)
      return false;
  }
  return true;
}

static inline void insertDbgValueCall(ScalarizerContext &Ctx,
    Instruction *I, Instruction *InsertBefore, bool Add) {
  if (Add)
    I->setDebugLoc(Ctx.DbgLoc);
  Ctx.DIB->insertDbgValueIntrinsic(
    I, dyn_cast<DILocalVariable>(Ctx.DbgVar),
    DIExpression::get(Ctx.F.getContext(), {}),
    Ctx.DbgLoc, InsertBefore
  );
}

static void insertLoadInstructions(ScalarizerContext &Ctx) {
  auto *BeforeInstr = new LoadInst(
    Ctx.V->getType()->getPointerElementType(),
    Ctx.V, "load." + Ctx.V->getName(),
    &Ctx.L->getLoopPreheader()->back());
  Ctx.InsertedLoads.push_back(BeforeInstr);

  auto *insertBefore = &Ctx.L->getLoopPreheader()->back();
  insertDbgValueCall(Ctx, BeforeInstr, insertBefore, true);
  if (Ctx.ValueChanged) {
    auto BeforeInstr2 = new LoadInst(
      BeforeInstr->getType()->getPointerElementType(),
      BeforeInstr, "load.ptr." + Ctx.V->getName(),
      &Ctx.L->getLoopPreheader()->back());
    Ctx.InsertedLoads.push_back(BeforeInstr2);
  }
}

static inline void insertStoreInstructions(ScalarizerContext &Ctx) {
  SmallVector<BasicBlock *, 8> ExitBlocks;
  Ctx.L->getExitBlocks(ExitBlocks);
  for (auto *BB : ExitBlocks) {
    auto storeVal = Ctx.ValueChanged ? Ctx.InsertedLoads.front() : Ctx.V;
    new StoreInst(Ctx.LastValues[BB], storeVal, BB->getFirstNonPHI());
  }
}

static void getAllStoreOperands(LoadInst *I, DenseMap<StoreInst *, Instruction *> &Stores) {
  for (auto *User : I->users()) {
    auto *Child = dyn_cast<Instruction>(User);
    if (!Child)
      continue;
    for (auto *ChildUser : Child->users()) {
      if (auto *Store = dyn_cast<StoreInst>(ChildUser))
        if (Store->getPointerOperand() == I->getPointerOperand()) {
          Stores[Store] = Child;
          break;
        }
    }
  }
}

static void handleLoadsInBB(BasicBlock *BB, ScalarizerContext &Ctx) {
  SmallVector<Instruction *, 16> Loads;
  DenseMap<StoreInst *, Value *> Stores;
  auto ReplaceLoads = [&](LoadInst *Load, Value *ReplaceWith) {
    DenseMap<StoreInst *, Instruction *> storeInstructions;
    getAllStoreOperands(Load, storeInstructions);
    for (auto &Pair : storeInstructions) {
      Ctx.LastValues[Pair.second->getParent()] = Pair.second;
      Ctx.ChangedLastInst.insert(Pair.second->getParent());
    }
    Stores.insert(storeInstructions.begin(), storeInstructions.end());
    Load->replaceAllUsesWith(ReplaceWith);
  };
  for (auto &Instr : BB->getInstList()) {
    auto *SI = dyn_cast<StoreInst>(&Instr);
    auto *LI = dyn_cast<LoadInst>(&Instr);
    if (SI && SI->getPointerOperand() == Ctx.V) {
      Stores.insert({SI, SI->getValueOperand()});
      Ctx.LastValues[SI->getParent()] = SI->getValueOperand();
      Ctx.ChangedLastInst.insert(SI->getParent());
    } else if (LI && LI->getPointerOperand() == Ctx.V && !LI->user_empty()) {
      Value *Last = Ctx.LastValues[BB];
      if (!Ctx.ValueChanged) {
        ReplaceLoads(LI, Last);
      } else {
        for (auto *user : LI->users())
          if (auto *LoadChild = dyn_cast<LoadInst>(user)) {
            ReplaceLoads(LoadChild, Last);
            Loads.push_back(LoadChild);
          }
        ReplaceLoads(LI, Ctx.InsertedLoads.front());
      }
      Loads.push_back(LI);
    }
  }
  for (auto &Pair : Stores)
    if (auto *I = dyn_cast<Instruction>(Pair.second))
      insertDbgValueCall(Ctx, I, Pair.first, false);
  for (auto *Load : Loads) {
    Load->dropAllReferences();
    Load->eraseFromParent();
  }
  for (auto &Pair : Stores) {
    Pair.first->dropAllReferences();
    Pair.first->eraseFromParent();
  }
  if (pred_size(BB) == 1 && Ctx.ChangedLastInst.find(BB) == Ctx.ChangedLastInst.end())
    Ctx.LastValues[BB] = Ctx.LastValues[BB->getSinglePredecessor()];
}

static void handleLoads(ScalarizerContext &Ctx,
    BasicBlock *BB,
    DenseSet<BasicBlock *> &Completed,
    bool Init = false) {
  if (Completed.find(BB) != Completed.end())
    return;
  if (!Init)
    handleLoadsInBB(BB, Ctx);
  Completed.insert(BB);
  for (auto *Succ : successors(BB))
    if (Ctx.L->contains(Succ)) {
      handleLoads(Ctx, Succ, Completed);
    }
}

static inline void freeLinks(DenseMap<BasicBlock *, phiNodeLink *> &PhiLinks) {
  for (auto It : PhiLinks)
    delete It.second;
}

static void insertPhiNodes(ScalarizerContext &Ctx, BasicBlock *BB, bool Init = false) {
  if (Ctx.PhiLinks.count(BB))
    return;
  bool NeedsCreate = false;
  if (pred_size(BB) == 1 && !Init) {
    if (auto Itr{Ctx.PhiLinks.find(BB->getSinglePredecessor())}; Itr != Ctx.PhiLinks.end()) {
      auto *parentLink = Itr->second;
      Ctx.PhiLinks[BB] = new phiNodeLink(parentLink);
    } else {
      NeedsCreate = true;
    }
  } else if (!Init) {
    NeedsCreate = true;
  }
  if (NeedsCreate) {
    auto *Phi = PHINode::Create(Ctx.InsertedLoads.back()->getType(), 0,
        "Phi." + BB->getName(), &BB->front());
    insertDbgValueCall(Ctx, Phi, BB->getFirstNonPHI(), true);
    Ctx.PhiLinks[BB] = new phiNodeLink(Phi);
    Ctx.UniqueNodes.insert(Phi);
  }
  for (auto *Succ : successors(BB))
    if (Ctx.L->contains(Succ))
      insertPhiNodes(Ctx, Succ);
  // all nodes and links are created at this point and BB = loop predecessor
  if (Init) {
    Ctx.LastValues[BB] = Ctx.InsertedLoads.back();
    for (auto &P : Ctx.PhiLinks)
      Ctx.LastValues[P.getFirst()] = P.getSecond()->getPhi();
  }
}

static void fillPhiNodes(ScalarizerContext &Ctx) {
  for (auto *Phi : Ctx.UniqueNodes) {
    auto *BB = Phi->getParent();
    for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
      if (Ctx.LastValues.find(*Pred) != Ctx.LastValues.end()) {
        Phi->addIncoming(Ctx.LastValues[*Pred], *Pred);
      } else {
        auto *load = new LoadInst(Ctx.V->getType()->getPointerElementType(),
            Ctx.V, "dummy.load." + Ctx.V->getName(), &(*Pred)->back());
        Phi->addIncoming(load, *Pred);
      }
    }
  }
}

static void deleteRedundantPhiNodes(ScalarizerContext &Ctx) {
  for (auto *Phi : Ctx.UniqueNodes) {
    bool hasSameOperands = true;
    auto *operand = Phi->getOperand(0);
    for (int i = 0; i < Phi->getNumOperands(); ++i) {
      if (operand != Phi->getOperand(i)) {
        hasSameOperands = false;
        break;
      }
    }
    if (hasSameOperands) {
      Phi->replaceAllUsesWith(operand);
      Ctx.UniqueNodes.erase(Phi);
      Ctx.PhiLinks[Phi->getParent()]->phiNode = nullptr;
      auto *Instr = dyn_cast<Instruction>(operand);
      Ctx.PhiLinks[Phi->getParent()]->parent = Ctx.PhiLinks[Instr->getParent()];
      Phi->eraseFromParent();
    }
  }
}

static bool analyzeAliasTree(Value *V, AliasTree &AT, Loop *L, TargetLibraryInfo &TLI) {
  auto STR = SpanningTreeRelation<AliasTree *>(&AT);
  auto *EM = AT.find(MemoryLocation(V, LocationSize(0)));
  if (!EM)
    return false;
  EM = EM->getTopLevelParent();
  for (auto *BB : L->getBlocks()) {
    for (auto &Inst : BB->getInstList()) {
      bool HasWrite = false;
      auto *EMNode = EM->getAliasNode(AT);
      auto memLambda = [&STR, &HasWrite, &AT, &EM, &EMNode](
          Instruction &I, MemoryLocation &&Loc, unsigned, AccessInfo, AccessInfo W) {
        if (HasWrite || W == AccessInfo::No)
          return;
        auto InstEM = AT.find(Loc);
        assert(InstEM && "alias tree node is empty");
        auto *InstNode = InstEM->getAliasNode(AT);
        if (InstEM->getTopLevelParent() != EM && !STR.isUnreachable(EMNode, InstNode))
          HasWrite = true;
      };
      auto unknownMemLambda = [&HasWrite, &AT, &STR, &EMNode](
          Instruction &I, AccessInfo, AccessInfo W) {
        if (HasWrite || W == AccessInfo::No)
          return;
        auto *instEM = AT.findUnknown(&I);
        if (!STR.isEqual(instEM, EMNode) && !STR.isUnreachable(instEM, EMNode))
          HasWrite = true;
      };
      for_each_memory(Inst, TLI, memLambda, unknownMemLambda);
      if (HasWrite)
        return false;
    }
  }
  return true;
}

static void createDbgInfo(ScalarizerContext &Ctx,
    DIType *DIT, AliasTree &AT,
    SmallVectorImpl<Metadata *> &MDs) {
  auto &DL = Ctx.F.getParent()->getDataLayout();
  auto TypeSize = DL.getTypeStoreSize(Ctx.V->getType()->getPointerElementType());
  auto EM = AT.find(MemoryLocation(Ctx.V, LocationSize::precise(TypeSize)));
  if (EM == nullptr)
    return;
  auto *RawDIMem = getRawDIMemoryIfExists(
      *EM->getTopLevelParent(),
      Ctx.F.getContext(), DL,
      AT.getDomTree());
  auto *Scope = dyn_cast<DIScope>(Ctx.L->getStartLoc()->getScope());
  auto *NewVar = Ctx.DIB->createAutoVariable(
      Scope, "deref." + Ctx.DbgVar->getName().str(),
          Ctx.DbgLoc->getFile(), Ctx.DbgVar->getLine(),
      DIT, false,
      DINode::FlagZero);
  auto *Node = DINode::get(Ctx.F.getContext(), {RawDIMem, NewVar});
  MDs.push_back(Node);
  Ctx.DbgVar = NewVar;
}

bool PointerScalarizerPass::runOnFunction(Function &F) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &LoopAttr = getAnalysis<LoopAttributesDeductionPass>();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);

  auto MDsToAttach = SmallVector<Metadata*, 8>();
  for_each_loop(LI, [&TraitPool, &LoopAttr, &AT, &TLI, &F, &MDsToAttach](Loop *L) {
    // 1. Find memory that was marked anti/flow/output;
    // 2. Check that this memory is accessed inside the loop;
    // 3. If possible, copy its value at the preheader and store it back after the exit block(s);
    // 4. Replace all load/store instructions and their users in the loop's body
    //    with corresponding operations with the copied memory;
    // 5. Map inserted instructions with the original value using DI nodes.
    if (!L->getLoopID() ||
        !L->getLoopPreheader() ||
        !LoopAttr.hasAttr(*L, Attribute::NoUnwind) ||
        LoopAttr.hasAttr(*L, Attribute::Returned))
      return;
    auto &Pool = TraitPool[L->getLoopID()];
    if (!Pool)
      return;
    SmallDenseSet<Value *> Values;
    SmallDenseMap<Value *, DIMemoryTrait *> ValueTraitMapping;
    for (auto &T : *Pool) {
      if (!T.is_any<trait::Anti, trait::Flow, trait::Output>())
        continue;
      for (auto &V : *T.getMemory()) {
        if (!V.pointsToAliveValue() ||
            isa<UndefValue>(V) ||
            isa<GetElementPtrInst>(V) ||
            isa<LoadInst>(V))
          continue;
        if (auto *I = dyn_cast<Instruction>(V))
          if (L->contains(I))
            continue;
        for (auto *User : V->users()) {
          if (auto *SI = dyn_cast<StoreInst>(User))
            if (L->contains(SI) && SI->getPointerOperand() == V) {
              Values.insert(V);
              ValueTraitMapping[V] = &T;
              break;
            }
        }
      }
    }
    for (auto *Val : Values) {
      bool MultipleLoad = false;
      for (auto *User1 : Val->users())
        if (isa<LoadInst>(User1))
          for (auto *User2 : User1->users())
            if (isa<LoadInst>(User2))
              MultipleLoad = true;
      auto DIB = new DIBuilder(*F.getParent());
      auto Ctx = ScalarizerContext(Val, F, L, DIB, MultipleLoad);
      if (!validateValue(Ctx) ||
          hasVolatileInstInLoop(Ctx) ||
          !analyzeAliasTree(Val, AT, L, TLI)) {
        ValueTraitMapping.erase(Val);
        continue;
      }
      // find dbg.value call for Val and save it for adding debug information later
      for (auto &BB : F.getBasicBlockList()) {
        for (auto &Inst : BB.getInstList()) {
          if (Ctx.DbgVar && Ctx.DbgLoc)
            continue;
          if (auto *DVI = dyn_cast<DbgValueInst>(&Inst)) {
            if (DVI->getValue() == Val) {
              Ctx.DbgLoc = DVI->getDebugLoc();
              Ctx.DbgVar = DVI->getVariable();
            }
          } else if (auto *DDI = dyn_cast<DbgDeclareInst>(&Inst)) {
            if (DDI->getAddress() == Val) {
              Ctx.DbgLoc = DDI->getDebugLoc();
              Ctx.DbgVar = DDI->getVariable();
            }
          }
        }
      }
      if (dyn_cast<GlobalValue>(Ctx.V)) {
        SmallVector<DIMemoryLocation, 4> DILocs;
        auto MD = findMetadata(Ctx.V, DILocs, &AT.getDomTree());
        Ctx.DbgVar = MD->Var;
        Ctx.DbgLoc = L->getStartLoc();
        createDbgInfo(Ctx, Ctx.DbgVar->getType(), AT, MDsToAttach);
      } else {
        auto *derivedType = dyn_cast<DIDerivedType>(Ctx.DbgVar->getType());
        if (derivedType && Val->getType()->isPointerTy())
          createDbgInfo(Ctx, derivedType->getBaseType(), AT, MDsToAttach);
        else
          createDbgInfo(Ctx, Ctx.DbgVar->getType(), AT, MDsToAttach);
      }

      insertLoadInstructions(Ctx);
      insertPhiNodes(Ctx, L->getLoopPredecessor(), true);
      DenseSet<BasicBlock *> Processed;
      handleLoads(Ctx, L->getLoopPredecessor(), Processed, true);
      fillPhiNodes(Ctx);
      deleteRedundantPhiNodes(Ctx);
      insertStoreInstructions(Ctx);
      freeLinks(Ctx.PhiLinks);
    }
    for (auto &Pair: ValueTraitMapping)
      Pair.getSecond()->unset<trait::NoPromotedScalar>();
  });
  if (!MDsToAttach.empty()) {
    auto *MappingNode = DINode::get(F.getContext(), MDsToAttach);
    F.setMetadata("alias.tree.mapping", MappingNode);
  }
  return false;
}
