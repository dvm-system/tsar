#include "tsar/Transform/IR/PointerReduction.h"
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

#undef DEBUG_TYPE
#define DEBUG_TYPE "ptr-red"

using namespace tsar;
using namespace llvm;

char PointerReductionPass::ID = 0;

INITIALIZE_PASS_BEGIN(PointerReductionPass, "ptr-red",
                      "Pointer Reduction Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass);
INITIALIZE_PASS_DEPENDENCY(LoopAttributesDeductionPass);
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass);
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass);
INITIALIZE_PASS_END(PointerReductionPass, "ptr-red",
                    "Pointer Reduction Pass", false, false)

void PointerReductionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<LoopAttributesDeductionPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

struct phiNodeLink {
  PHINode *phiNode;
  phiNodeLink *parent;

  explicit phiNodeLink(phiNodeLink *node) : phiNode(nullptr), parent(node) {}

  explicit phiNodeLink(PHINode *phi) : phiNode(phi), parent(nullptr) {}

  PHINode *getPhi() {
    if (phiNode) {
      return phiNode;
    }
    return parent->getPhi();
  }
};

struct PtrRedContext {
  explicit PtrRedContext(Value *v, Function &f, Loop *l, bool changed)
      : V(v), DbgVar(), DbgLoc(), F(f), L(l), ValueChanged(changed) {}

  Value *V;
  DILocalVariable *DbgVar;
  DILocation *DbgLoc;
  Function &F;
  Loop *L;
  SmallVector<LoadInst *, 2> InsertedLoads;
  DenseMap<BasicBlock *, phiNodeLink *> PhiLinks;
  DenseSet<PHINode *> UniqueNodes;
  DenseMap<BasicBlock *, Instruction *> LastInstructions;
  DenseSet<BasicBlock *> ChangedLastInst;
  bool ValueChanged;
};

bool hasVolatileLoadInstInLoop(Value *V, Loop *L) {
  for (auto *User : V->users()) {
    if (auto *SI = dyn_cast<LoadInst>(User)) {
      if (L->contains(SI) && SI->isVolatile()) {
        return true;
      }
    }
  }
  return false;
}

bool validateValue(PtrRedContext &Ctx) {
  if (dyn_cast<GEPOperator>(Ctx.V)) {
    return false;
  }
  for (auto *User : Ctx.V->users()) {
    auto *GEP = dyn_cast<GetElementPtrInst>(User);
    auto *Call = dyn_cast<CallInst>(User);
    auto *Store = dyn_cast<StoreInst>(User);
    if (Ctx.ValueChanged && Store && Ctx.L->contains(Store)) {
      return false;
    }
    if (GEP && Ctx.L->contains(GEP)) {
      return false;
    }
    if (Call) {
      DenseSet<BasicBlock *> visited;
      if (Ctx.L->contains(Call->getParent()) && Call->getParent() != Ctx.L->getExitingBlock()) {
        return false;
      }
    }
  }
  return true;
}

void insertDbgValueCall(PtrRedContext &ctx, Instruction *Inst, const DebugLoc &Loc, Instruction *InsertBefore, bool add) {
  auto *DIB = new DIBuilder(*ctx.F.getParent());

  if (add) {
    Inst->setDebugLoc(Loc);
  }
  DIB->insertDbgValueIntrinsic(Inst, ctx.DbgVar, DIExpression::get(ctx.F.getContext(), {}), Loc, InsertBefore);
}

void insertLoadInstructions(PtrRedContext &ctx) {
  auto *BeforeInstr = new LoadInst(ctx.V->getType()->getPointerElementType(), ctx.V, "load." + ctx.V->getName(), &ctx.L->getLoopPredecessor()->back());
  ctx.InsertedLoads.push_back(BeforeInstr);

  auto *insertBefore = &ctx.L->getLoopPredecessor()->back();
  insertDbgValueCall(ctx, BeforeInstr, ctx.DbgLoc, insertBefore, true);
  if (ctx.ValueChanged) {
    auto BeforeInstr2 = new LoadInst(BeforeInstr->getType(), BeforeInstr, "load.ptr." + ctx.V->getName(), (Instruction *) nullptr);
    BeforeInstr2->insertAfter(BeforeInstr);
    ctx.InsertedLoads.push_back(BeforeInstr2);
  }
}

