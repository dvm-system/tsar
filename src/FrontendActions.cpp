//===-- FrontendActions.cpp - Useful Frontend Actions -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements useful frontend actions.
//
//===----------------------------------------------------------------------===//

#include "FrontendActions.h"
#include "DefaultPragmaHandlers.h"
#include "tsar_pragma.h"
#include "tsar_utility.h"
#include <clang/Frontend/ASTConsumers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/ADT/STLExtras.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

std::unique_ptr<ASTConsumer>
tsar::ASTPrintAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return CreateASTPrinter(nullptr, "");
}

std::unique_ptr<ASTConsumer>
tsar::ASTDumpAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return CreateASTDumper(nullptr, "", true, true, true);
}

GenPCHPragmaAction::~GenPCHPragmaAction() {
  DeleteContainerPointers(mNamespaces);
}

bool GenPCHPragmaAction::BeginSourceFileAction(CompilerInstance &CI) {
  CI.getLangOpts().CompilingPCH = true;
  mPP = &CI.getPreprocessor();
  AddPragmaHandlers(*mPP, mNamespaces);
  if (!WrapperFrontendAction::BeginSourceFileAction(CI)) {
    DeleteContainerPointers(mNamespaces);
    return false;
  }
  return true;
}

void GenPCHPragmaAction::EndSourceFileAction() {
  WrapperFrontendAction::EndSourceFileAction();
  RemovePragmaHandlers(*mPP, mNamespaces.begin(), mNamespaces.end());
  DeleteContainerPointers(mNamespaces);
}
