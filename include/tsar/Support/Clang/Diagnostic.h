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
// Defines the Diagnostic-related interfaces.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_DIAGNOSTIC_H
#define TSAR_CLANG_DIAGNOSTIC_H

#include "tsar/Support/Diagnostic.h"
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceLocation.h>

namespace tsar {
/// \brief Issue the message to the client.
///
/// This actually returns an instance of DiagnosticBuilder which emits the
/// diagnostics when it is destroyed.
///
/// Built-in (TSAR and Clang) and custom diagnostics may be used
clang::DiagnosticBuilder toDiag(clang::DiagnosticsEngine &Diags,
    clang::SourceLocation Loc, unsigned int DiagId);

/// \brief Issue the message to the client.
///
/// This actually returns an instance of DiagnosticBuilder which emits the
/// diagnostics when it is destroyed.
///
/// Built-in (TSAR and Clang) and custom diagnostics may be used
clang::DiagnosticBuilder toDiag(clang::DiagnosticsEngine &Diags,
    unsigned int DiagId);
}
#endif//TSAR_CLANG_DIAGNOSTIC_H
