
//=== Diagnostic.h - Fortran Language Family Diagnostic Handling *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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

#ifndef TSAR_FLANG_DIAGNOSTIC_H
#define TSAR_FLANG_DIAGNOSTIC_H

#include "tsar/Support/Diagnostic.h"
#include <flang/Semantics/semantics.h>
#include <llvm/ADT/SmallString.h>

namespace tsar {
// Return fixed part of a message which describes a specified diagnostic.
const DiagnosticInfo * getFlangMessageText(unsigned int DiagId,
    llvm::SmallVectorImpl<char> &Text);

/// Issue the message to the client.
template <typename... ArgT>
Fortran::parser::Message &toDiag(Fortran::semantics::SemanticsContext &Ctx,
    Fortran::parser::CharBlock Loc, unsigned int DiagId, ArgT &&... Args) {
  llvm::SmallString<128> Text;
  auto *Diag{getFlangMessageText(DiagId, Text)};
  assert(Diag && !Text.empty() && "Unknown diagnostic ID!");
  // We use MessageFormattedText around MessageFixedText, because
  // MessageFixedText does not make a copy of text and it points to a
  // temporary object Text.
  return Ctx.Say(Loc, Fortran::parser::MessageFormattedText{
                          Fortran::parser::MessageFixedText{
                              Text.data(), Text.size(),
                              Diag->isError()
                                  ? Fortran::parser::Severity::Error
                                  : Fortran::parser::Severity::Warning},
                          std::forward<ArgT>(Args)...});
}

/// Issue the message to the client.
template <typename... ArgT>
Fortran::parser::Message &toDiag(Fortran::semantics::SemanticsContext &Ctx,
                                 unsigned int DiagId, ArgT &&... Args) {
  auto Loc{Ctx.allCookedSources().allSources().GetFirstFileProvenance()};
  assert(Loc && "At least one file must be parsed!");
  return toDiag(Ctx, *Loc, DiagId, std::forward<ArgT>(Args)...);
}
} // namespace tsar

#endif//TSAR_FLANG_DIAGNOSTIC_H