void insertStoreInstructions(PtrRedContext &ctx) {
  SmallVector<BasicBlock *, 8> ExitBlocks;
  ctx.L->getExitBlocks(ExitBlocks);
  for (auto *BB : ExitBlocks) {
    auto storeVal = ctx.ValueChanged ? ctx.InsertedLoads.front() : ctx.V;
    new StoreInst(ctx.LastInstructions[BB], storeVal, BB->getFirstNonPHI());
  }
}

void getAllStoreOperands(Instruction *Inst, DenseMap<StoreInst *, Instruction *> &Stores) {
  for (auto *User : Inst->users()) {
    if (auto *ChildInst = dyn_cast<Instruction>(User)) {
      for (auto *ChildUser : ChildInst->users()) {
        if (auto *Store = dyn_cast<StoreInst>(ChildUser)) {
          Stores[Store] = ChildInst;
          break;
        }
      }
    }
  }
}

void handleLoadsInBB(BasicBlock *BB, PtrRedContext &ctx) {
  SmallVector<Instruction *, 16> loads;
  DenseMap<StoreInst *, Instruction *> stores;
  for (auto &Instr : BB->getInstList()) {
    if (auto *Load = dyn_cast<LoadInst>(&Instr)) {
      if (Load->getPointerOperand() != ctx.V) {
        continue;
      }
      Instruction *lastVal = ctx.LastInstructions[BB];
      if (Load->user_empty()) {
        continue;
      }
      auto replaceAllLoadUsers = [&](LoadInst *Load, Instruction *ReplaceWith) {
        DenseMap<StoreInst *, Instruction *> storeInstructions;
        getAllStoreOperands(Load, storeInstructions);
        for (auto &Pair : storeInstructions) {
          ctx.LastInstructions[Pair.second->getParent()] = Pair.second;
          ctx.ChangedLastInst.insert(Pair.second->getParent());
        }
        stores.insert(storeInstructions.begin(), storeInstructions.end());
        Load->replaceAllUsesWith(ReplaceWith);
      };
      if (!ctx.ValueChanged) {
        replaceAllLoadUsers(Load, lastVal);
      } else {
        for (auto *user : Load->users()) {
          if (auto *LoadChild = dyn_cast<LoadInst>(user)) {
            replaceAllLoadUsers(LoadChild, lastVal);
            loads.push_back(LoadChild);
          }
        }
        replaceAllLoadUsers(Load, ctx.InsertedLoads.front());
      }
      loads.push_back(Load);
    }
  }
  for (auto &Pair : stores) {
    insertDbgValueCall(ctx, Pair.second, Pair.first->getDebugLoc(), Pair.first, false);
  }
  for (auto *Load : loads) {
    Load->dropAllReferences();
    Load->eraseFromParent();
  }
  for (auto &Pair : stores) {
    Pair.first->dropAllReferences();
    Pair.first->eraseFromParent();
  }
  if (pred_size(BB) == 1 && ctx.ChangedLastInst.find(BB) == ctx.ChangedLastInst.end()) {
    ctx.LastInstructions[BB] = ctx.LastInstructions[BB->getSinglePredecessor()];
  }
}

void handleLoads(
        PtrRedContext &ctx,
        BasicBlock *BB,
        DenseSet<BasicBlock *> &completedBlocks,
        bool init = false) {
  if (completedBlocks.find(BB) != completedBlocks.end()) {
    return;
  }

  if (!init) {
    handleLoadsInBB(BB, ctx);
  }
  completedBlocks.insert(BB);
  for (auto *Succ : successors(BB)) {
    if (ctx.L->contains(Succ)) {
      handleLoads(ctx, Succ, completedBlocks);
    }
  }
}

void freeLinks(DenseMap<BasicBlock *, phiNodeLink *> &phiLinks) {
  for (auto It : phiLinks) {
    delete It.second;
  }
}

