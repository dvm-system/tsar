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
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include <tsar/Analysis/Memory/Utils.h>
#include "tsar/Support/IRUtils.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DIBuilder.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "ptr2reg"

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
  PHINode *PhiNode;
  PhiNodeLink *Parent;

  PhiNodeLink() = default;

  explicit PhiNodeLink(PhiNodeLink *Node) : PhiNode(nullptr), Parent(Node) {}

  explicit PhiNodeLink(PHINode *Phi) : PhiNode(Phi), Parent(nullptr) {}

  bool hasValue() const { return PhiNode || (Parent && Parent->hasValue()); }

  PHINode *getPhi() const {
    if (PhiNode)
      return PhiNode;
    return Parent->getPhi();
  }
};

struct ScalarizerContext {
  explicit ScalarizerContext(Value *V, Loop *L, DIBuilder *DIB)
      : V(V), L(L), DIB(DIB), PhiLinks{} {
    for (auto *BB : L->getBlocks())
      PhiLinks.insert({BB, PhiNodeLink()});
  }

  Value *V{nullptr};
  DILocalVariable *DbgVar{nullptr};
  DILocation *DbgLoc{nullptr};
  Loop *L{nullptr};
  LoadInst *InsertedLoad{nullptr};
  DenseMap<BasicBlock *, PhiNodeLink> PhiLinks;
  DenseSet<PHINode *> UniqueNodes;
  DenseMap<BasicBlock *, Value *> LastValues;
  DIBuilder *DIB{nullptr};
};
}

char PointerScalarizerPass::ID = 0;

INITIALIZE_PASS_BEGIN(PointerScalarizerPass, "ptr2reg",
    "Promote Pointer To Register", false, false)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
INITIALIZE_PASS_DEPENDENCY(LoopAttributesDeductionPass);
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass);
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass);
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass);
INITIALIZE_PASS_END(PointerScalarizerPass, "ptr2reg",
    "Promote Pointer To Register", false, false)

FunctionPass * llvm::createPointerScalarizerPass() {
  return new PointerScalarizerPass();
}

static inline void insertDbgValueCall(ScalarizerContext &Ctx, Value *V,
                                      Instruction *InsertBefore, bool Add) {
  Ctx.DIB->insertDbgValueIntrinsic(V, Ctx.DbgVar,
                                   DIExpression::get(V->getContext(), {}),
                                   Ctx.DbgLoc, InsertBefore);
}

static void insertLoadInstructions(ScalarizerContext &Ctx) {
  auto PreheaderBB{Ctx.L->getLoopPreheader()};
  auto *BeforeInstr{new LoadInst(getPointerElementType(*Ctx.V),
                                 Ctx.V, "load." + Ctx.V->getName(),
                                 &PreheaderBB->back())};
  Ctx.InsertedLoad = BeforeInstr;
  auto *InsertBefore = &Ctx.L->getLoopPreheader()->back();
  insertDbgValueCall(Ctx, BeforeInstr, InsertBefore, true);
  Ctx.LastValues[PreheaderBB] = Ctx.InsertedLoad;
}

static inline void insertStoreInstructions(ScalarizerContext &Ctx) {
  SmallVector<BasicBlock *, 8> ExitBlocks;
  Ctx.L->getExitBlocks(ExitBlocks);
  for (auto *BB : ExitBlocks) {
    auto SI{new StoreInst(Ctx.LastValues[BB], Ctx.V, BB->getFirstNonPHI())};
    insertDbgValueCall(Ctx, SI->getValueOperand(), SI, false);
  }
}

static void handleMemoryAccess(BasicBlock *BB, ScalarizerContext &Ctx) {
  SmallVector<Instruction *, 16> Loads;
  DenseMap<StoreInst *, Value *> Stores;
  SmallVector<Instruction *, 16> ToDelete;
  Value *LastValue{Ctx.PhiLinks[BB].getPhi()};
  if (cast<Instruction>(LastValue)->getParent() != BB)
    LastValue = Ctx.LastValues[BB->getSinglePredecessor()];
  for (auto &Instr : BB->getInstList()) {
    if (auto *SI{dyn_cast<StoreInst>(&Instr)};
        SI && SI->getPointerOperand() == Ctx.V) {
      ToDelete.push_back(SI);
      LastValue = SI->getValueOperand();
      insertDbgValueCall(Ctx, SI->getValueOperand(), SI, false);
    } else if (auto *LI{dyn_cast<LoadInst>(&Instr)};
               LI && LI->getPointerOperand() == Ctx.V && !LI->user_empty()) {
      LI->replaceAllUsesWith(LastValue);
      ToDelete.push_back(LI);
    }
  }
  for (auto *I : ToDelete) {
    I->dropAllReferences();
    I->eraseFromParent();
  }
  Ctx.LastValues[BB] = LastValue;
}

