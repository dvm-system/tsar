//===-- FrontendActions.cpp - Useful Frontend Actions -----------*- C++ -*-===//
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
// This file implements useful frontend actions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/FrontendActions.h"
#include "tsar/Frontend/Clang/DefaultPragmaHandlers.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include <clang/AST/ASTDumperUtils.h>
#include <clang/Frontend/ASTConsumers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/ADT/iterator.h>
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
  return CreateASTDumper(nullptr, "", true, true, true, true, ADOF_Default);
}

bool GenPCHPragmaAction::BeginSourceFileAction(CompilerInstance &CI) {
  CI.getLangOpts().CompilingPCH = true;
  mPP = &CI.getPreprocessor();
  AddPragmaHandlers(*mPP, mNamespaces);
  if (!WrapperFrontendAction::BeginSourceFileAction(CI)) {
    return false;
  }
  return true;
}

void GenPCHPragmaAction::EndSourceFileAction() {
  WrapperFrontendAction::EndSourceFileAction();
  using ItrTy =
    pointer_iterator<pointee_iterator<decltype(mNamespaces)::iterator>>;
  RemovePragmaHandlers(*mPP,
    ItrTy(mNamespaces.begin()), ItrTy(mNamespaces.end()));
}
