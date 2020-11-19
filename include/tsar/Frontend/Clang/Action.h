//===--- Action.h ----------- TSAR Frontend Action --------------*- C++ -*-===//
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

#ifndef TSAR_ACTION_H
#define TSAR_ACTION_H

#include "tsar/Core/TransformationContext.h"
#include <bcl/utility.h>
#include <clang/Tooling/Tooling.h>
#include <memory>

namespace tsar {
class TransformationInfo;
class QueryManager;

/// Base front-end action to analyze and transform sources.
class MainAction : public clang::ASTFrontendAction {
public:
  MainAction(llvm::ArrayRef<std::string> CL, QueryManager *QM);

  /// Callback at the start of processing a single input.
  ///
  /// \return True on success; on failure ExecutionAction() and
  /// EndSourceFileAction() will not be called.
  bool BeginSourceFileAction(clang::CompilerInstance &CI) override;

  /// Callback at the end of processing a single input.
  ///
  /// This is guaranteed to only be called following a successful call to
  /// BeginSourceFileAction (and BeginSourceFile).
  void EndSourceFileAction() override;

  /// Return true because this action supports use with IR files.
  bool hasIRSupport() const override { return true; }

  /// Execute action, evaluation of IR files is also supported.
  void ExecuteAction() override;

  /// Create AST Consumer.
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;

private:
  tsar::QueryManager *mQueryManager;
  std::unique_ptr<TransformationInfo> mTfmInfo;
};

/// Creates an analysis/transformations actions factory.
template <typename ActionT, typename... ArgT>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newActionFactory(std::tuple<ArgT...> Args) {
  class ActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    explicit ActionFactory(std::tuple<ArgT...> Args) : mArgs{std::move(Args)} {}
    std::unique_ptr<clang::FrontendAction> create() override {
      return std::unique_ptr<clang::FrontendAction>(
          bcl::make_unique_piecewise<ActionT>(mArgs).release());
    }

  private:
    std::tuple<ArgT...> mArgs;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
      new ActionFactory(std::move(Args)));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <typename ActionT, typename AdaptorT, typename... ActionArgT,
          typename... AdaptorArgT>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newActionFactory(std::tuple<ActionArgT...> ActionArgs = {},
                 std::tuple<AdaptorArgT...> AdaptorArgs = {}) {
  class ActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    ActionFactory(std::tuple<ActionArgT...> ActionArgs,
                  std::tuple<AdaptorArgT...> AdaptorArgs)
        : mActionArgs{std::move(ActionArgs)}
        , mAdaptorArgs{std::move(AdaptorArgs)} {}
    std::unique_ptr<clang::FrontendAction> create() override {
      std::unique_ptr<clang::FrontendAction> Action{
          bcl::make_unique_piecewise<ActionT>(mActionArgs).release()};
      return std::unique_ptr<clang::FrontendAction>(
          bcl::make_unique_piecewise<AdaptorT>(
              std::tuple_cat(std::forward_as_tuple(std::move(Action)),
                             mAdaptorArgs))
              .release());
    }

  private:
    std::tuple<ActionArgT...> mActionArgs;
    std::tuple<AdaptorArgT...> mAdaptorArgs;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
      new ActionFactory(std::move(ActionArgs), std::move(AdaptorArgs)));
}
}
#endif//TSAR_ACTION_H
