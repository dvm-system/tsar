//===--- Attributes.cpp --- TSAR Attributes ---------------------*- C++ -*-===//
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
// This file implements functions from Attributes.h which allow to process
// TSAR attributes.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include <bcl/utility.h>
#include <llvm/IR/Function.h>

using namespace llvm;
using namespace tsar;

/// Table of string attribute names indexed by enum value.
static const char * const AttributeNameTable[] = {
  "not_attribute",
#define GET_ATTRIBUTE_NAME_TABLE
#include "tsar/Analysis/Attributes.gen"
#undef GET_ATTRIBUTE_NAME_TABLE
};

namespace tsar {
StringRef getAsString(AttrKind Kind) {
  assert(Kind < AttrKind::num_attributes && Kind > AttrKind::not_attribute &&
    "Invalid attribute kind!");
  return AttributeNameTable[static_cast<unsigned>(Kind)];
}

void addFnAttr(llvm::Function &F, AttrKind Kind, StringRef Val) {
  F.addFnAttr(getAsString(Kind), Val);
}
void removeFnAttr(llvm::Function &F, AttrKind Kind) {
  F.removeFnAttr(getAsString(Kind));
}

bool hasFnAttr(const llvm::Function &F, AttrKind Kind) {
  return F.hasFnAttribute(getAsString(Kind));
}

/// Table of string names of input/output library functions.
static const char * const IOFunctionNameTable[] = {
#define GET_IO_FUNCTIONS
#include "tsar/Analysis/LibraryFunctions.def"
#undef GET_IO_FUNCTIONS
};

bool isIOLibFuncName(StringRef FuncName) {
  const char* const *Start = &IOFunctionNameTable[0];
  const char* const *End =
    &IOFunctionNameTable[bcl::array_sizeof(IOFunctionNameTable)];
  if (FuncName.empty())
    return false;
  const char* const *I = std::find(Start, End, FuncName);
  if (I != End)
    return true;
  return false;
}
}
