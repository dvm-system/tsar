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
#include <clang/Frontend/ASTConsumers.h>
#include <clang/Frontend/CompilerInstance.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

std::unique_ptr<ASTConsumer>
tsar::ASTPrintAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return CreateASTPrinter(nullptr, "");
}

std::unique_ptr<ASTConsumer>
tsar::ASTDumpAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return CreateASTDumper("", true, false);
}
