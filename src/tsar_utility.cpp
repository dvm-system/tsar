//===--- tsar_utility.cpp - Utility Methods and Classes----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods declared in tsar_utility.h
//
//===----------------------------------------------------------------------===//

#include "tsar_utility.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Transforms/Utils/Local.h>
#include <regex>

using namespace llvm;

namespace {
/// Computes a list of basic blocks which dominates a list of specified uses of
/// a value `V` and contains some of `llvm.dbg.value` associated with `V`.
DenseMap<DIVariable *, SmallPtrSet<BasicBlock *, 8>>
findDomDbgValues(const Value *V, const DominatorTree &DT,
    const SmallVectorImpl<Instruction *> &Users) {
  DbgValueList AllDbgValues;
  FindAllocaDbgValues(AllDbgValues, const_cast<Value *>(V));
  DenseMap<DIVariable *, SmallPtrSet<BasicBlock *, 8>> Dominators;
  for (auto *DVI : AllDbgValues) {
    if (!DVI->getVariable())
      continue;
    bool IsDbgDom = true;
    auto UI = Users.begin(), UE = Users.end();
    for (; UI != UE; ++UI)
      if (!DT.dominates(DVI, *UI))
        break;
    if (UI != UE)
      continue;
    auto Pair = Dominators.try_emplace(DVI->getVariable());
    Pair.first->second.insert(DVI->getParent());
  }
  return Dominators;
}

/// \brief Determines all basic blocks which contains variables aliases with a
/// specified one.
///
/// Two conditions is going to be checked. At first it means that such block
/// should contain llvm.dbg.value intrinsic which is associated with a
/// specified variable `Var` and a register other then `V`.
/// The second condition is that there is no llvm.dbg.values after this
/// intrinsic in the basic block which is associated with `Var` and `V`.
SmallPtrSet<BasicBlock *, 16>
findAliasBlocks(const Value * V, const DIVariable *Var) {
  SmallPtrSet<BasicBlock *, 16> Aliases;
  auto *MDV = MetadataAsValue::getIfExists(
    Var->getContext(), const_cast<DIVariable *>(Var));
  if (!MDV)
    return Aliases;
  for (User *U : MDV->users()) {
    DbgValueInst *DVI = dyn_cast<DbgValueInst>(U);
    if (!DVI)
      continue;
    auto I = DVI->getIterator(), E = DVI->getParent()->end();
    for (; I != E; ++I) {
      if (auto *SuccDVI = dyn_cast<DbgValueInst>(&*I))
        if (SuccDVI->getValue() == V && SuccDVI->getVariable() == Var) {
          break;
        }
    }
    if (I == E)
      Aliases.insert(DVI->getParent());
  }
  return Aliases;
}

Value * cloneChainImpl(Value *From, Instruction *BoundInst, DominatorTree *DT,
    std::size_t BeforeCloneIdx, SmallVectorImpl<Instruction *> &CloneList) {
  assert(From && "Cloned value must not be null!");
  if (auto *AI = dyn_cast<AllocaInst>(From)) {
    assert(&AI->getFunction()->getEntryBlock() == AI->getParent() &&
      "Alloca instructions must be presented in an entry block only!");
    return From;
  }
  auto *I = dyn_cast<Instruction>(From);
  if (!I)
    return From;
  if (BoundInst && DT && DT->dominates(I, BoundInst))
    return From;
  if (isa<PHINode>(I)) {
    for (auto Idx = BeforeCloneIdx + 1, IdxE = CloneList.size();
         Idx != IdxE; ++Idx)
      delete CloneList[Idx];
    CloneList.resize(BeforeCloneIdx + 1);
    return nullptr;
  }
  auto *Clone = I->clone();
  CloneList.push_back(Clone);
  for (unsigned OpIdx = 0, OpIdxE = Clone->getNumOperands();
       OpIdx < OpIdxE; ++OpIdx) {
    auto *Op = Clone->getOperand(OpIdx);
    auto *OpClone =
      cloneChainImpl(Op, BoundInst, DT, BeforeCloneIdx, CloneList);
    if (!OpClone)
      return nullptr;
    if (OpClone != Op)
      Clone->setOperand(OpIdx, OpClone);
  }
  return Clone;
}
}

