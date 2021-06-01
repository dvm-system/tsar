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

#include "tsar/Analysis/Attributes.h"
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
#include <llvm/Transforms/Utils/Local.h>
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

struct PhiNodeLink {
  PHINode *phiNode;
  PhiNodeLink *parent;

  PhiNodeLink() = default;

  explicit PhiNodeLink(PhiNodeLink *node) : phiNode(nullptr), parent(node) {}

  explicit PhiNodeLink(PHINode *phi) : phiNode(phi), parent(nullptr) {}

  bool hasValue() const {
    return phiNode || (parent && parent->hasValue());
  }

  PHINode *getPhi() const {
    if (phiNode) {
      return phiNode;
    }
    return parent->getPhi();
  }
};

struct ScalarizerContext {
  explicit ScalarizerContext(Value *V, Function &F, Loop *L, DIBuilder *DIB)
      : V(V), DbgVar(), DbgLoc(), F(F), L(L), DIB(DIB), PhiLinks{} {
    for (auto *BB : L->getBlocks())
      PhiLinks.insert({BB, PhiNodeLink()});
  }

  Value *V;
  DIVariable *DbgVar;
  DILocation *DbgLoc;
  Function &F;
  Loop *L;
  LoadInst *InsertedLoad;
  DenseMap<BasicBlock *, PhiNodeLink> PhiLinks;
  DenseSet<PHINode *> UniqueNodes;
  DenseMap<BasicBlock *, Value *> LastValues;
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
  Ctx.InsertedLoad = BeforeInstr;
  auto *insertBefore = &Ctx.L->getLoopPreheader()->back();
  insertDbgValueCall(Ctx, BeforeInstr, insertBefore, true);
}

static inline void insertStoreInstructions(ScalarizerContext &Ctx) {
  SmallVector<BasicBlock *, 8> ExitBlocks;
  Ctx.L->getExitBlocks(ExitBlocks);
  for (auto *BB : ExitBlocks) {
    new StoreInst(Ctx.LastValues[BB], Ctx.V, BB->getFirstNonPHI());
  }
}

static void handleMemoryAccess(BasicBlock *BB, ScalarizerContext &Ctx) {
  SmallVector<Instruction *, 16> Loads;
  DenseMap<StoreInst *, Value *> Stores;
  SmallVector<Instruction *, 16> ToDelete;
  bool LastValuesChanged = false;
  for (auto &Instr : BB->getInstList()) {
    auto *SI = dyn_cast<StoreInst>(&Instr);
    auto *LI = dyn_cast<LoadInst>(&Instr);
    if (SI && SI->getPointerOperand() == Ctx.V){
      ToDelete.push_back(SI);
      Ctx.LastValues[SI->getParent()] = SI->getValueOperand();
      LastValuesChanged = true;
    } else if (LI && LI->getPointerOperand() == Ctx.V && !LI->user_empty()) {
      Value *Last = Ctx.LastValues[BB];
      LI->replaceAllUsesWith(Last);
      ToDelete.push_back(LI);
    }
  }
  for (auto *I : ToDelete) {
    I->dropAllReferences();
    I->eraseFromParent();
  }
  if (pred_size(BB) == 1 && !LastValuesChanged)
    Ctx.LastValues[BB] = Ctx.LastValues[BB->getSinglePredecessor()];
}

static void handleLoads(ScalarizerContext &Ctx,
    BasicBlock *BB,
    DenseSet<BasicBlock *> &Completed,
    bool Init = false) {
  if (Completed.find(BB) != Completed.end())
    return;
  if (!Init)
    handleMemoryAccess(BB, Ctx);
  Completed.insert(BB);
  for (auto *Succ : successors(BB))
    if (Ctx.L->contains(Succ))
      handleLoads(Ctx, Succ, Completed);
    else
      Ctx.LastValues[Succ] = Ctx.LastValues[BB];
}

