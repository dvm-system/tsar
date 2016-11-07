//===-------- main.cpp ------ Traits Static Analyzer ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Traits Static Analyzer (TSAR) is a part of a system for automated
// parallelization SAPFOR. The main goal of analyzer is to determine
// data dependences, privatizable and induction variables and other traits of
// analyzed program which could be helpful to parallelize program automatically.
//
//===----------------------------------------------------------------------===//

#include <llvm/Support/Debug.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include "tsar_tool.h"

using namespace llvm;
using namespace tsar;

int main(int Argc, const char** Argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram StackTraceProgram(Argc, Argv);
  EnableDebugBuffering = true;
  llvm_shutdown_obj ShutdownObj; //call llvm_shutdown() on exit
  Tool Analyzer(Argc, Argv);
  return Analyzer.run();
}