void insertPhiNodes(PtrRedContext &ctx, BasicBlock *BB, bool init = false) {
  if (ctx.PhiLinks.find(BB) != ctx.PhiLinks.end()) {
    return;
  }
  bool needsCreate = false;
  if (pred_size(BB) == 1 && !init) {
    if (ctx.PhiLinks.find(BB->getSinglePredecessor()) != ctx.PhiLinks.end()) {
      auto *parentLink = ctx.PhiLinks.find(BB->getSinglePredecessor())->second;
      ctx.PhiLinks[BB] = new phiNodeLink(parentLink);
    } else {
      needsCreate = true;
    }
  } else if (!init) {
    needsCreate = true;
  }
  if (needsCreate) {
    auto *phi = PHINode::Create(ctx.InsertedLoads.back()->getType(), 0,
                                "phi." + BB->getName(), &BB->front());
    insertDbgValueCall(ctx, phi, ctx.DbgLoc, BB->getFirstNonPHI(), true);
    ctx.PhiLinks[BB] = new phiNodeLink(phi);
    ctx.UniqueNodes.insert(phi);
  }
  for (auto *Succ : successors(BB)) {
    insertPhiNodes(ctx, Succ);
  }
  // all nodes and links are created at this point and BB = loop predecessor
  if (init) {
    ctx.LastInstructions[BB] = ctx.InsertedLoads.back();
    for (auto &P : ctx.PhiLinks) {
      ctx.LastInstructions[P.getFirst()] = P.getSecond()->getPhi();
    }
  }
}

void fillPhiNodes(PtrRedContext &ctx) {
  for (auto *Phi : ctx.UniqueNodes) {
    auto *BB = Phi->getParent();
    for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
      if (ctx.LastInstructions.find(*Pred) != ctx.LastInstructions.end()) {
        Phi->addIncoming(ctx.LastInstructions[*Pred], *Pred);
      } else {
        auto *load = new LoadInst(ctx.V->getType()->getPointerElementType(), ctx.V, "dummy.load." + ctx.V->getName(), *Pred);
        Phi->addIncoming(load, *Pred);
      }
    }
  }
}

void deleteRedundantPhiNodes(PtrRedContext &ctx) {
  for (auto *Phi : ctx.UniqueNodes) {
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
      ctx.UniqueNodes.erase(Phi);
      ctx.PhiLinks[Phi->getParent()]->phiNode = nullptr;
      auto *Instr = dyn_cast<Instruction>(operand);
      ctx.PhiLinks[Phi->getParent()]->parent = ctx.PhiLinks[Instr->getParent()];
      Phi->eraseFromParent();
    }
  }
}

bool analyzeAliasTree(Value *V, AliasTree &AT, Loop *L, TargetLibraryInfo &TLI) {
  auto STR = SpanningTreeRelation<AliasTree *>(&AT);
  auto *EM = AT.find(MemoryLocation(V));
  for (auto *BB : L->getBlocks()) {
    for (auto &Inst : BB->getInstList()) {
      // find where the value is defined
      if (dyn_cast<Value>(&Inst) == V) {
        return false;
      }

      bool writesToV = false;
      auto memLambda = [&STR, &V, &writesToV, &AT, &EM](Instruction &I, MemoryLocation &&Loc,
                                                        unsigned, AccessInfo, AccessInfo IsWrite) {
        if (writesToV || IsWrite == AccessInfo::No || Loc.Ptr == V) {
          return;
        }
        auto instEM = AT.find(Loc);
        if (EM && instEM && !STR.isUnreachable(EM->getAliasNode(AT), instEM->getAliasNode(AT))) {
          writesToV = true;
        }
      };
      auto unknownMemLambda = [&writesToV, &AT, &STR, &EM](Instruction &I, AccessInfo, AccessInfo W) {
        if (writesToV || W == AccessInfo::No) {
          return;
        }
        auto *instEM = AT.findUnknown(&I);
        if (EM && instEM && !STR.isUnreachable(instEM, EM->getAliasNode(AT))) {
          writesToV = true;
        }
      };
      for_each_memory(Inst, TLI, memLambda, unknownMemLambda);
      if (writesToV) {
        return false;
      }
    }
  }
  return true;
}

