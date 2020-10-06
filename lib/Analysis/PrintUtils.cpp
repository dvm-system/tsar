//===- PrintUtils.cpp ------------ Output Functions ------------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements a set of output functions for various bits of
// information.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Support/GlobalOptions.h"
#include <bcl/utility.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include "llvm/Pass.h"
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace tsar;

namespace {
void printLoopsImpl(llvm::raw_ostream &OS, bool FilenameOnly, const Twine &Offset,
                LoopInfo::reverse_iterator ReverseI,
                LoopInfo::reverse_iterator ReverseEI) {
  for (; ReverseI != ReverseEI; --ReverseEI) {
    (Offset + "- ").print(OS);
    DebugLoc Loc = (*ReverseI)->getStartLoc();
    print(OS, Loc, FilenameOnly);
    OS << "\n";
    printLoopsImpl(OS, FilenameOnly, Offset + "\t",
      (*ReverseI)->rbegin(), (*ReverseI)->rend());
  }
}
}

namespace tsar {
StringRef getFile(DebugLoc Loc, bool FilenameOnly) {
  auto *Scope = cast<DIScope>(Loc.getScope());
  auto Filename = Scope->getFilename();
  if (FilenameOnly) {
    Filename.consume_front(Scope->getDirectory());
    Filename = Filename.ltrim("/\\");
  }
  return Filename;
}

void printLoops(llvm::raw_ostream &o, const LoopInfo &LI, bool FilenameOnly) {
  printLoopsImpl(o, FilenameOnly, "", LI.rbegin(), LI.rend());
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
/// Print analysis info for function passes.
class FunctionPassPrinter : public FunctionPass, public bcl::Uncopyable {
public:
  static char ID;

  FunctionPassPrinter(const PassInfo *PI, raw_ostream &Out)
    : FunctionPass(ID), mPassToPrint(PI), mOut(Out) {
    assert(PI && "PassInfo must not be null!");
    auto PassToPrintName = mPassToPrint->getPassName().str();
    mPassName = "FunctionPass Printer: " + PassToPrintName;
  }

  bool runOnFunction(Function &F) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    if (!GO.AnalyzeLibFunc && tsar::hasFnAttr(F, tsar::AttrKind::LibFunc))
      return false;
    mOut << "Printing analysis '" << mPassToPrint->getPassName()
      << "' for function '" << F.getName() << "':\n";
    getAnalysisID<Pass>(mPassToPrint->getTypeInfo()).
      print(mOut, F.getParent());
    return false;
  }

  StringRef getPassName() const override { return mPassName; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GlobalOptionsImmutableWrapper>();
    // Pass manager releases memory which has been used in passes that are not
    // used any longer. So, if we want to use some of analysis available in
    // the printed pass, we should specify necessary analysis passes for the
    // print pass.
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

/// Print analysis info for module passes.
class ModulePassPrinter : public ModulePass {
public:
  static char ID;

  ModulePassPrinter(const PassInfo *PI, raw_ostream &out)
      : ModulePass(ID), mPassToPrint(PI), mOut(out) {
    assert(PI && "PassInfo must not be null!");
    auto PassToPrintName = mPassToPrint->getPassName().str();
    mPassName = "ModulePass Printer: " + PassToPrintName;
  }

  bool runOnModule(Module &M) override {
    mOut << "Printing analysis '" << mPassToPrint->getPassName() << "':\n";
    getAnalysisID<Pass>(mPassToPrint->getTypeInfo()).print(mOut, &M);
    return false;
  }

  StringRef getPassName() const override { return mPassName; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Pass manager releases memory which has been used in passes that are not
    // used any longer. So, if we want to use some of analysis available in
    // the printed pass, we should specify necessary analysis passes for the
    // print pass.
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

char ModulePassPrinter::ID = 0;
}

FunctionPass *llvm::createFunctionPassPrinter(
    const PassInfo *PI, raw_ostream &OS) {
  return new FunctionPassPrinter(PI, OS);
}

ModulePass *llvm::createModulePassPrinter(const PassInfo *PI, raw_ostream &OS) {
  return new ModulePassPrinter(PI, OS);
}