namespace tsar {
std::vector<StringRef> tokenize(StringRef Str, StringRef Pattern) {
  std::vector<StringRef> Tokens;
  std::regex Rgx(Pattern.data());
  std::cmatch Cm;
  if (!std::regex_search(Str.data(), Cm, Rgx))
    return Tokens;
  bool HasSubMatch = false;
  for (std::size_t I = 1; I < Cm.size(); ++I)
    if (Cm[I].matched) {
      HasSubMatch = true;
      Tokens.emplace_back(Cm[I].first, Cm[I].length());
    }
  if (!HasSubMatch)
    Tokens.emplace_back(Cm[0].first, Cm[0].length());
  while (std::regex_search(Cm[0].second, Cm, Rgx)) {
    bool HasSubMatch = false;
    for (std::size_t I = 1; I < Cm.size(); ++I)
      if (Cm[I].matched) {
        HasSubMatch = true;
        Tokens.emplace_back(Cm[I].first, Cm[I].length());
      }
    if (!HasSubMatch)
      Tokens.emplace_back(Cm[0].first, Cm[0].length());
  }
  return Tokens;
}

llvm::Argument * getArgument(llvm::Function &F, std::size_t ArgNo) {
  auto ArgItr = F.arg_begin();
  auto ArgItrE = F.arg_end();
  for (std::size_t I = 0; ArgItr != ArgItrE && I <= ArgNo; ++I, ++ArgItr);
  return ArgItr != ArgItrE ? &*ArgItr : nullptr;
}

bool cloneChain(Instruction *From,
    SmallVectorImpl<Instruction *> &CloneList,
    Instruction *BoundInst, DominatorTree *DT) {
  return cloneChainImpl(From, BoundInst, DT, CloneList.size(), CloneList);
}

bool findNotDom(Instruction *From, Instruction *BoundInst, DominatorTree *DT,
    SmallVectorImpl<Use *> &NotDom) {
  assert(From && "Instruction must not be null!");
  assert(BoundInst && "Bound instruction must not be null!");
  assert(DT && "Dominator tree must not be null!");
  if (!DT->dominates(From, BoundInst))
    return true;
  for (auto &Op : From->operands())
    if (auto *I = dyn_cast<Instruction>(&Op))
      if (findNotDom(I, BoundInst, DT, NotDom))
        NotDom.push_back(&Op);
  return false;
}

DIGlobalVariable * findMetadata(const GlobalVariable *Var) {
  assert(Var && "Variable must not be null!");
  if (auto DIExpr = dyn_cast_or_null<DIGlobalVariableExpression>(
      Var->getMetadata(LLVMContext::MD_dbg)))
    return DIExpr->getVariable();
  return nullptr;
}

DILocalVariable *findMetadata(const AllocaInst *AI) {
  assert(AI && "Alloca must not be null!");
  DbgDeclareInst *DDI = FindAllocaDbgDeclare(const_cast<AllocaInst *>(AI));
  return DDI ? DDI->getVariable() : nullptr;
}

DIVariable * findMetadata(const Value * V, SmallVectorImpl<DIVariable *> &Vars,
    const DominatorTree *DT) {
  assert(V && "Value must not be null!");
  Vars.clear();
  if (auto AI = dyn_cast<AllocaInst>(V)) {
    auto DIVar = findMetadata(AI);
    if (DIVar)
      Vars.push_back(DIVar);
    return DIVar;
  }
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    auto DIVar = findMetadata(GV);
    if (DIVar)
      Vars.push_back(DIVar);
    return DIVar;
  }
  if (!DT)
    return nullptr;
  SmallVector<Instruction *, 8> Users;
  // TODO (kaniandr@gmail.com): User is not an `Instruction` sometimes.
  // If `V` is a declaration of a function then call of this function will
  // be a `ConstantExpr`. May be some other cases exist.
  for (User *U : const_cast<Value *>(V)->users())
    if (auto I = dyn_cast<Instruction>(U))
      Users.push_back(I);
    else
      return nullptr;
  auto Dominators = findDomDbgValues(V, *DT, Users);
  for (auto &VarToDom : Dominators) {
    auto Var = VarToDom.first;
    // If Aliases is empty it does not mean that a current variable can be used
    // because if there are usage of V between two llvm.dbg.value (the first is
    // associated with some other variable and the second is associated with the
    // Var) the appropriate basic block will not be inserted into Aliases.
    auto Aliases = findAliasBlocks(V, Var);
    // Checks that each definition of a variable `Var` in `VarToDom.second`
    // reaches each uses of `V` in `Users`.
    bool IsReached = false;
    for (auto *I : Users) {
      auto BB = I->getParent();
      auto ReverseItr = I->getReverseIterator(), ReverseItrE = BB->rend();
      for (IsReached = false; ReverseItr != ReverseItrE; ++ReverseItr) {
        if (auto *DVI = dyn_cast<DbgValueInst>(&*ReverseItr))
          if (DVI->getVariable() == Var) {
            if (DVI->getValue() == V)
              IsReached = true;
            break;
          }
      }
      if (IsReached)
        continue;
      if (ReverseItr != ReverseItrE)
        break;
      // A backward traverse of CFG will be performed. It is started in a `BB`.
      SmallPtrSet<BasicBlock *, 8> VisitedPreds;
      std::vector<BasicBlock *> Worklist;
      Worklist.push_back(BB);
      IsReached = true;
      while (!Worklist.empty()) {
        auto WorkBB = Worklist.back();
        Worklist.pop_back();
        auto PredItr = pred_begin(WorkBB), PredItrE = pred_end(WorkBB);
        for (; PredItr != PredItrE; ++PredItr) {
          if (VisitedPreds.count(*PredItr))
            continue;
          if (Aliases.count(*PredItr))
            break;
          if (VarToDom.second.count(*PredItr))
            continue;
          Worklist.push_back(*PredItr);
          VisitedPreds.insert(*PredItr);
        }
        if (PredItr != PredItrE) {
          IsReached = false;
          break;
        }
      }
      if (!IsReached)
        break;
    }
    if (IsReached)
      Vars.push_back(Var);
  }
  auto VarItr = Vars.begin(), VarItrE = Vars.end();
  for (; VarItr != VarItrE; ++VarItr) {
    if ([&V, &Vars, &VarItr, &VarItrE, &Dominators, DT]() {
      auto VarToDom = Dominators.find(*VarItr);
      for (auto I = Vars.begin(), E = VarItrE; I != E; ++I) {
        if (I == VarItr)
          continue;
        for (auto BB : Dominators.find(*I)->second)
          for (auto VarBB : VarToDom->second)
            if (VarBB == BB) {
              for (auto &Inst : *VarBB)
                if (auto DVI = dyn_cast<DbgValueInst>(&Inst))
                  if (DVI->getValue() == V)
                    if (DVI->getVariable() == *VarItr)
                      break;
                    else if (DVI->getVariable() == *I)
                      return false;
            } else if (!DT->dominates(VarBB, BB)) {
              return false;
            }
      }
      return true;
    }())
      return *VarItr;
  }
  return nullptr;
}
}
