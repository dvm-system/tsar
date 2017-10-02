//===--- Diagnostic.h - C Language Family Diagnostic Handling ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Defines the Diagnostic-related interfaces.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_DIAGNOSTIC_H
#define TSAR_DIAGNOSTIC_H

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceLocation.h>

namespace clang {
namespace diag {
/// Identifiers of built-in diagnostics of TSAR.
enum {
#define DIAG(ENUM,LEVEL,DESC) ENUM,
#include "DiagnosticKinds.inc"
#undef DIAG
  NUM_BUILTIN_TSAR_DIAGNOSTICS
};
}
}

namespace tsar {
/// \brief Issue the message to the client.
///
/// This actually returns an instance of DiagnosticBuilder which emits the
/// diagnostics when it is destroyed.
///
/// Built-in (TSAR and Clang) and custom diagnostics may be used
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

/// \brief Issue the message to the client.
///
/// This actually returns an instance of DiagnosticBuilder which emits the
/// diagnostics when it is destroyed.
///
/// Built-in (TSAR and Clang) and custom diagnostics may be used
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
#endif//TSAR_DIAGNOSTIC_H