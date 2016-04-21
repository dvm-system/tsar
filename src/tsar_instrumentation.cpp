//===- tsar_instrumentation.cpp - TSAR Instrumentation Engine ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements LLVM IR level instrumentation engine.
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Function.h>
#include "tsar_instrumentation.h"

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "instrumentation"

STATISTIC(NumInstLoop, "Number of instrumented loops");

char InstrumentationPass::ID = 0;
INITIALIZE_PASS_BEGIN(InstrumentationPass, "instrumentation",
  "LLVM IR Instrumentation", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(InstrumentationPass, "instrumentation",
  "LLVM IR Instrumentation", false, false)

bool InstrumentationPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  llvm::Module *M = F.getParent();
  return true;
}

void InstrumentationPass::releaseMemory() {}

void InstrumentationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
}

FunctionPass * llvm::createInstrumentationPass() {
  return new InstrumentationPass();
}