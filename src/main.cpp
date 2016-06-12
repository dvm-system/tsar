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

#include <map>
#include "tsar_bimap.h"

using namespace llvm;
using namespace tsar;

int main(int Argc, const char** Argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram StackTraceProgram(Argc, Argv);
  EnableDebugBuffering = true;
  llvm_shutdown_obj ShutdownObj; //call llvm_shutdown() on exit
  Bimap<int, int> BM(Bimap<int, int>({std::make_pair(1,2), std::make_pair(5,6)}));
  decltype(BM) BM2{std::make_pair(3,4)};
  decltype(BM) BM3;
  BM3 = BM2;
  errs() << (BM3 == BM2) << (BM3 != BM2) << (BM2 == BM) << (BM2 != BM);


  Tool Analyzer(Argc, Argv);
  return Analyzer.run();
}