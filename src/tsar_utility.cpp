//===--- tsar_utility.cpp - Utility Methods and Classes----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods declared in tsar_utility.h
//
//===----------------------------------------------------------------------===//

#include <llvm/IR/Module.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Transforms/Utils/Local.h>
#include "tsar_utility.h"

using namespace llvm;

namespace tsar {
DIGlobalVariable * getMetadata(const GlobalVariable *Var) {
  assert(Var && "Variable must not be null!");
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
  return nullptr;
}

DILocalVariable *getMetadata(const AllocaInst *AI) {
  assert(AI && "Alloca must not be null!");
  DbgDeclareInst *DDI = FindAllocaDbgDeclare(const_cast<AllocaInst *>(AI));
  return DDI ? DDI->getVariable() : nullptr;
}
}