bool PointerReductionPass::runOnFunction(Function &F) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &LoopAttr = getAnalysis<LoopAttributesDeductionPass>();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);

  // 1. Find memory that was marked anti after the first step;
  // 2. Check if this is a pointer that is dereferenced inside the loop;
  // 3. Copy its value at the preheader and store it back after the loop;
  // 4. Replace all load/store instructions in the loop's body
  //    with corresponding operations with this copied memory;
  // 5. Check if this helped; TODO

  for_each_loop(LI, [this, &TraitPool, &LoopAttr, &AT, &TLI, &F](Loop *L) {
    if (!LoopAttr.hasAttr(*L, Attribute::NoUnwind) || LoopAttr.hasAttr(*L, Attribute::Returned)) {
      return;
    }
    auto &Pool = TraitPool[L->getLoopID()];
    SmallDenseSet<Value *> Values;
    if (!Pool) {
      Pool = std::make_unique<DIMemoryTraitRegionPool>();
    } else {
      for (auto &T : *Pool) {
        if (T.is<trait::Anti>() || T.is<trait::Flow>() || T.is<trait::Output>()) {
          for (auto &V : *T.getMemory()) {
            if (V.pointsToAliveValue() && !dyn_cast<UndefValue>(V)) {
              for (auto *User : V->users()) {
                if (auto *LI = dyn_cast<LoadInst>(User)) {
                  bool hasLoadUses = false;
                  for (auto *LoadChild : LI->users()) {
                    if (dyn_cast<LoadInst>(LoadChild)) {
                      hasLoadUses = true;
                      break;
                    }
                  }
                  if (hasLoadUses) {
                    continue;
                  }
                  if (L->contains(LI)) {
                    Values.insert(LI->getPointerOperand());
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }
    DenseSet<Value *> toDelete;
    for (auto *Val : Values) {
      if (auto *Load = dyn_cast<LoadInst>(Val)) {
        toDelete.insert(Load->getPointerOperand());
      }
    }
    for (auto *el : toDelete) {
      Values.erase(el);
    }
    for (auto *Val : Values) {
      auto *V = Val;
      if (auto *Load = dyn_cast<LoadInst>(Val)) {
        V = Load->getPointerOperand();
      }
      auto ctx = PtrRedContext(V, F, L, Val != V);
      if (!validateValue(ctx)) {
        continue;
      }
      if (hasVolatileLoadInstInLoop(V, L)) {
        continue;
      }
      if (!analyzeAliasTree(V, AT, L, TLI)) {
        continue;
      }
      // find dbg.value call for V and save it for adding debug information later
      for (auto &BB : F.getBasicBlockList()) {
        for (auto &Inst : BB.getInstList()) {
          if (auto *Dbg = dyn_cast<DbgValueInst>(&Inst)) {
            if (Dbg->getValue() == V) {
              ctx.DbgLoc = Dbg->getDebugLoc();
              ctx.DbgVar = Dbg->getVariable();
            }
          } else if (auto *Dbg = dyn_cast<DbgDeclareInst>(&Inst)) {
            if (Dbg->getAddress() == V) {
              ctx.DbgLoc = Dbg->getDebugLoc();
              ctx.DbgVar = Dbg->getVariable();
            }
          }
        }
      }
      //there are no dbg.value or dbg.declare calls for global variables
      if (dyn_cast<GlobalValue>(ctx.V)) {
        auto *DIB = new DIBuilder(*ctx.F.getParent());
        auto *scope = dyn_cast<DIScope>(L->getStartLoc()->getScope());
        ctx.DbgVar = DIB->createAutoVariable(scope, "sapfor.da", nullptr, 0, nullptr, false, DINode::FlagZero | DINode::FlagArtificial);
        ctx.DbgLoc = L->getStartLoc();
      }

      insertLoadInstructions(ctx);
      insertPhiNodes(ctx, L->getLoopPredecessor(), true);
      DenseSet<BasicBlock *> processedBlocks;
      handleLoads(ctx, L->getLoopPredecessor(), processedBlocks, true);
      fillPhiNodes(ctx);
      deleteRedundantPhiNodes(ctx);
      insertStoreInstructions(ctx);
      freeLinks(ctx.PhiLinks);
    }
  });
  return false;
}
