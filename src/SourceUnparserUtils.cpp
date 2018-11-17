//===- SourceUnparserUtils.cpp - Utils For Source Info Unparser -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements utility functions to generalize unparsing of metdata
// for different source languages.
//
//===----------------------------------------------------------------------===//

#include "SourceUnparserUtils.h"
#include "CSourceUnparser.h"
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/IR/DebugInfoMetadata.h>

using namespace llvm;

namespace tsar {
bool unparseToString(unsigned DWLang,
    const DIMemoryLocation &Loc, llvm::SmallVectorImpl<char> &S) {
  switch (DWLang) {
  case dwarf::DW_LANG_C:
  case dwarf::DW_LANG_C89:
  case dwarf::DW_LANG_C99:
  case dwarf::DW_LANG_C11:
  case dwarf::DW_LANG_C_plus_plus:
  case dwarf::DW_LANG_C_plus_plus_03:
  case dwarf::DW_LANG_C_plus_plus_11:
  case dwarf::DW_LANG_C_plus_plus_14:
  {
    CSourceUnparser U(Loc);
    return U.toString(S);
  }
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    llvm_unreachable("Unparsing of Fortran metadata is not implemented yet!");
  }
  return false;
}

bool unparsePrint(unsigned DWLang,
    const DIMemoryLocation &Loc, llvm::raw_ostream &OS) {
  switch (DWLang) {
  case dwarf::DW_LANG_C:
  case dwarf::DW_LANG_C89:
  case dwarf::DW_LANG_C99:
  case dwarf::DW_LANG_C11:
  case dwarf::DW_LANG_C_plus_plus:
  case dwarf::DW_LANG_C_plus_plus_03:
  case dwarf::DW_LANG_C_plus_plus_11:
  case dwarf::DW_LANG_C_plus_plus_14:
  {
    CSourceUnparser U(Loc);
    return U.print(OS);
  }
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    llvm_unreachable("Unparsing of Fortran metadata is not implemented yet!");
  }
  return false;
}

bool unparseDump(unsigned DWLang, const DIMemoryLocation &Loc) {
  switch (DWLang) {
  case dwarf::DW_LANG_C:
  case dwarf::DW_LANG_C89:
  case dwarf::DW_LANG_C99:
  case dwarf::DW_LANG_C11:
  case dwarf::DW_LANG_C_plus_plus:
  case dwarf::DW_LANG_C_plus_plus_03:
  case dwarf::DW_LANG_C_plus_plus_11:
  case dwarf::DW_LANG_C_plus_plus_14:
  {
    CSourceUnparser U(Loc);
    return U.dump();
  }
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    llvm_unreachable("Unparsing of Fortran metadata is not implemented yet!");
  }
  return false;
}
}
