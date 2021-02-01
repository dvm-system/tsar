//===- TableGen.cpp - Top-Level TableGen Implementation for TSAR -*- C++ -*===//
//
//                     Traits Static Analyzer (SAPFOR)
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
// This file contains the main function for TSAR's TableGen.
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/SmallPtrSet.h>
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
  GenTSARDirectivesDefs,
  GenTSARAttrubutesDefs,
};

namespace {
  cl::opt<ActionType>
  Action(cl::desc("Action to perform:"),
         cl::values(clEnumValN(GenTSARDiagsDefs, "gen-tsar-diags-defs",
                               "Generate TSAR diagnostics definitions")),
         cl::values(clEnumValN(GenTSARDirectivesDefs, "gen-tsar-directives-defs",
                               "Generate TSAR directives definitions")),
         cl::values(clEnumValN(GenTSARIntrinsicsDefs,"gen-tsar-intrinsics-defs",
                               "Generate TSAR intrinsics definitions")),
         cl::values(clEnumValN(GenTSARAttrubutesDefs,"gen-tsar-attributes-defs",
                               "Generate TSAR attributes definitions")));

void GenFileHeader(raw_ostream &OS) {
  OS << "\
//===- Automatically TableGen'erated file, do not edit!----------*- C++ -*-===//\
\n\n";
}

//===----------------------------------------------------------------------===//
// Generate TSAR directives definitions.
//===----------------------------------------------------------------------===//

void GenExprKindList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// List of expression kinds of clause parameters\n";
  OS << "// #define KIND(EK, IsSingle, ClangTok) ... \n";
  OS << "#ifdef GET_CLAUSE_EXPR_KINDS\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("ExprKind")) {
    OS << "  KIND(" << Rec->getName()
      << ", " << (Rec->getValueAsBit("IsSingle") ? "true" : "false")
      << ", " << Rec->getValueAsString("ClangTok")
      << ")\n";
  }
  OS << "#endif\n\n";
}

void GenNamespaceIdList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Enum values for Directive IDs\n";
  OS << "#ifdef GET_NAMESPACE_ENUM_VALUES\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Namespace")) {
    OS << "  " << Rec->getName() << ",";
    OS << "                // " << Rec->getValueAsString("Name") << "\n";
  }
  OS << "#endif\n\n";
}

void GenDirectiveList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// List of directives\n";
  OS << "// #define DIRECTIVE(ID, Name, HasBody) ... \n";
  OS << "#ifdef GET_DIRECTIVE_LIST\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Directive")) {
    OS << "  DIRECTIVE(";
    OS << Rec->getName();
    OS << ",  \"";
    OS.write_escaped(Rec->getValueAsString("Name")) << "\"";
    OS << ", " << (Rec->getValueAsBit("HasBody") ? "true" : "false");
    OS << ")";
    Record *Parent = Rec->getValueAsDef("Parent");
    OS << "                // " << Parent->getValueAsString("Name") << " ";
    OS << Rec->getValueAsString("Name") << "\n";
  }
  OS << "#endif\n\n";
}

void GenClauseIdList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Enum values for Clause IDs\n";
  OS << "#ifdef GET_CLAUSE_ENUM_VALUES\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Clause")) {
    OS << "  " << Rec->getName() << ",";
    Record *Parent = Rec->getValueAsDef("Parent");
    OS << "                // " << Parent->getValueAsString("Name") << " ";
    OS << Rec->getValueAsString("Name") << "\n";
  }
  OS << "#endif\n\n";
}

unsigned GenExprDescription(raw_ostream &OS, Record *Rec) {
  unsigned Size = 1;
  OS << " CLAUSE_EXPR(" << Rec->getValueAsDef("Kind")->getName() << ")";
  if (Rec->getValueAsDef("Kind")->getValueAsBit("IsSingle"))
    return Size;
  for (Record *Child : Rec->getValueAsListOfDefs("ExprList")) {
    Size += GenExprDescription(OS, Child);
  }
  auto Anchor = Rec->getRecords().getDef("Anchor");
  OS << " CLAUSE_EXPR(" << Anchor->getValueAsDef("Kind")->getName() << ")";
  return Size + 1;
}

