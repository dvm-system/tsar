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
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace tsar {
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
