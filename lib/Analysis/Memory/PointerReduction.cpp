#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/PointerReduction.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"

#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Transform/IR/InterprocAttr.h"

#include <iostream>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/PassSupport.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/IR/DIBuilder.h>

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

void getAllStoreOperandInstructions(Instruction *Inst, DenseSet<Instruction *> &Storage, SmallVector<Instruction *, 16> &toErase) {
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

void handleLoadsInBB(Value *V,
    BasicBlock *BB,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<BasicBlock *> &changedLastInst,
    bool changedVal)
{
  SmallVector<Instruction *, 16> toErase;
  for (auto &Instr : BB->getInstList()) {
    if (auto *Load = dyn_cast<LoadInst>(&Instr)) {
      if (Load->getPointerOperand() != V) {
        continue;
      }
      std::cerr << "found load\n";
      Instruction *lastVal = lastInstructions[BB];
      if (Load->user_empty()) {
        continue;
      }
      auto replaceAllLoadUsers = [&](LoadInst *Load) {
        DenseSet<Instruction *> instructions;
        getAllStoreOperandInstructions(Load, instructions, toErase);
        for (auto *Inst : instructions) {
          lastInstructions[Inst->getParent()] = Inst;
          changedLastInst.insert(Inst->getParent());
        }
        Load->replaceAllUsesWith(lastVal);
        std::cerr << "replaced load\n";
      };
      if (!changedVal) {
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
  if (pred_size(BB) == 1 && changedLastInst.find(BB) == changedLastInst.end()) {
      lastInstructions[BB] = lastInstructions[BB->getSinglePredecessor()];
  }
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

void handleLoads(
    Value *V,
    Loop *L,
    BasicBlock *BB,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<BasicBlock *> &completedBlocks,
    DenseSet<BasicBlock *> &changedLastInst,
    bool changedVal,
    bool init = false)
{
  if (completedBlocks.find(BB) != completedBlocks.end()) {
    return;
  }
  std::cerr << "now handling loads for " << BB->getName().data() << std::endl;

  if (!init) {
    handleLoadsInBB(V, BB, lastInstructions, changedLastInst, changedVal);
  }
  completedBlocks.insert(BB);
  for (auto *Succ : successors(BB)) {
    if (L->contains(Succ)) {
      handleLoads(V, L, Succ, lastInstructions, completedBlocks, changedLastInst, changedVal);
    }
  }
}

void freeLinks(DenseMap<BasicBlock *, phiNodeLink *> &phiLinks) {
  for (auto It : phiLinks) {
      delete It.second;
  }
}

void insertPhiNodes(
    BasicBlock *BB,
    DenseMap<BasicBlock *, phiNodeLink *> &phiLinks,
    DenseMap<BasicBlock *, Instruction *> &lastInstructions,
    DenseSet<PHINode *> &uniqueNodes,
    Value *V,
    bool init = false)
{
  if (phiLinks.find(BB) != phiLinks.end()) {
    return;
  }
  bool needsCreate = false;
  if (pred_size(BB) == 1 && !init) {
    if (phiLinks.find(BB->getSinglePredecessor()) != phiLinks.end()) {
        auto *parentLink = phiLinks.find(BB->getSinglePredecessor())->second;
        phiLinks[BB] = new phiNodeLink(parentLink);
    } else {
      needsCreate = true;
    }
  } else if (!init) {
    needsCreate = true;
  }
  if (needsCreate) {
    auto *phi = PHINode::Create(V->getType()->getPointerElementType(), 0,
        "phi." + BB->getName(), &BB->front());
    phiLinks[BB] = new phiNodeLink(phi);
    uniqueNodes.insert(phi);
  }
  for (auto *Succ : successors(BB)) {
    insertPhiNodes(Succ, phiLinks, lastInstructions, uniqueNodes, V);
  }
}

void deleteRedundantPhiNodes(
    DenseMap<BasicBlock *, phiNodeLink *> &phiLinks,
    DenseSet<PHINode *> &uniqueNodes)
{
  for (auto *Phi : uniqueNodes) {
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
        uniqueNodes.erase(Phi);
        phiLinks[Phi->getParent()]->phiNode = nullptr;
        auto *Instr = dyn_cast<Instruction>(operand);
        phiLinks[Phi->getParent()]->parent = phiLinks[Instr->getParent()];
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

    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";
    std::cerr << "--------------------\n";

    for (auto *Val : Values) {

      auto *V = Val;
      if (auto *Load = dyn_cast<LoadInst>(Val)) {
        std::cerr << "changed val for parent\n";
        V = Load->getPointerOperand();
      }
      bool changedVal = Val != V;
      std::cerr << "now working with " << V->getName().data() << " from " << F.getName().data() << std::endl;
      std::cerr << "type " << V->getType()->getPointerElementType()->getTypeID() << std::endl;
      std::cerr << "global " << dyn_cast<GlobalValue>(V) << std::endl;
      int count = 0;
      if (dyn_cast<GEPOperator>(V)) {
          break;
      }
      std::cerr << "Val has " << count << " uses\n";
      if (dyn_cast<GlobalValue>(V) && !V->getType()->getPointerElementType()->isPointerTy()) {
          break;
      }
      // we cannot replace anything if the pointer value can change somehow
      if (!validateValue(V, L)) {
        break;
      }
      // Load instructions that are marked volatile cannot be removed
      if (hasVolatileLoadInstInLoop(V, L)) {
        break;
      }
      std::cerr << "validated successfully\n";

      if (!analyzeAliasTree(V, AT, L, TLI)) {
        break;
      }
      std::cerr << "analyzed successfully\n";

      auto *BeforeInstr = new LoadInst(V, "load." + V->getName());
//      BeforeInstr->setMetadata("sapfor.da", MDNode::get(V->getContext(), {}));
//      auto *dbg = DbgValueInst::Create();
        BeforeInstr->insertBefore(&L->getLoopPredecessor()->back());

     /*   auto func = Intrinsic::getDeclaration(F.getParent(), Intrinsic::dbg_value);
        auto *tmp = &L->getLoopPredecessor()->back().getDebugLoc();
      auto *DIB = new DIBuilder(*F.getParent());
      auto *scope = cast<DIScope>(tmp->getScope());
      auto *debug = DIB->createAutoVariable(scope, "myvar", nullptr, tmp->getLine() + 1, nullptr);
      BeforeInstr->setMetadata("sapfor.dbg", debug);
        SmallVector<Value*, 3> Args(3);
        Args[0] = dyn_cast<Value>(BeforeInstr->getMetadata("sapfor.dbg"));
        Args[1] = dyn_cast<Value>(ConstantInt::get(BeforeInstr->getType(), 0));
        Args[2] = dyn_cast<Value>(debug);

        CallInst::Create(func, Args)->insertAfter(BeforeInstr);*/



        auto *oldBefore = BeforeInstr;
      if (changedVal) {
        auto Instr2 = new LoadInst(BeforeInstr, "load.ptr." + V->getName());
        Instr2->insertAfter(BeforeInstr);
        BeforeInstr = Instr2;
      }
        std::cerr << "inserted instructions\n";

      DenseMap<BasicBlock *, phiNodeLink *> phiLinks;
      DenseSet<PHINode *> uniqueNodes;
      DenseMap<BasicBlock *, Instruction *> lastInstructions;
      insertPhiNodes(L->getLoopPredecessor(), phiLinks, lastInstructions, uniqueNodes, Val, true);
        std::cerr << "inserted phi nodes\n";
      lastInstructions[L->getLoopPredecessor()] = BeforeInstr;
      for (auto &P : phiLinks) {
        lastInstructions[P.getFirst()] = P.getSecond()->getPhi();
      }

      DenseSet<BasicBlock *> processedBlocks;
      DenseSet<BasicBlock *> changedLastInst;
        std::cerr << "handling loads\n";
      handleLoads(V, L, L->getLoopPredecessor(), lastInstructions, processedBlocks, changedLastInst, changedVal, true);
        std::cerr << "loads handled\n";

      for (auto *Phi : uniqueNodes) {
        auto *BB = Phi->getParent();
        for (auto Pred = pred_begin(BB); Pred != pred_end(BB); Pred++) {
          if (lastInstructions.find(*Pred) != lastInstructions.end()) {
            Phi->addIncoming(lastInstructions[*Pred], *Pred);
          } else {
              auto *load = new LoadInst(V, "dummy.load." + V->getName(), *Pred);
              Phi->addIncoming(load, *Pred);
          }
        }
      }
      deleteRedundantPhiNodes(phiLinks, uniqueNodes);

      SmallVector<BasicBlock *, 8> ExitBlocks;
      L->getExitBlocks(ExitBlocks);
      for (auto *BB : ExitBlocks) {
        auto storeVal = changedVal ? oldBefore : V;
        new StoreInst(lastInstructions[BB], storeVal, BB->getFirstNonPHI());
      }
      freeLinks(phiLinks);
    }
  });
  return false;
}
