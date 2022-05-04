//===--- Action.h ------- TSAR Frontend Action (Clang) ----------*- C++ -*-===//
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
// This file contains front-end actions which is necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_ACTION_H
#define TSAR_CLANG_ACTION_H

#include "tsar/Frontend/ActionFactory.h"
#include "tsar/Core/TransformationContext.h"
#include <clang/Tooling/Tooling.h>

namespace tsar {
class QueryManager;

/// Base front-end action to analyze and transform sources.
class ClangMainAction : public clang::ASTFrontendAction {
public:
  ClangMainAction(const clang::tooling::CompilationDatabase &Compilations,
                  QueryManager &QM)
      : mTfmInfo(Compilations), mQueryManager(QM) {}

  /// Callback at the start of processing a single input.
  ///
  /// \return True on success; on failure ExecutionAction() and
  /// EndSourceFileAction() will not be called.
  bool BeginSourceFileAction(clang::CompilerInstance &CI) override;

  /// Callback at the end of processing a single input.
  ///
  /// This is guaranteed to only be called following a successful call to
  /// BeginSourceFileAction (and BeginSourceFile).
  bool shouldEraseOutputFiles() override;

  /// Create AST Consumer.
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;

private:
  TransformationInfo mTfmInfo;
  QueryManager &mQueryManager;
};

/// Creates an analysis/transformations actions factory.
template <typename ActionT, typename... ArgT>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newActionFactory(std::tuple<ArgT...> Args) {
  return newActionFactory<clang::tooling::FrontendActionFactory,
                          clang::FrontendAction, ActionT>(std::move(Args));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <typename ActionT, typename AdaptorT, typename... ActionArgT,
          typename... AdaptorArgT>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newClangActionFactory(std::tuple<ActionArgT...> ActionArgs = {},
                      std::tuple<AdaptorArgT...> AdaptorArgs = {}) {
  return newActionFactory<clang::tooling::FrontendActionFactory,
                          clang::FrontendAction, ActionT, AdaptorT>(
      std::move(ActionArgs), std::move(AdaptorArgs));
}
}
#endif//TSAR_CLANG_ACTION_H
