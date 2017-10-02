//===- TableGen.cpp - Top-Level TableGen implementation for TSAR ----------===//
//
//                     Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file contains the main function for TSAR's TableGen.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/TableGen/Main.h"
#include "llvm/TableGen/Record.h"

using namespace llvm;

enum ActionType {
  GenTSARDiagsDefs,
};

namespace {
  cl::opt<ActionType>
  Action(cl::desc("Action to perform:"),
         cl::values(clEnumValN(GenTSARDiagsDefs, "gen-tsar-diags-defs",
                               "Generate TSAR diagnostics definitions")));

bool LLVMTableGenMain(raw_ostream &OS, RecordKeeper &Records) {
  switch (Action) {
  case GenTSARDiagsDefs:
    for (Record *Rec : Records.getAllDerivedDefinitions("Diagnostic")) {
      OS << "DIAG(" << Rec->getName() << ", ";
      OS << Rec->getValueAsDef("Level")->getName();
      OS << ", \"";
      OS.write_escaped(Rec->getValueAsString("Text")) << '"';
      OS << ")\n";
    }
    break;
  }
  return false;
}
}

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv);

  llvm_shutdown_obj Y;

  return TableGenMain(argv[0], &LLVMTableGenMain);
}

#ifdef __has_feature
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
// Disable LeakSanitizer for this binary as it has too many leaks that are not
// very interesting to fix. See compiler-rt/include/sanitizer/lsan_interface.h .
int __lsan_is_turned_off() { return 1; }
#endif  // __has_feature(address_sanitizer)
#endif  // defined(__has_feature)
