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
#include <llvm/Config/llvm-config.h>
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
  Tokens.emplace_back(Cm[0].first, Cm[0].length());
  while (std::regex_search(Cm[0].second, Cm, Rgx))
    Tokens.emplace_back(Cm[0].first, Cm[0].length());
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

DIGlobalVariable * getMetadata(const GlobalVariable *Var) {
  assert(Var && "Variable must not be null!");
#if (LLVM_VERSION_MAJOR < 4)
  const Module *M = Var->getParent();
  assert(M && "Module must not be null!");
  NamedMDNode *CompileUnits = M->getNamedMetadata("llvm.dbg.cu");
  assert(CompileUnits && "Compile units must not be null!");
  for (MDNode *N : CompileUnits->operands()) {
    auto *CUNode = cast<DICompileUnit>(N);
    for (auto *DIVar : CUNode->getGlobalVariables()) {
      if (DIVar->getVariable() == Var)
        return DIVar;
    }
  }
#else
  if (auto DIExpr = dyn_cast_or_null<DIGlobalVariableExpression>(
      Var->getMetadata(LLVMContext::MD_dbg)))
    return DIExpr->getVariable();
  return nullptr;
#endif
}

DILocalVariable *getMetadata(const AllocaInst *AI) {
  assert(AI && "Alloca must not be null!");
  DbgDeclareInst *DDI = FindAllocaDbgDeclare(const_cast<AllocaInst *>(AI));
  return DDI ? DDI->getVariable() : nullptr;
}
}
