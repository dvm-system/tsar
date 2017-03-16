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
#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_dbg_output.h"
#include "tsar_df_location.h"
#include "tsar_pass.h"
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

namespace {
/// \brief Prints analysis info for function passes.
///
/// This class is similar to printers which is used in the opt tool.
class FunctionPassPrinter : public FunctionPass, public bcl::Uncopyable {
public:
  static char ID;

  FunctionPassPrinter(const PassInfo *PI, raw_ostream &Out)
    : FunctionPass(ID), mPassToPrint(PI), mOut(Out) {
    assert(PI && "PassInfo must not be null!");
    std::string PassToPrintName = mPassToPrint->getPassName();
    mPassName = "FunctionPass Printer: " + PassToPrintName;
  }

  bool runOnFunction(Function &F) override {
    mOut << "Printing analysis '" << mPassToPrint->getPassName()
      << "' for function '" << F.getName() << "':\n";
    getAnalysisID<Pass>(mPassToPrint->getTypeInfo()).
      print(mOut, F.getParent());
    return false;
  }

  StringRef getPassName() const override { return mPassName; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequiredID(mPassToPrint->getTypeInfo());
    AU.setPreservesAll();
  }

private:
  const PassInfo *mPassToPrint;
  raw_ostream &mOut;
  std::string mPassName;
};

char FunctionPassPrinter::ID = 0;
}

/// \brief Creates a pass to print analysis info for function passes.
///
/// To use this function it is necessary to override
/// void `llvm::Pass::print(raw_ostream &O, const Module *M) const` method for
/// a function pass internal state of which must be printed.
FunctionPass *llvm::createFunctionPassPrinter(
    const PassInfo *PI, raw_ostream &OS) {
  return new FunctionPassPrinter(PI, OS);
}