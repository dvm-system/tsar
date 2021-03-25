//===- PointerScalarizer.cpp - Promoting certain values after SROA  --------- *- C++ -*-===//
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
  explicit ScalarizerContext(Value *V, Function &F,
      Loop *L, DIBuilder *DIB, bool Changed)
  : V(V), DbgVar(), DbgLoc(), F(F), L(L), ValueChanged(Changed), DIB(DIB) {}

  Value *V;
  DIVariable *DbgVar;
  DILocation *DbgLoc;
  Function &F;
  Loop *L;
  SmallVector<LoadInst *, 2> InsertedLoads;
  DenseMap<BasicBlock *, phiNodeLink *> PhiLinks;
  DenseSet<PHINode *> UniqueNodes;
  DenseMap<BasicBlock *, Instruction *> LastInstructions;
  DenseSet<BasicBlock *> ChangedLastInst;
  bool ValueChanged;
  DIBuilder *DIB;
};

bool hasVolatileLoadInstInLoop(Value *V, Loop *L) {
  for (auto *User : V->users()) {
    auto *LI = dyn_cast<LoadInst>(User);
    if (!LI)
      continue;
    if (L->contains(LI) && LI->isVolatile())
      return true;
  }
  return false;
}

bool validateValue(const ScalarizerContext &Ctx) {
  if (dyn_cast<GEPOperator>(Ctx.V))
    return false;
  for (auto *User : Ctx.V->users()) {
    auto *GEP = dyn_cast<GetElementPtrInst>(User);
    auto *Call = dyn_cast<CallInst>(User);
    auto *Store = dyn_cast<StoreInst>(User);
    if (Ctx.ValueChanged && Store && Ctx.L->contains(Store))
      return false;
    if (GEP && Ctx.L->contains(GEP))
      return false;
    if (Call) {
      if (Ctx.L->contains(Call->getParent()) && Call->getParent() != Ctx.L->getExitingBlock())
        return false;
    }
  }
  return true;
}

void insertDbgValueCall(ScalarizerContext &Ctx,
    Instruction *I, Instruction *InsertBefore, bool Add) {
  if (Add)
    I->setDebugLoc(Ctx.DbgLoc);
  Ctx.DIB->insertDbgValueIntrinsic(
          I,
    dyn_cast<DILocalVariable>(Ctx.DbgVar),
    DIExpression::get(Ctx.F.getContext(), {}),
          Ctx.DbgLoc, InsertBefore
  );
}

void insertLoadInstructions(ScalarizerContext &Ctx) {
  auto *BeforeInstr = new LoadInst(
    Ctx.V->getType()->getPointerElementType(),
    Ctx.V, "load." + Ctx.V->getName(),
    &Ctx.L->getLoopPredecessor()->back());
  Ctx.InsertedLoads.push_back(BeforeInstr);

  auto *insertBefore = &Ctx.L->getLoopPredecessor()->back();
  insertDbgValueCall(Ctx, BeforeInstr, insertBefore, true);
  if (Ctx.ValueChanged) {
    auto BeforeInstr2 = new LoadInst(
      BeforeInstr->getType()->getPointerElementType(),
      BeforeInstr, "load.ptr." + Ctx.V->getName(),
      &Ctx.L->getLoopPredecessor()->back());
    Ctx.InsertedLoads.push_back(BeforeInstr2);
  }
}

void insertStoreInstructions(ScalarizerContext &Ctx) {
  SmallVector<BasicBlock *, 8> ExitBlocks;
  Ctx.L->getExitBlocks(ExitBlocks);
  for (auto *BB : ExitBlocks) {
    auto storeVal = Ctx.ValueChanged ? Ctx.InsertedLoads.front() : Ctx.V;
    new StoreInst(Ctx.LastInstructions[BB], storeVal, BB->getFirstNonPHI());
  }
}

