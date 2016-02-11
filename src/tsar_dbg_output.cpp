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
#include <llvm/IR/DebugInfo.h>
#include "tsar_dbg_output.h"
#include "tsar_df_location.h"
#include "tsar_utility.h"

using namespace llvm;

namespace tsar {
void printLocationSource(llvm::raw_ostream &o, const Value *Loc) {
  if (!Loc) {
    o << "<unknown location>";
    return;
  }
  auto Src = locationToSource(Loc);
  o << Src;
  if (!Src.empty())
    return;
  if (auto *LI = dyn_cast<const LoadInst>(Loc))
    o << "*(", printLocationSource(o, LI->getPointerOperand()), o << ")";
  else if (auto *GEPI = dyn_cast<const GetElementPtrInst>(Loc))
    o << "(", printLocationSource(o, GEPI->getPointerOperand()), o << ") + ...";
  else
    o << *Loc;
}

void printDIType(raw_ostream &o, const DITypeRef &DITy) {
  Metadata *DITyVal = DITy;
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
    loc.print(o);
    o << "\n";
    printLoops(o, Offset + "\t", (*ReverseI)->rbegin(), (*ReverseI)->rend());
  }
}
}

void printLoops(llvm::raw_ostream &o, const LoopInfo &LI) {
  printLoops(o, "", LI.rbegin(), LI.rend());
}
}