//===---- tsar_dbg_output.cpp - Output functions for debugging --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements a set of output functions.
//
//===----------------------------------------------------------------------===//

#include "tsar_dbg_output.h"
#include "DIEstimateMemory.h"
#include "DIMemoryLocation.h"
#include "DIUnparser.h"
#include "tsar_pass.h"
#include "SourceUnparserUtils.h"
#include "tsar_utility.h"
#include <bcl/utility.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace tsar {
void printLocationSource(llvm::raw_ostream &O, const Value *Loc,
    const DominatorTree *DT) {
  if (!Loc)
    O << "?";
  else if (!unparsePrint(O, Loc, DT))
    Loc->printAsOperand(O, false);
}

void printLocationSource(llvm::raw_ostream &O, const llvm::MemoryLocation &Loc,
    const DominatorTree *DT) {
  O << "<";
  printLocationSource(O, Loc.Ptr, DT);
  O << ", ";
  if (Loc.Size == MemoryLocation::UnknownSize)
    O << "?";
  else
    O << Loc.Size;
  O << ">";
}

void printDILocationSource(unsigned DWLang,
    const DIMemoryLocation &Loc, raw_ostream &O) {
  if (!Loc.isValid()) {
    O << "<";
    O << "sapfor.invalid";
    if (Loc.Var)
      O << "(" << Loc.Var->getName() << ")";
    O << ",?>";
    return;
  }
  O << "<";
  if (!unparsePrint(DWLang, Loc, O))
    O << "?" << Loc.Var->getName() << "?";
  O << ", ";
  auto Size = Loc.getSize();
  if (Size == MemoryLocation::UnknownSize)
    O << "?";
  else
    O << Size;
  O << ">";
}

void printDILocationSource(unsigned DWLang,
    const DIMemory &Loc, llvm::raw_ostream &O) {
  auto M = const_cast<DIMemory *>(&Loc);
  auto printDbgLoc = [&Loc, &O]() {
    SmallVector<DebugLoc, 1> DbgLocs;
    Loc.getDebugLoc(DbgLocs);
    if (DbgLocs.size() == 1) {
      O << ":" << DbgLocs.front().getLine() << ":" << DbgLocs.front().getCol();
    } else if (!DbgLocs.empty()) {
      O << ":{";
      auto I = DbgLocs.begin();
      for (auto EI = DbgLocs.end() - 1; I != EI; ++I)
        if (*I)
          O << I->getLine() << ":" << I->getCol() << "|";
      if (*I)
        O << I->getLine() << ":" << I->getCol() << "}";
    }
  };
  if (auto EM = dyn_cast<DIEstimateMemory>(M)) {
    DIMemoryLocation TmpLoc{ EM->getVariable(), EM->getExpression(),
      nullptr, EM->isTemplate() };
    if (!TmpLoc.isValid()) {
      O << "<";
      O << "sapfor.invalid";
      if (TmpLoc.Var)
        O << "(" << TmpLoc.Var->getName() << ")";
      printDbgLoc();
      O << ",?>";
      return;
    }
    O << "<";
    if (!unparsePrint(DWLang, TmpLoc, O))
      O << "?" << TmpLoc.Var->getName() << "?";
    printDbgLoc();
    O << ", ";
    auto Size = TmpLoc.getSize();
    if (Size == MemoryLocation::UnknownSize)
      O << "?";
    else
      O << Size;
    O << ">";
  } else if (auto UM = dyn_cast<DIUnknownMemory>(M)) {
    auto MD = UM->getMetadata();
    assert(MD && "MDNode must not be null!");
    if (UM->isExec()) {
      if (!isa<DISubprogram>(MD))
        O << "?()";
      else
        O << cast<DISubprogram>(MD)->getName() << "()";
      printDbgLoc();
    } else if (UM->isResult()) {
      if (!isa<DISubprogram>(MD))
        O << "<?()";
      else
        O << "<" << cast<DISubprogram>(MD)->getName() << "()";
      printDbgLoc();
      O << ",?>";
    } else {
      if (isa<DISubprogram>(MD)) {
        O << "<*" << cast<DISubprogram>(MD)->getName() << ",?>";
      } else if (isa<DIVariable>(MD)) {
        O << "<*" << cast<DIVariable>(MD)->getName() << ",?>";
      } else {
        SmallString<32> Address("?");
        if (MD->getNumOperands() == 1)
          if (auto Const = dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
            auto CInt = cast<ConstantInt>(Const->getValue());
            Address.front() = '*';
            CInt->getValue().toStringUnsigned(Address);
          }
        O << "<" << Address;
        printDbgLoc();
        O << ",?>";
      }
    }
  } else {
    O << "<sapfor.invalid,?>";
  }
}

void printDIType(raw_ostream &o, const DITypeRef &DITy) {
  Metadata *DITyVal = DITy;
  bool isDerived = false;
  if (auto *DITy = dyn_cast_or_null<DIDerivedType>(DITyVal)) {
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
void printLoops(llvm::raw_ostream &OS, bool FilenameOnly, const Twine &Offset,
                LoopInfo::reverse_iterator ReverseI,
                LoopInfo::reverse_iterator ReverseEI) {
  for (; ReverseI != ReverseEI; --ReverseEI) {
    (Offset + "- ").print(OS);
    DebugLoc Loc = (*ReverseI)->getStartLoc();
    print(OS, Loc, FilenameOnly);
    OS << "\n";
    printLoops(OS, FilenameOnly, Offset + "\t",
      (*ReverseI)->rbegin(), (*ReverseI)->rend());
  }
}
}

void printLoops(llvm::raw_ostream &o, const LoopInfo &LI, bool FilenameOnly) {
  printLoops(o, FilenameOnly, "", LI.rbegin(), LI.rend());
}

void print(llvm::raw_ostream &OS, llvm::DebugLoc Loc, bool FilenameOnly) {
  if (!Loc)
    return;
  OS << getFile(Loc, FilenameOnly);
  OS << ':' << Loc.getLine();
  if (Loc.getCol() != 0)
    OS << ':' << Loc.getCol();

  if (llvm::DebugLoc InlinedAtDL = Loc.getInlinedAt()) {
    OS << " @[ ";
    print(OS, InlinedAtDL, FilenameOnly);
    OS << " ]";
  }
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
    auto *P = mPassToPrint->getNormalCtor()();
    P->getAnalysisUsage(AU);
    delete P;
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