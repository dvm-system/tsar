#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/PointerReduction.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"

#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Transform/IR/InterprocAttr.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/PassSupport.h>
#include <llvm/Transforms/Scalar.h>

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

  explicit phiNodeLink(phiNodeLink *node) : phiNode(nullptr), parent(node) { }

  explicit phiNodeLink(PHINode *phi) : phiNode(phi), parent(nullptr) { }

  PHINode *getPhi() {
    if (phiNode) {
      return phiNode;
    }
    return parent->getPhi();
  }
};

struct PtrRedContext {
  explicit PtrRedContext(Value *v, Function &f, Loop *l, bool changed)
    : V(v), F(f), L(l), ValueChanged(changed) { }

  Value *V;
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

bool hasPossibleReductionUses(Instruction *Instr) {
  const static auto opCodes = {
      BinaryOperator::Add,
      BinaryOperator::FAdd,
      BinaryOperator::Sub,
      BinaryOperator::FSub,
      BinaryOperator::Mul,
      BinaryOperator::FMul,
      BinaryOperator::UDiv,
      BinaryOperator::SDiv,
      BinaryOperator::FDiv,

  };
  for (auto *User : Instr->users()) {
    if (auto *Op = dyn_cast<Instruction>(User)) {
      for (auto opCode : opCodes) {
        if (Op->getOpcode() == opCode) {
          return true;
        }
      }
    }
  }
  return false;
}

bool hasPathInCFG(BasicBlock *From, BasicBlock *To, DenseSet<BasicBlock *> &Visited) {
  if (From == To) {
    return true;
  }
  Visited.insert(From);
  for (auto *succ : successors(From)) {
    if (Visited.find(succ) == Visited.end() && hasPathInCFG(succ, To, Visited)) {
      return true;
    }
  }
  return false;
}

bool validateValue(Value *V, Loop *L) {
  if (dyn_cast<GEPOperator>(V)) {
    return false;
  }
  if (dyn_cast<GlobalValue>(V) && !V->getType()->getPointerElementType()->isPointerTy()) {
    return false;
  }
  for (auto *User : V->users()) {
    auto *GEP = dyn_cast<GetElementPtrInst>(User);
    auto *Call = dyn_cast<CallInst>(User);

    if (GEP && L->contains(GEP)) {
      return false;
    }
    if (Call) {
      DenseSet<BasicBlock *> visited;
      auto *from = L->getHeader();
      auto *to = Call->getParent();
      return !L->contains(to) && hasPathInCFG(from, to, visited);
    }
  }
  return true;
}

void insertDbgValueCall(Instruction *Inst, Function &F, Loop *L, const DebugLoc &Loc, Instruction *InsertBefore) {
  auto *DIB = new DIBuilder(*F.getParent());

  auto *scope = dyn_cast<DIScope>(Loc->getScope());
  auto *debug = DIB->createAutoVariable(scope, Inst->getName(), nullptr, Loc->getLine() + 1, nullptr);
  Inst->setMetadata("sapfor.dbg", debug);

  DIB->insertDbgValueIntrinsic(Inst, debug, DIExpression::get(Inst->getContext(), {}), Loc, InsertBefore);
}

void insertLoadInstructions(PtrRedContext &ctx) {
  auto *BeforeInstr = new LoadInst(ctx.V, "load." + ctx.V->getName());
  BeforeInstr->insertBefore(&ctx.L->getLoopPredecessor()->back());
  ctx.InsertedLoads.push_back(BeforeInstr);

  auto *insertBefore = &ctx.L->getLoopPredecessor()->back();
  insertDbgValueCall(BeforeInstr, ctx.F, ctx.L, insertBefore->getDebugLoc(), insertBefore);
  if (ctx.ValueChanged) {
    auto BeforeInstr2 = new LoadInst(BeforeInstr, "load.ptr." + ctx.V->getName());
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

void getAllStoreOperands(Instruction *Inst, DenseSet<Instruction *> &Storage, SmallVector<Instruction *, 16> &toErase) {
  for (auto *User : Inst->users()) {
    bool hasStore = false;
    if (auto *ChildInst = dyn_cast<Instruction>(User)) {
      for (auto *ChildUser : ChildInst->users()) {
        if (auto *Store = dyn_cast<StoreInst>(ChildUser)) {
          toErase.push_back(Store);
          hasStore = true;
          break;
        }
      }
      if (hasStore) {
        Storage.insert(ChildInst);
      }
    }
  }
}

void handleLoadsInBB(BasicBlock *BB, PtrRedContext &ctx) {
  SmallVector<Instruction *, 16> toErase;
  for (auto &Instr : BB->getInstList()) {
    if (auto *Load = dyn_cast<LoadInst>(&Instr)) {
      if (Load->getPointerOperand() != ctx.V) {
        continue;
      }
      Instruction *lastVal = ctx.LastInstructions[BB];
      if (Load->user_empty()) {
        continue;
      }
      auto replaceAllLoadUsers = [&](LoadInst *Load) {
        DenseSet<Instruction *> instructions;
        getAllStoreOperands(Load, instructions, toErase);
        for (auto *Inst : instructions) {
          ctx.LastInstructions[Inst->getParent()] = Inst;
          ctx.ChangedLastInst.insert(Inst->getParent());
        }
        Load->replaceAllUsesWith(lastVal);
      };
      if (!ctx.ValueChanged) {
        replaceAllLoadUsers(Load);
      } else {
        for (auto *user : Load->users()) {
          if (auto *LoadChild = dyn_cast<LoadInst>(user)) {
            replaceAllLoadUsers(LoadChild);
            toErase.push_back(LoadChild);
          }
        }
      }
      toErase.push_back(Load);
    }
  }
  for (auto *Load : toErase) {
    Load->dropAllReferences();
    Load->eraseFromParent();
  }
  if (pred_size(BB) == 1 && ctx.ChangedLastInst.find(BB) == ctx.ChangedLastInst.end()) {
    ctx.LastInstructions[BB] = ctx.LastInstructions[BB->getSinglePredecessor()];
  }
}

void handleLoads(
    PtrRedContext &ctx,
    BasicBlock *BB,
    DenseSet<BasicBlock *> &completedBlocks,
    bool init = false)
{
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
    auto *phi = PHINode::Create(ctx.V->getType()->getPointerElementType(), 0,
        "phi." + BB->getName(), &BB->front());
     insertDbgValueCall(phi, ctx.F, ctx.L, BB->getFirstNonPHI()->getDebugLoc(), BB->getFirstNonPHI());
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
        auto *load = new LoadInst(ctx.V, "dummy.load." + ctx.V->getName(), *Pred);
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
  auto *EM = AT.find(MemoryLocation(V));
  for (auto *BB : L->getBlocks()) {
    for (auto &Inst : BB->getInstList()) {
      // find where the value is defined
      if (dyn_cast<Value>(&Inst) == V) {
        return false;
      }

     /* bool writesToV = false;
      auto memLambda = [&EM, &writesToV, &AT](Instruction &I, MemoryLocation &&Loc,
          unsigned, AccessInfo, AccessInfo IsWrite)
      {
        if (writesToV) {
          return;
        }
//        if (IsWrite != AccessInfo::No) {
          if (auto *ST = dyn_cast<StoreInst>(&I)) {
            std::cerr << "found store " << ST->getPointerOperand()->getName().data() << std::endl;
          }
          auto instEM = AT.find(Loc);
          if (EM && instEM && !EM->isUnreachable(*instEM)) {
            std::cerr << "here" << std::endl;
            writesToV = true;
          }
//        }
      };
      for_each_memory(Inst, TLI, memLambda,
        [](Instruction &, AccessInfo, AccessInfo) {});
      if (writesToV) {
        return false;
      }*/
    }
  }
  return true;
}

bool PointerReductionPass::runOnFunction(Function &F) {
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &LoopAttr = getAnalysis<LoopAttributesDeductionPass>();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();

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
      Pool = make_unique<DIMemoryTraitRegionPool>();
    } else {
      for (auto &T : *Pool) {
        if (T.is<trait::Anti>() || T.is<trait::Flow>() || T.is<trait::Output>()) {
          for (auto &V : *T.getMemory()) {
            if (V.pointsToAliveValue() && !dyn_cast<UndefValue>(V)) {
              for (auto *User : V->users()) {
                if (auto *LI = dyn_cast<LoadInst>(User)) {
                  if (L->contains(LI) && hasPossibleReductionUses(LI)) {
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

    for (auto *Val : Values) {
      auto *V = Val;
      if (auto *Load = dyn_cast<LoadInst>(Val)) {
        V = Load->getPointerOperand();
      }
      if (!validateValue(V, L)) {
        break;
      }
      if (hasVolatileLoadInstInLoop(V, L)) {
        break;
      }
      if (!analyzeAliasTree(V, AT, L, TLI)) {
        break;
      }
      auto ctx = PtrRedContext(V, F, L, Val != V);
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
