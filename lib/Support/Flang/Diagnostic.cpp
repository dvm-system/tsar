//=== Diagnostic.cpp - Fortran Language Family Diagnostic Handling -* C++ *===//
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
// Implements the Diagnostic-related interfaces.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/Flang/Diagnostic.h"
#include "tsar/Support/Diagnostic.h"

using namespace Fortran;
using namespace llvm;
using namespace tsar;

const tsar::DiagnosticInfo * tsar::getFlangMessageText(unsigned int DiagId,
    SmallVectorImpl<char> &Text) {
  Text.clear();
  auto Diag{getDiagnosticInfo(DiagId)};
  if (!Diag)
    return nullptr;
  char PrevChar{'\0'};
  bool InArgRef{false};
  for (auto C : Diag->description()) {
    if (InArgRef) {
      if (std::isdigit(C))
        continue;
      InArgRef = false;
      Text.push_back('s');
    } else if (PrevChar != '\\' && C == '%') {
      InArgRef = true;
    }
    Text.push_back(C);
    PrevChar = C;
  }
  return Diag;
}