static void handleLoads(ScalarizerContext &Ctx, BasicBlock *BB,
                        SmallPtrSetImpl<BasicBlock *> &Completed,
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

static void insertPhiNodes(ScalarizerContext &Ctx, BasicBlock *BB) {
  if (Ctx.L->contains(BB) && Ctx.PhiLinks[BB].hasValue())
    return;
  bool NeedsCreate{false};
  if (pred_size(BB) == 1) {
    auto *Pred{BB->getSinglePredecessor()};
    if (Ctx.L->contains(Pred) && Ctx.PhiLinks[Pred].hasValue())
      Ctx.PhiLinks[BB] = Ctx.PhiLinks[Pred];
    else
      NeedsCreate = true;
  } else {
    NeedsCreate = true;
  }
  if (NeedsCreate) {
    auto *Phi{PHINode::Create(Ctx.InsertedLoad->getType(), 0,
                              Ctx.V->getName() + "." + BB->getName(),
                              &BB->front())};
    Ctx.PhiLinks[BB] = PhiNodeLink(Phi);
    Ctx.UniqueNodes.insert(Phi);
  }
  for (auto *Succ : successors(BB))
    if (Ctx.L->contains(Succ))
      insertPhiNodes(Ctx, Succ);
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
    for (auto *Phi : Ctx.UniqueNodes) {
      bool SameOperands = true;
      auto *Op = Phi->getOperand(0);
      for (auto *OtherOp : Phi->operand_values())
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
      Ctx.PhiLinks[Phi->getParent()].PhiNode = nullptr;
      auto *Instr = dyn_cast<Instruction>(Phi->getOperand(0));
      Ctx.PhiLinks[Phi->getParent()] = Ctx.PhiLinks[Instr->getParent()];
      Phi->eraseFromParent();
    }
  } while (Changed);
}

static bool hasAmbiguousAccessesOrReadonly(Value *V, AliasTree &AT, Loop *L,
                                           TargetLibraryInfo &TLI) {
  auto STR = SpanningTreeRelation<AliasTree *>(&AT);
  auto *EM = AT.find(MemoryLocation(V, LocationSize(0)));
  if (!EM)
    return false;
  EM = EM->getTopLevelParent();
  // TODO(kaniandr@gmail.com): to promote readonly variables it is necessary
  // add corresponding analysis for readonly registers in the
  // DIDependenceAnalysisPass pass.
  bool HasWrite{false};
  for (auto *BB : L->getBlocks()) {
    for (auto &Inst : BB->getInstList()) {
      bool HasAccess = false;
      auto *EMNode = EM->getAliasNode(AT);
      auto memLambda = [BB, &V, &STR, &AT, &EM, &EMNode, &HasAccess,
                        &HasWrite](Instruction &I, MemoryLocation &&Loc,
                                   unsigned, AccessInfo R, AccessInfo W) {
        if (HasAccess || (W == AccessInfo::No && R == AccessInfo::No))
          return;
        auto InstEM = AT.find(Loc);
        assert(InstEM && "Memory location must not be null!");
        auto *InstNode = InstEM->getAliasNode(AT);
        if (InstEM->getTopLevelParent() != EM &&
            !STR.isUnreachable(EMNode, InstNode)) {
          HasAccess = true;
        } else if (InstEM->getTopLevelParent() == EM) {
          if (auto *LI = dyn_cast<LoadInst>(&I)) {
            if (LI->getPointerOperand() != V || LI->isVolatile())
              HasAccess = true;
          } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
            HasWrite = true;
            if (SI->getPointerOperand() != V || SI->isVolatile())
              HasAccess = true;
          } else {
            HasAccess = true;
          }
        }
      };
      auto unknownMemLambda = [&HasAccess, &AT, &STR, &EMNode](
                                  Instruction &I, AccessInfo R, AccessInfo W) {
        if (HasAccess || (W == AccessInfo::No && R == AccessInfo::No))
          return;
        auto *InstEM = AT.findUnknown(&I);
        if (!STR.isUnreachable(InstEM, EMNode))
          HasAccess = true;
      };
      for_each_memory(Inst, TLI, memLambda, unknownMemLambda);
      if (HasAccess)
        return true;
    }
  }
  return !HasWrite;
}

