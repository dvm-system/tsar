//===--- Diagnostic.h - C Language Family Diagnostic Handling ---*- C++ -*-===//
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

#include "tsar/Support/Clang/Diagnostic.h"

namespace tsar {
clang::DiagnosticBuilder toDiag(clang::DiagnosticsEngine &Diags,
    clang::SourceLocation Loc, unsigned int DiagId) {
  switch (DiagId) {
  default: return Diags.Report(Loc, DiagId);
#define DIAG(ENUM,LEVEL,DESC) \
  case tsar::diag::ENUM: \
    { \
      unsigned CustomId = Diags.getCustomDiagID( \
        clang::DiagnosticsEngine::LEVEL, DESC); \
      return Diags.Report(Loc, CustomId); \
    }
#include "tsar/Support/DiagnosticKinds.inc"
#undef DIAG
  }
}

clang::DiagnosticBuilder toDiag(clang::DiagnosticsEngine &Diags,
    unsigned int DiagId) {
  switch (DiagId) {
  default: return Diags.Report(DiagId);
#define DIAG(ENUM,LEVEL,DESC) \
  case tsar::diag::ENUM: \
    { \
      unsigned CustomId = Diags.getCustomDiagID( \
        clang::DiagnosticsEngine::LEVEL, DESC); \
      return Diags.Report(CustomId); \
    }
#include "tsar/Support/DiagnosticKinds.inc"
#undef DIAG
  }
}
}
