//===--- Diagnostic.h - C Language Family Diagnostic Handling ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Implements the Diagnostic-related interfaces.
//
//===----------------------------------------------------------------------===//

#include "Diagnostic.h"

namespace tsar {
clang::DiagnosticBuilder toDiag(clang::DiagnosticsEngine &Diags,
    clang::SourceLocation Loc, unsigned int DiagId) {
  switch (DiagId) {
  default: return Diags.Report(Loc, DiagId);
#define DIAG(ENUM,LEVEL,DESC) \
  case clang::diag::ENUM: \
    { \
      unsigned CustomId = Diags.getCustomDiagID( \
        clang::DiagnosticsEngine::LEVEL, DESC); \
      return Diags.Report(Loc, CustomId); \
    }
#include "DiagnosticKinds.inc"
#undef DIAG
  }
}

clang::DiagnosticBuilder toDiag(clang::DiagnosticsEngine &Diags,
    unsigned int DiagId) {
  switch (DiagId) {
  default: return Diags.Report(DiagId);
#define DIAG(ENUM,LEVEL,DESC) \
  case clang::diag::ENUM: \
    { \
      unsigned CustomId = Diags.getCustomDiagID( \
        clang::DiagnosticsEngine::LEVEL, DESC); \
      return Diags.Report(CustomId); \
    }
#include "DiagnosticKinds.inc"
#undef DIAG
  }
}
}