bool PointerScalarizerPass::runOnFunction(Function &F) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &LoopAttr = getAnalysis<LoopAttributesDeductionPass>();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto MDsToAttach = SmallVector<Metadata *, 8>();
  DIBuilder DIB{*F.getParent()};
  for_each_loop(LI, [&TraitPool, &LoopAttr, &AT, &TLI, &F, &DIB,
                     &MDsToAttach](Loop *L) {
    // 1. Find memory that was marked anti/flow/output.
    // 2. Check that this memory is accessed inside the loop.
    // 3. If possible, copy its value at the preheader and store it back after
    // the exit block(s).
    // 4. Replace all load/store instructions and their users in the loop's body
    //    with corresponding operations with the copied memory.
    // 5. Map inserted instructions with the original value using DI nodes.
    if (!L->getLoopID() || !L->getLoopPreheader() ||
        !LoopAttr.hasAttr(*L, Attribute::NoUnwind) ||
        LoopAttr.hasAttr(*L, Attribute::ReturnsTwice) ||
        !LoopAttr.hasAttr(*L, AttrKind::AlwaysReturn))
      return;
    auto PoolItr{TraitPool.find(L->getLoopID())};
    if (PoolItr == TraitPool.end())
      return;
    SmallPtrSet<Value *, 8> Disabled;
    SmallDenseSet<Value *> Values;
    SmallDenseMap<Value *, SmallVector<DIMemoryTrait *, 1>> ToPromote;
    auto disableAll = [L, &Disabled](auto &T) {
      for (auto &V : *T.getMemory())
        if (V.pointsToAliveValue() && !isa<UndefValue>(V) &&
            any_of_user_insts(*V, [L](User *U) {
              return isa<Instruction>(U) && L->contains(cast<Instruction>(U));
            }))
          Disabled.insert(V);
    };
    for (auto &T : *PoolItr->get<tsar::Pool>()) {
      if (!T.is_any<trait::Anti, trait::Flow, trait::Output>())
        continue;
      Value *SingleV{nullptr};
      for (auto &V : *T.getMemory()) {
        if (!V.pointsToAliveValue() || isa<UndefValue>(V) ||
            !any_of_user_insts(*V, [L](User *U) {
              return isa<Instruction>(U) && L->contains(cast<Instruction>(U));
            }))
          continue;
        if (Disabled.contains(V) || SingleV || !getPointerElementType(*V) ||
            (isa<Instruction>(V) && L->contains(cast<Instruction>(V))) ||
            [L, V]() {
              for (auto *BB : L->getBlocks())
                for (auto &I : BB->getInstList())
                  if (auto *DVI{dyn_cast<DbgVariableIntrinsic>(&I)};
                      DVI && DVI->getVariableLocationOp(0) == V)
                    return true;
              return false;
            }() ||
            hasAmbiguousAccessesOrReadonly(V, AT, L, TLI)) {
          SingleV = nullptr;
          disableAll(T);
          break;
        }
        SingleV = V;
      }
      if (SingleV)
        ToPromote.try_emplace(SingleV).first->second.push_back(&T);
    }
    for (auto &&[V, Traits] : ToPromote) {
      if (Disabled.contains(V))
        continue;
      auto Ctx = ScalarizerContext(V, L, &DIB);
      auto *DITy{createStubType(
          *F.getParent(), cast<PointerType>(V->getType())->getAddressSpace(),
          DIB)};
      auto *Scope{dyn_cast<DIScope>(Ctx.L->getStartLoc()->getScope())};
      StringRef Name{V->getName()};
      DIFile *File{nullptr};
      unsigned Line{0};
      if (auto *DIEM{dyn_cast<DIEstimateMemory>(Traits.front()->getMemory())}) {
        auto *Var{DIEM->getVariable()};
        Name = Var->getName();
        File = Var->getFile();
        Line = Var->getLine();
      }
      auto *NewVar{Ctx.DIB->createAutoVariable(Scope, ("deref." + Name).str(),
                                               File, Line, DITy, false,
                                               DINode::FlagZero)};
      for (auto *T : Traits) {
        auto *Node{DINode::get(F.getContext(),
                               {static_cast<Metadata *>(const_cast<MDNode *>(
                                    (T->getMemory()->getAsMDNode()))),
                                NewVar})};
        MDsToAttach.push_back(Node);
      }
      Ctx.DbgVar = NewVar;
      Ctx.DbgLoc = DILocation::get(F.getContext(), NewVar->getLine(), 0,
                                   Ctx.DbgVar->getScope());
      insertLoadInstructions(Ctx);
      insertPhiNodes(Ctx, L->getHeader());
      SmallPtrSet<BasicBlock *, 8> Processed;
      handleLoads(Ctx, L->getLoopPreheader(), Processed, true);
      fillPhiNodes(Ctx);
      deleteRedundantPhiNodes(Ctx);
      insertStoreInstructions(Ctx);
    }
  });
  if (!MDsToAttach.empty()) {
    auto *MappingNode = DINode::get(F.getContext(), MDsToAttach);
    F.setMetadata("alias.tree.mapping", MappingNode);
  }
  return false;
}
