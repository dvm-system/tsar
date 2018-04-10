//===--- tsar_pragma_action.cpp ---- TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements front-end action which is necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#include "tsar_pragma_action.h"

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Preprocessor.h>


bool tsar::PragmaAction::BeginSourceFileAction(clang::CompilerInstance& CI,
  llvm::StringRef Filename) {
  mPP = &CI.getPreprocessor();
  for (auto& Handler : mPragmaHandlers) {
    mPP->AddPragmaHandler(Handler.get());
  }
  return true;
}

void tsar::PragmaAction::EndSourceFileAction() {
  for (auto& Handler : mPragmaHandlers) {
    mPP->RemovePragmaHandler(Handler.get());
  }
  return;
}

bool tsar::GenPCHPragmaAction::BeginSourceFileAction(
  clang::CompilerInstance& CI, llvm::StringRef Filename) {
  CI.getLangOpts().CompilingPCH = true;

  mPP = &CI.getPreprocessor();
  for (auto& Handler : mPragmaHandlers) {
    mPP->AddPragmaHandler(Handler.get());
  }
  return WrapperFrontendAction::BeginSourceFileAction(CI, Filename);
}

void tsar::GenPCHPragmaAction::EndSourceFileAction() {
  WrapperFrontendAction::EndSourceFileAction();
  for (auto& Handler : mPragmaHandlers) {
    mPP->RemovePragmaHandler(Handler.get());
  }
  return;
}
