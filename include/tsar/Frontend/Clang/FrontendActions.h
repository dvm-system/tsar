//===-- FrontendActions.h - Useful Frontend Actions -------------*- C++ -*-===//
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

#ifndef TSAR_FRONTEND_ACTIONS_H
#define TSAR_FRONTEND_ACTIONS_H

#include "tsar/Frontend/Clang/PragmaHandlers.h"
#include <clang/Frontend/FrontendAction.h>

namespace tsar {
/// \brief This is an action which wraps other action.
///
/// This is similar to `clang::WrapperFrontendAction` but implement public
/// methods to access a wrapped action.
/// \attention This manages wrapped action, so do not delete it outside
/// this class.
class PublicWrapperFrontendAction : public clang::WrapperFrontendAction {
public:
  explicit PublicWrapperFrontendAction(clang::FrontendAction *WrappedAction) :
    clang::WrapperFrontendAction(
      std::unique_ptr<clang::FrontendAction>(WrappedAction)),
    mWrappedAction(WrappedAction) {}

  clang::FrontendAction & getWrappedAction() noexcept {
    return *mWrappedAction;
  }
  const clang::FrontendAction & getWrappedAction() const noexcept {
    return *mWrappedAction;
  }
private:
  clang::FrontendAction *mWrappedAction;
};

/// This action is used to print AST.
class ASTPrintAction : public clang::ASTFrontendAction {
protected:
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;
};

/// This action is used to dump AST.
class ASTDumpAction : public clang::ASTFrontendAction {
protected:
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;
};

/// This action wraps other action and setups pragma handlers.
class GenPCHPragmaAction : public PublicWrapperFrontendAction {
public:
  GenPCHPragmaAction(std::unique_ptr<clang::FrontendAction> WrappedAction)
    : PublicWrapperFrontendAction(WrappedAction.release()) {}

  bool BeginSourceFileAction(clang::CompilerInstance& CI) override;
  void EndSourceFileAction() override;
private:
  llvm::SmallVector<PragmaNamespaceReplacer *, 1> mNamespaces;
  clang::Preprocessor* mPP = nullptr;
};
}
#endif//TSAR_FRONTEND_ACTIONS_H
