//===- SourceUnparserUtils.cpp - Utils For Source Info Unparser -*- C++ -*-===//
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
// This file implements utility functions to generalize unparsing of metdata
// for different source languages.
//
//===----------------------------------------------------------------------===//

#include "tsar/Unparse/SourceUnparserUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Unparse/CSourceUnparser.h"
#include "tsar/Unparse/FortranSourceUnparser.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace tsar {
bool unparseToString(unsigned DWLang,
    const DIMemoryLocation &Loc, llvm::SmallVectorImpl<char> &S, bool IsMinimal) {
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
    CSourceUnparser U(Loc, IsMinimal);
    return U.toString(S);
  }
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    FortranSourceUnparser U(Loc, IsMinimal);
    return U.toString(S);
  }
  return false;
}

bool unparsePrint(unsigned DWLang,
    const DIMemoryLocation &Loc, llvm::raw_ostream &OS, bool IsMinimal) {
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
    CSourceUnparser U(Loc, IsMinimal);
    return U.print(OS);
  }
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    FortranSourceUnparser U(Loc, IsMinimal);
    return U.print(OS);
  }
  return false;
}

bool unparseDump(unsigned DWLang, const DIMemoryLocation &Loc, bool IsMinimal) {
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
    CSourceUnparser U(Loc, IsMinimal);
    return U.dump();
  }
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    FortranSourceUnparser U(Loc, IsMinimal);
    return U.dump();
  }
  return false;
}

bool unparseCallee(const llvm::CallBase &CB, llvm::Module &M,
    llvm::DominatorTree &DT, llvm::SmallVectorImpl<char> &S, bool IsMinimal) {
  auto Callee = CB.getCalledOperand()->stripPointerCasts();
  if (auto F = dyn_cast<Function>(Callee)) {
    S.assign(F->getName().begin(), F->getName().end());
    return true;
  }
  auto DIM = buildDIMemory(MemoryLocation(Callee, LocationSize::afterPointer()),
    M.getContext(), M.getDataLayout(), DT);
  if (DIM && DIM->isValid())
    if (auto DWLang = getLanguage(*DIM->Var))
      return unparseToString(*DWLang, *DIM, S, IsMinimal);
   return false;
}
}
