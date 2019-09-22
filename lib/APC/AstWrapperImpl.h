//===- AstWrapperImpl.h - LLVM Based Source Level Information ---*- C++ -*-===//
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
// This file implements classes which are declared and used in external APC
// library. These classes propose access to a source-level information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_AST_WRAPPER_IMPL_H
#define TSAR_AST_WRAPPER_IMPL_H

#include "tsar/Analysis/Memory/DIMemoryLocation.h"

class Symbol {
public:
  explicit Symbol(tsar::DIMemoryLocation DIM) : mMemory(DIM) {}
  const tsar::DIMemoryLocation & getMemory() const noexcept { return mMemory; }
private:
  tsar::DIMemoryLocation mMemory;
};

#endif//TSAR_AST_WRAPPER_IMPL_H
