//===---- tsar_dbg_output.cpp - Output functions for debugging --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements a set of output functions specified in tsar_dbg_output.h
//
//===----------------------------------------------------------------------===//

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include <llvm/DebugInfo.h>
#else
#include <llvm/IR/DebugInfo.h>
#endif
#include "tsar_dbg_output.h"
#include "tsar_utility.h"

using namespace llvm;

namespace tsar {
void printLocationSource(llvm::raw_ostream &o, Value *Loc) {
  assert(Loc && "Location must not be null!");
  if (LoadInst *LI = dyn_cast<LoadInst>(Loc)) {
    Loc = LI->getPointerOperand();
    o << "*(", printLocationSource(o, Loc), o << ")";
    return;
  }
  if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(Loc)) {
    Loc = GEPI->getPointerOperand();
    o << "(", printLocationSource(o, Loc), o << ") + ...";
    return;
  }
  DIVariable *DIVar = nullptr;
  if (GlobalVariable *Var = dyn_cast<GlobalVariable>(Loc))
    DIVar = getMetadata(Var);
  else if (AllocaInst *AI = dyn_cast<AllocaInst>(Loc))
    DIVar = getMetadata(AI);
  if (DIVar)
    printDIVariable(o, DIVar);
  else
    o << "<unsupported location>";
  o << " " << *Loc;
}

void printDIType(raw_ostream &o, const DITypeRef &DITy) {
  Metadata *DITyVal = (Metadata *)DITy;
  bool isDerived = false;
  if (DIDerivedType *DITy = dyn_cast_or_null<DIDerivedType>(DITyVal)) {
    DITyVal = DITy->getBaseType();
    isDerived = true;
  }
  if (DIType *DITy = dyn_cast_or_null<DIType>(DITyVal))
    o << DITy->getName();
  else
    o << "<unknown type>";
  if (isDerived)
    o << "*";
}

void printDIVariable(raw_ostream &o, DIVariable *DIVar) {
  assert(DIVar && "Variable must not be null!");
  o << DIVar->getLine() << ": ";
  printDIType(o, DIVar->getType()), o << " ";
  o << DIVar->getName();
}

namespace {
void printLoops(llvm::raw_ostream &o, const Twine &Offset,
                LoopInfo::reverse_iterator ReverseI,
                LoopInfo::reverse_iterator ReverseEI) {
  for (; ReverseI != ReverseEI; --ReverseEI) {
    (Offset + "- ").print(o);
    DebugLoc loc = (*ReverseI)->getStartLoc();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 7)
    loc.print(getGlobalContext(), o);
#else
    loc.print(o);
#endif
    o << "\n";
    printLoops(o, Offset + "\t", (*ReverseI)->rbegin(), (*ReverseI)->rend());
  }
}
}

void printLoops(llvm::raw_ostream &o, const LoopInfo &LI) {
  printLoops(o, "", LI.rbegin(), LI.rend());
}
}