//===---- Diagnostic.h -- Low-level Diagnostic Handling ---------*- C++ -*-===//
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
// This file implements functions to emit different low-level diagnostics.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_IR_DIAGNOSTIC_H
#define TSAR_IR_DIAGNOSTIC_H

#include <llvm/IR/DiagnosticInfo.h>

namespace tsar {
inline void emitUnableShrink(llvm::LLVMContext &Ctx, const llvm::Function &F,
    const llvm::DebugLoc &Loc,
    llvm::DiagnosticSeverity Severity = llvm::DS_Error) {
  llvm::DiagnosticInfoUnsupported Diag(
    F, "unable to shrink location", Loc, Severity);
  Ctx.diagnose(Diag);
}

inline void emitTypeOverflow(llvm::LLVMContext &Ctx, const llvm::Function &F,
    const llvm::DebugLoc &Loc, const llvm::Twine &NoteMsg,
    llvm::DiagnosticSeverity Severity = llvm::DS_Error) {
  if (!NoteMsg.isTriviallyEmpty()) {
    llvm::DiagnosticInfoUnsupported Diag(F, NoteMsg, Loc, llvm::DS_Note);
    Ctx.diagnose(Diag);
  }
  llvm::DiagnosticInfoUnsupported Diag(F, "type overflow", Loc, Severity);
  Ctx.diagnose(Diag);
}
}
#endif//TSAR_IR_DIANGOSTIC_H