void getAllStoreOperands(LoadInst *I, DenseMap<StoreInst *, Instruction *> &Stores) {
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

void handleLoadsInBB(BasicBlock *BB, ScalarizerContext &Ctx) {
  SmallVector<Instruction *, 16> Loads;
  DenseMap<StoreInst *, Instruction *> Stores;
  auto ReplaceLoads = [&](LoadInst *Load, Instruction *ReplaceWith) {
    DenseMap<StoreInst *, Instruction *> storeInstructions;
    getAllStoreOperands(Load, storeInstructions);
    for (auto &Pair : storeInstructions) {
      Ctx.LastInstructions[Pair.second->getParent()] = Pair.second;
      Ctx.ChangedLastInst.insert(Pair.second->getParent());
    }
    Stores.insert(storeInstructions.begin(), storeInstructions.end());
    Load->replaceAllUsesWith(ReplaceWith);
  };
  for (auto &Instr : BB->getInstList()) {
    auto *Load = dyn_cast<LoadInst>(&Instr);
    if (!Load || Load->getPointerOperand() != Ctx.V || Load->user_empty())
      continue;
    Instruction *LI = Ctx.LastInstructions[BB];
    if (!Ctx.ValueChanged) {
      ReplaceLoads(Load, LI);
    } else {
      for (auto *user : Load->users())
        if (auto *LoadChild = dyn_cast<LoadInst>(user)) {
          ReplaceLoads(LoadChild, LI);
          Loads.push_back(LoadChild);
        }
      ReplaceLoads(Load, Ctx.InsertedLoads.front());
    }
    Loads.push_back(Load);
  }
  for (auto &Pair : Stores)
    insertDbgValueCall(Ctx, Pair.second, Pair.first, false);
  for (auto *Load : Loads) {
    Load->dropAllReferences();
    Load->eraseFromParent();
  }
  for (auto &Pair : Stores) {
    Pair.first->dropAllReferences();
    Pair.first->eraseFromParent();
  }
  if (pred_size(BB) == 1 && Ctx.ChangedLastInst.find(BB) == Ctx.ChangedLastInst.end())
    Ctx.LastInstructions[BB] = Ctx.LastInstructions[BB->getSinglePredecessor()];
}

void handleLoads(ScalarizerContext &Ctx,
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

void freeLinks(DenseMap<BasicBlock *, phiNodeLink *> &PhiLinks) {
  for (auto It : PhiLinks)
    delete It.second;
}

void insertPhiNodes(ScalarizerContext &Ctx, BasicBlock *BB, bool Init = false) {
  if (Ctx.PhiLinks.find(BB) != Ctx.PhiLinks.end())
    return;
  bool needsCreate = false;
  if (pred_size(BB) == 1 && !Init) {
    if (Ctx.PhiLinks.find(BB->getSinglePredecessor()) != Ctx.PhiLinks.end()) {
      auto *parentLink = Ctx.PhiLinks.find(BB->getSinglePredecessor())->second;
      Ctx.PhiLinks[BB] = new phiNodeLink(parentLink);
    } else {
      needsCreate = true;
    }
  } else if (!Init) {
    needsCreate = true;
  }
  if (needsCreate) {
    auto *phi = PHINode::Create(Ctx.InsertedLoads.back()->getType(), 0,
        "phi." + BB->getName(), &BB->front());
    insertDbgValueCall(Ctx, phi, BB->getFirstNonPHI(), true);
    Ctx.PhiLinks[BB] = new phiNodeLink(phi);
    Ctx.UniqueNodes.insert(phi);
  }
  for (auto *Succ : successors(BB))
    insertPhiNodes(Ctx, Succ);
  // all nodes and links are created at this point and BB = loop predecessor
  if (Init) {
    Ctx.LastInstructions[BB] = Ctx.InsertedLoads.back();
    for (auto &P : Ctx.PhiLinks)
      Ctx.LastInstructions[P.getFirst()] = P.getSecond()->getPhi();
  }
}

void fillPhiNodes(ScalarizerContext &Ctx) {
  for (auto *Phi : Ctx.UniqueNodes) {
    auto *BB = Phi->getParent();
    for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
      if (Ctx.LastInstructions.find(*Pred) != Ctx.LastInstructions.end()) {
        Phi->addIncoming(Ctx.LastInstructions[*Pred], *Pred);
      } else {
        auto *load = new LoadInst(Ctx.V->getType()->getPointerElementType(),
            Ctx.V, "dummy.load." + Ctx.V->getName(), *Pred);
        Phi->addIncoming(load, *Pred);
      }
    }
  }
}

void deleteRedundantPhiNodes(ScalarizerContext &Ctx) {
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

bool analyzeAliasTree(Value *V, AliasTree &AT, Loop *L, TargetLibraryInfo &TLI) {
  auto STR = SpanningTreeRelation<AliasTree *>(&AT);
  auto *EM = AT.find(MemoryLocation(V));
  for (auto *BB : L->getBlocks()) {
    for (auto &Inst : BB->getInstList()) {
      if (dyn_cast<Value>(&Inst) == V)
        return false;
      bool writesToV = false;
      auto memLambda = [&STR, &V, &writesToV, &AT, &EM](
          Instruction &I, MemoryLocation &&Loc, unsigned, AccessInfo, AccessInfo W) {
        if (writesToV || W == AccessInfo::No || Loc.Ptr == V)
          return;
        auto instEM = AT.find(Loc);
        if (EM && instEM && !STR.isUnreachable(EM->getAliasNode(AT), instEM->getAliasNode(AT)))
          writesToV = true;
      };
      auto unknownMemLambda = [&writesToV, &AT, &STR, &EM](
          Instruction &I, AccessInfo, AccessInfo W) {
        if (writesToV || W == AccessInfo::No)
          return;
        auto *instEM = AT.findUnknown(&I);
        if (EM && instEM && !STR.isUnreachable(instEM, EM->getAliasNode(AT)))
          writesToV = true;
      };
      for_each_memory(Inst, TLI, memLambda, unknownMemLambda);
      if (writesToV)
        return false;
    }
  }
  return true;
}

void handlePointerDI(ScalarizerContext &Ctx,
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
  auto *Node = DINode::get(Ctx.F.getContext(), {RawDIMem, NewVar, Ctx.DbgVar});
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
        !LoopAttr.hasAttr(*L, Attribute::NoUnwind) ||
        LoopAttr.hasAttr(*L, Attribute::Returned))
      return;
    auto &Pool = TraitPool[L->getLoopID()];
    if (!Pool)
      return;
    SmallDenseSet<Value *> Values;
    for (auto &T : *Pool) {
      if (!T.is_any<trait::Anti, trait::Flow, trait::Output>())
        continue;
      for (auto &V : *T.getMemory()) {
        if (!V.pointsToAliveValue() || isa<UndefValue>(V))
          continue;
        bool HasLoad = false, HasStore = false;
        for (auto *User : V->users()) {
          if (auto *LI = dyn_cast<LoadInst>(User))
            if (L->contains(LI))
              HasLoad = true;
          if (auto *LI = dyn_cast<StoreInst>(User))
            if (L->contains(LI))
              HasStore = true;
          if (HasLoad && HasStore) {
            Values.insert(V);
            break;
          }
        }
      }
    }
    SmallPtrSet<Value *, 8> ToDelete;
    for (auto *Val : Values)
      if (auto *Load = dyn_cast<LoadInst>(Val))
        ToDelete.insert(Load->getPointerOperand());
    for (auto *el : ToDelete)
      Values.erase(el);
    if (!Values.empty())
      for (auto &T: *Pool)
        T.unset<trait::NoPromotedScalar>();
    for (auto *Val : Values) {
      auto *V = Val;
      if (auto *Load = dyn_cast<LoadInst>(Val))
        V = Load->getPointerOperand();
      auto DIB = new DIBuilder(*F.getParent());
      auto Ctx = ScalarizerContext(V, F, L, DIB, Val != V);
      if (!validateValue(Ctx) ||
          hasVolatileLoadInstInLoop(V, L) ||
          !analyzeAliasTree(V, AT, L, TLI))
        continue;
      // find dbg.value call for V and save it for adding debug information later
      for (auto &BB : F.getBasicBlockList()) {
        for (auto &Inst : BB.getInstList()) {
          if (auto *DbgVal = dyn_cast<DbgValueInst>(&Inst)) {
            if (DbgVal->getValue() == V) {
              Ctx.DbgLoc = DbgVal->getDebugLoc();
              Ctx.DbgVar = DbgVal->getVariable();
            }
          } else if (auto *Declare = dyn_cast<DbgDeclareInst>(&Inst)) {
            if (Declare->getAddress() == V) {
              Ctx.DbgLoc = Declare->getDebugLoc();
              Ctx.DbgVar = Declare->getVariable();
            }
          }
        }
      }
      if (dyn_cast<GlobalValue>(Ctx.V)) {
        SmallVector<DIMemoryLocation, 4> DILocs;
        auto MD = findMetadata(Ctx.V, DILocs, &AT.getDomTree());
        Ctx.DbgVar = MD->Var;
        Ctx.DbgLoc = L->getStartLoc();
        handlePointerDI(Ctx, Ctx.DbgVar->getType(), AT, MDsToAttach);
      }
      auto *derivedType = dyn_cast<DIDerivedType>(Ctx.DbgVar->getType());
      if (!dyn_cast<GlobalValue>(Ctx.V) && derivedType && V->getType()->isPointerTy())
        handlePointerDI(Ctx, derivedType->getBaseType(), AT, MDsToAttach);

      insertLoadInstructions(Ctx);
      insertPhiNodes(Ctx, L->getLoopPredecessor(), true);
      DenseSet<BasicBlock *> Processed;
      handleLoads(Ctx, L->getLoopPredecessor(), Processed, true);
      fillPhiNodes(Ctx);
      deleteRedundantPhiNodes(Ctx);
      insertStoreInstructions(Ctx);
      freeLinks(Ctx.PhiLinks);
    }
  });
  if (!MDsToAttach.empty()) {
    auto *MappingNode = DINode::get(F.getContext(), MDsToAttach);
    F.setMetadata("alias.tree.mapping", MappingNode);
  }
  return false;
}