void GenClausePrototypeList(raw_ostream &OS, RecordKeeper &Records,
    SmallVectorImpl<unsigned> &Offsets) {
  OS << "// Clause Offset[ID] to prototype table\n";
  OS << "#ifdef GET_CLAUSE_PROTOTYPE_TABLE\n";
  unsigned Offset = 0;
  for (Record *Rec : Records.getAllDerivedDefinitions("Clause")) {
    Offsets.push_back(Offset);
    OS << "  // Prototype for " << Rec->getValueAsString("Name") << "\n";
    for (auto Param : Rec->getValueAsListOfDefs("ExprList")) {
      OS << " ";
      Offset += GenExprDescription(OS, Param);
      OS << "\n";
    }
  }
  Offsets.push_back(Offset);
  OS << "#endif\n\n";
}

void GenClauseOffsetList(raw_ostream &OS, RecordKeeper &Records) {
  SmallVector<unsigned, 32> Offsets;
  GenClausePrototypeList(OS, Records, Offsets);
  OS << "// Clause ID to Offset (in prototype table) table\n";
  OS << "#ifdef GET_CLAUSE_PROTOTYPE_OFFSET_TABLE\n";
  for (unsigned I = 0; I < Offsets.size() - 1; ++I)
    OS << "  PROTOTYPE(" << Offsets[I] << ',' << Offsets[I+1] << ")\n";
  OS << "#endif\n\n";
}

void GenNamespaceNameList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Namespace ID to name table\n";
  OS << "#ifdef GET_NAMESPACE_NAME_TABLE\n";
  OS << "// Note that entry #0 is the invalid namespace!\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Namespace")) {
    OS << "  \"";
    OS.write_escaped(Rec->getValueAsString("Name")) << "\",\n";
  }
  OS << "#endif\n\n";
}

void GenClauseNameList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Clause ID to name table\n";
  OS << "#ifdef GET_CLAUSE_NAME_TABLE\n";
  OS << "// Note that entry #0 is the invalid clause!\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Clause")) {
    OS << "  \"";
    OS.write_escaped(Rec->getValueAsString("Name")) << "\",\n";
  }
  OS << "#endif\n\n";
}

void GenDirectiveNamespaceList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Directive ID to parent Namespace ID table\n";
  OS << "#ifdef GET_DIRECTIVE_NAMESPACE_TABLE\n";
  OS << "// Note that entry #0 is the invalid directive!\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Directive")) {
    Record *Parent = Rec->getValueAsDef("Parent");
    OS << "  NAMESPACE(" << Parent->getName() << ")\n";
  }
  OS << "#endif\n\n";
}

void GenClauseDirectiveList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Clause ID to parent Directive ID table\n";
  OS << "#ifdef GET_CLAUSE_DIRECTIVE_TABLE\n";
  OS << "// Note that entry #0 is the invalid clause!\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Clause")) {
    Record *Parent = Rec->getValueAsDef("Parent");
    OS << "  DIRECTIVE(" << Parent->getName() << ")\n";
  }
  OS << "#endif\n\n";
}

//===----------------------------------------------------------------------===//
// Generate TSAR intrinsics definitions.
//===----------------------------------------------------------------------===//

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
  OS << "#endif\n\n";
}

//===----------------------------------------------------------------------===//
// Generate TSAR attributes definitions.
//===----------------------------------------------------------------------===//

void GenAttributeIdList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Enum values for Attribute IDs\n";
  OS << "#ifdef GET_ATTRIBUTE_ENUM_VALUES\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Attribute")) {
    OS << "  " << Rec->getName() << ",";
    OS << "                // " << Rec->getValueAsString("Name") << "\n";
  }
  OS << "#endif\n\n";
}

void GenAttributeNameList(raw_ostream &OS, RecordKeeper &Records) {
  OS << "// Attribute ID to name table\n";
  OS << "#ifdef GET_ATTRIBUTE_NAME_TABLE\n";
  OS << "// Note that entry #0 is the invalid attribute!\n";
  for (Record *Rec : Records.getAllDerivedDefinitions("Attribute")) {
    OS << "  \"";
    OS.write_escaped(Rec->getValueAsString("Name")) << "\",\n";
  }
  OS << "#endif\n\n";
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
  case GenTSARAttrubutesDefs:
    GenAttributeIdList(OS, Records);
    GenAttributeNameList(OS, Records);
    break;
  case GenTSARDirectivesDefs:
    GenExprKindList(OS, Records);
    GenNamespaceIdList(OS, Records);
    GenDirectiveList(OS, Records);
    GenClauseIdList(OS, Records);
    GenNamespaceNameList(OS, Records);
    GenClauseNameList(OS, Records);
    GenClauseDirectiveList(OS, Records);
    GenDirectiveNamespaceList(OS, Records);
    GenClauseOffsetList(OS, Records);
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