static void insertPhiNodes(ScalarizerContext &Ctx, BasicBlock *BB, bool Init = false) {
  if (Ctx.L->contains(BB) && Ctx.PhiLinks[BB].hasValue())
    return;
  bool NeedsCreate = false;
  if (pred_size(BB) == 1 && !Init) {
    auto *Pred = BB->getSinglePredecessor();
    if (Ctx.L->contains(Pred) && Ctx.PhiLinks[Pred].hasValue())
      Ctx.PhiLinks[BB] = Ctx.PhiLinks[Pred];
    else
      NeedsCreate = true;
  } else if (!Init) {
    NeedsCreate = true;
  }
  if (NeedsCreate) {
    auto *Phi = PHINode::Create(Ctx.InsertedLoad->getType(), 0,
        "Phi." + BB->getName(), &BB->front());
    insertDbgValueCall(Ctx, Phi, BB->getFirstNonPHI(), true);
    Ctx.PhiLinks[BB] = PhiNodeLink(Phi);
    Ctx.UniqueNodes.insert(Phi);
  }
  for (auto *Succ : successors(BB))
    if (Ctx.L->contains(Succ))
      insertPhiNodes(Ctx, Succ);
  // all nodes and links are created at this point and BB = loop predecessor
  if (Init) {
    Ctx.LastValues[BB] = Ctx.InsertedLoad;
    for (auto &P : Ctx.PhiLinks)
      Ctx.LastValues[P.getFirst()] = P.getSecond().getPhi();
  }
}

static void fillPhiNodes(ScalarizerContext &Ctx) {
  for (auto *Phi : Ctx.UniqueNodes) {
    auto *BB = Phi->getParent();
    for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
      if (Ctx.LastValues.find(*Pred) != Ctx.LastValues.end())
        Phi->addIncoming(Ctx.LastValues[*Pred], *Pred);
    }
  }
}

static void deleteRedundantPhiNodes(ScalarizerContext &Ctx) {
  bool Changed = false;
  do {
    SmallVector<PHINode *, 4> ToDelete;
    for (auto *Phi: Ctx.UniqueNodes) {
      bool SameOperands = true;
      auto *Op = Phi->getOperand(0);
      for (auto *OtherOp: Phi->operand_values())
        if (Op != OtherOp) {
          SameOperands = false;
          break;
        }
      if (SameOperands)
        ToDelete.emplace_back(Phi);
    }
    Changed = !ToDelete.empty();
    for (auto *Phi : ToDelete) {
      Phi->replaceAllUsesWith(Phi->getOperand(0));
      Ctx.UniqueNodes.erase(Phi);
      Ctx.PhiLinks[Phi->getParent()].phiNode = nullptr;
      auto *Instr = dyn_cast<Instruction>(Phi->getOperand(0));
      Ctx.PhiLinks[Phi->getParent()] = Ctx.PhiLinks[Instr->getParent()];
      Phi->eraseFromParent();
    }
  } while (Changed);
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
      auto memLambda = [&V, &STR, &HasWrite, &AT, &EM, &EMNode](
          Instruction &I, MemoryLocation &&Loc, unsigned, AccessInfo, AccessInfo W) {
        if (HasWrite || W == AccessInfo::No)
          return;
        auto InstEM = AT.find(Loc);
        assert(InstEM && "alias tree node is empty");
        auto *InstNode = InstEM->getAliasNode(AT);
        if (InstEM->getTopLevelParent() != EM && !STR.isUnreachable(EMNode, InstNode)) {
          HasWrite = true;
        } else if (InstEM->getTopLevelParent() == EM) {
          if (auto *LI = dyn_cast<LoadInst>(&I))
            if (LI->getPointerOperand() != V)
              HasWrite = true;
          if (auto *SI = dyn_cast<StoreInst>(&I))
            if (SI->getPointerOperand() != V)
              HasWrite = true;
        }
      };
      auto unknownMemLambda = [&HasWrite, &AT, &STR, &EMNode](
          Instruction &I, AccessInfo, AccessInfo W) {
        if (HasWrite || W == AccessInfo::No)
          return;
        auto *InstEM = AT.findUnknown(&I);
        if (!STR.isEqual(InstEM, EMNode) && !STR.isUnreachable(InstEM, EMNode))
          HasWrite = true;
      };
      for_each_memory(Inst, TLI, memLambda, unknownMemLambda);
      if (HasWrite)
        return false;
    }
  }
  return true;
}

