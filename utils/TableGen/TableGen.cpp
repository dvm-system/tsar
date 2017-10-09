//===- TableGen.cpp - Top-Level TableGen Implementation for TSAR -*- C++ -*===//
//
//                     Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file contains the main function for TSAR's TableGen.
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/TableGen/Main.h>
#include <llvm/TableGen/Record.h>

using namespace llvm;

enum ActionType {
  GenTSARDiagsDefs,
  GenTSARIntrinsicsDefs,
};

namespace {
  cl::opt<ActionType>
  Action(cl::desc("Action to perform:"),
         cl::values(clEnumValN(GenTSARDiagsDefs, "gen-tsar-diags-defs",
                               "Generate TSAR diagnostics definitions")),
         cl::values(clEnumValN(GenTSARIntrinsicsDefs,"gen-tsar-intrinsics-defs",
                               "Generate TSAR intrinsics definitions")));

void GenFileHeader(raw_ostream &OS) {
  OS << "\
//===- Automatically TableGen'erated file, do not edit!----------*- C++ -*-===//\
\n\n";
}

void GenTypeKindList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Enum values for type kinds of intrinsic parameters\n";
  OS << "#ifdef GET_INTRINSIC_TYPE_KINDS\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("TypeKind"))
    OS << "  " << Rec->getName() << ",\n";
  OS << "#endif\n\n";
}

void GenIntrinsicIdList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Enum values for Intrinsic IDs\n";
  OS << "#ifdef GET_INTRINSIC_ENUM_VALUES\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Intrinsic")) {
    OS << "  " << Rec->getName() << ",";
    OS << "                // " << Rec->getValueAsString("Name") << "\n";
  }
  OS << "#endif\n\n";
}

void GenIntrinsicNameList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Intrinsic ID to name table\n";
  OS << "#ifdef GET_INTRINSIC_NAME_TABLE\n";
  OS << "// Note that entry #0 is the invalid intrinsic!\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Intrinsic")) {
    OS << "  \"";
    OS.write_escaped(Rec->getValueAsString("Name")) << "\",\n";
  }
  OS << "#endif\n\n";
}

unsigned GenTypeDescription(raw_ostream &OS, Record *Record) {
  unsigned Size = 1; // this takes into account the last type and anchor
  for (; Record->getValueAsDef("Kind")->getName() == "Pointer";
       ++Size, Record = Record->getValueAsDef("ElTy"))
    OS << "Pointer, ";
  OS << Record->getValueAsDef("Kind")->getName() << ",\n";
  return Size;
}

void GenIntrinsicPrototypeList(raw_ostream &OS, RecordKeeper &Records,
    SmallVectorImpl<unsigned> &Offsets) {
  OS << "// Intrinsic Offset[ID] to prototype table\n";
  OS << "#ifdef GET_INTRINSIC_PROTOTYPE_TABLE\n";
  unsigned Offset = 0;
  for (Record *Rec : Records.getAllDerivedDefinitions("Intrinsic")) {
    Offsets.push_back(Offset);
    OS << "  // Prototype for " << Rec->getValueAsString("Name") << "\n";
    OS << "  ";
    Offset += GenTypeDescription(OS, Rec->getValueAsDef("RetType"));
    for (auto Param : Rec->getValueAsListOfDefs("ParamTypes")) {
      OS << "  ";
      Offset += GenTypeDescription(OS, Param);
    }
  }
  Offsets.push_back(Offset);
  OS << "#endif\n\n";
}

void GenIntrinsicOffsetList(raw_ostream &OS, RecordKeeper &Records) {
  SmallVector<unsigned, 32> Offsets;
  GenIntrinsicPrototypeList(OS, Records, Offsets);
  OS << "// Intrinsic ID to Offset (in prototype table) table\n";
  OS << "#ifdef GET_INTRINSIC_PROTOTYPE_OFFSET_TABLE\n";
  for (unsigned I = 0; I < Offsets.size() - 1; ++I)
    OS << "  PROTOTYPE(" << Offsets[I] << ',' << Offsets[I+1] << ")\n";
  OS << "#endif\n";
}

bool LLVMTableGenMain(raw_ostream &OS, RecordKeeper &Records) {
  GenFileHeader(OS);
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
  case GenTSARIntrinsicsDefs:
    GenTypeKindList(OS, Records);
    GenIntrinsicIdList(OS, Records);
    GenIntrinsicNameList(OS, Records);
    GenIntrinsicOffsetList(OS, Records);
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