static bool createDbgInfo(ScalarizerContext &Ctx,
    DIType *DIT, AliasTree &AT,
    SmallVectorImpl<Metadata *> &MDs) {
  if (!Ctx.DbgVar)
    return false;
  auto &DL = Ctx.F.getParent()->getDataLayout();
  auto TypeSize = DL.getTypeStoreSize(Ctx.V->getType()->getPointerElementType());
  auto EM = AT.find(MemoryLocation(Ctx.V, LocationSize::precise(TypeSize)));
  if (EM == nullptr)
    return false;
  auto *RawDIMem = getRawDIMemoryIfExists(
      *EM,
      Ctx.F.getContext(), DL,
      AT.getDomTree());
  if (!RawDIMem)
    return false;
  auto *Scope = dyn_cast<DIScope>(Ctx.L->getStartLoc()->getScope());
  auto *NewVar = Ctx.DIB->createAutoVariable(
      Scope, "deref." + Ctx.DbgVar->getName().str(),
          Ctx.DbgVar->getFile(), Ctx.DbgVar->getLine(),
      DIT, false,
      DINode::FlagZero);
  auto *Node = DINode::get(Ctx.F.getContext(), {RawDIMem, NewVar});
  MDs.push_back(Node);
  Ctx.DbgVar = NewVar;
  return true;
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
        LoopAttr.hasAttr(*L, Attribute::ReturnsTwice) ||
        LoopAttr.hasAttr(*L, AttrKind::AlwaysReturn))
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
      auto DIB = new DIBuilder(*F.getParent());
      auto Ctx = ScalarizerContext(Val, F, L, DIB);
      if (hasVolatileInstInLoop(Ctx) ||
          !analyzeAliasTree(Val, AT, L, TLI)) {
        ValueTraitMapping.erase(Val);
        continue;
      }
      SmallVector<DIMemoryLocation, 4> DILocs;
      Optional<DIMemoryLocation> DIM;
      if (isa<AllocaInst>(Ctx.V) || isa<GlobalVariable>(Ctx.V)) {
        DIM = findMetadata(Ctx.V, DILocs);
      } else {
        bool HasDbgInLoop = false;
        for (auto *BB : L->getBlocks())
          for (auto &I : BB->getInstList())
            if (auto *DVI = dyn_cast<DbgValueInst>(&I))
              if (DVI->getValue() == Ctx.V) {
                HasDbgInLoop = true;
                break;
              }
        if (!HasDbgInLoop)
          DIM = findMetadata(Ctx.V, {&L->getHeader()->front()}, AT.getDomTree(), DILocs);
      }
      if (!DIM || !DIM->isValid())
        continue;
      Ctx.DbgVar = DIM->Var;
      Ctx.DbgLoc = DIM->Loc;
      bool InsertedDI = false;
      auto *DerivedType = dyn_cast<DIDerivedType>(Ctx.DbgVar->getType());
      if (DerivedType && Val->getType()->isPointerTy())
        InsertedDI = createDbgInfo(Ctx, DerivedType->getBaseType(), AT, MDsToAttach);
      else
        InsertedDI = createDbgInfo(Ctx, Ctx.DbgVar->getType(), AT, MDsToAttach);
      if (!InsertedDI)
        continue;
      if (!Ctx.DbgLoc)
        Ctx.DbgLoc = DILocation::get(
          F.getContext(),
          DIM->Var->getLine(),
          0,
          Ctx.DbgVar->getScope());
      insertLoadInstructions(Ctx);
      insertPhiNodes(Ctx, L->getLoopPreheader(), true);
      DenseSet<BasicBlock *> Processed;
      handleLoads(Ctx, L->getLoopPreheader(), Processed, true);
      fillPhiNodes(Ctx);
      deleteRedundantPhiNodes(Ctx);
      insertStoreInstructions(Ctx);
    }
    for (auto &Pair: ValueTraitMapping) {
      Pair.getSecond()->unset<trait::NoPromotedScalar>();
    }
  });
  if (!MDsToAttach.empty()) {
    auto *MappingNode = DINode::get(F.getContext(), MDsToAttach);
    F.setMetadata("alias.tree.mapping", MappingNode);
  }
  return false;
}
