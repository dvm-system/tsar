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
template <class ActionTy, class FirstTy, class SecondTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newAnalysisActionFactory(FirstTy First, SecondTy Second) {
  class AnalysisActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(FirstTy F, SecondTy S) :
      mFirst(std::move(F)), mSecond(std::move(S)) {}
    std::unique_ptr<clang::FrontendAction> create() override {
      return std::unique_ptr<clang::FrontendAction>(
        new ActionTy(mFirst, mSecond));
    }
  private:
    FirstTy mFirst;
    SecondTy mSecond;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new AnalysisActionFactory(std::move(First), std::move(Second)));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <class ActionTy, class AdaptorTy, class FirstTy, class SecondTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newAnalysisActionFactory(FirstTy First, SecondTy Second) {
  class AnalysisActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(FirstTy F, SecondTy S) :
      mFirst(std::move(F)), mSecond(std::move(S)) {}
    std::unique_ptr<clang::FrontendAction> create() override {
      std::unique_ptr<clang::FrontendAction> Action(
        new ActionTy(mFirst, mSecond));
      return std::unique_ptr<clang::FrontendAction>(
        new AdaptorTy(std::move(Action)));
    }
  private:
    FirstTy mFirst;
    SecondTy mSecond;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new AnalysisActionFactory(std::move(First), std::move(Second)));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <class ActionTy, class AdaptorTy,
          class FirstTy, class SecondTy, class AdaptorArgTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newAnalysisActionFactory(
    FirstTy First, SecondTy Second, AdaptorArgTy AdaptorArg) {
  class AnalysisActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(FirstTy F, SecondTy S, AdaptorArgTy A) :
      mFirst(std::move(F)), mSecond(std::move(S)), mAdaptorArg(std::move(A)) {}
    std::unique_ptr<clang::FrontendAction> create() override {
      std::unique_ptr<clang::FrontendAction> Action(
        new ActionTy(mFirst, mSecond));
      return std::unique_ptr<clang::FrontendAction>(
        new AdaptorTy(std::move(Action), mAdaptorArg));
    }
  private:
    FirstTy mFirst;
    SecondTy mSecond;
    AdaptorArgTy mAdaptorArg;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new AnalysisActionFactory(
      std::move(First), std::move(Second), std::move(AdaptorArg)));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <class ActionTy, class AdaptorTy,
          class FirstTy, class SecondTy,
          class AdaptorFirstTy, class AdaptorSecondTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newAnalysisActionFactory(FirstTy First, SecondTy Second,
    AdaptorFirstTy AdaptorFirst, AdaptorSecondTy AdaptorSecond) {
  class AnalysisActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(FirstTy F, SecondTy S,
        AdaptorFirstTy AF, AdaptorSecondTy AS) :
      mFirst(std::move(F)), mSecond(std::move(S)),
      mAdaptorFirst(std::move(AF)), mAdaptorSecond(std::move(AS)) {}
    std::unique_ptr<clang::FrontendAction> create() override {
      std::unique_ptr<clang::FrontendAction> Action(
        new ActionTy(mFirst, mSecond));
      return std::unique_ptr<clang::FrontendAction>(
        new AdaptorTy(std::move(Action), mAdaptorFirst, mAdaptorSecond));
    }
  private:
    FirstTy mFirst;
    SecondTy mSecond;
    AdaptorFirstTy mAdaptorFirst;
    AdaptorSecondTy mAdaptorSecond;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new AnalysisActionFactory(std::move(First), std::move(Second),
      std::move(AdaptorFirst), std::move(AdaptorSecond)));
}
}
namespace clang {
/// Creates an frontend actions factory with adaptor.
template <class ActionTy, class AdaptorTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newFrontendActionFactory() {
  class SimpleFrontendActionFactory :
    public clang::tooling::FrontendActionFactory {
  public:
    SimpleFrontendActionFactory() {}
    std::unique_ptr<clang::FrontendAction> create() override {
      std::unique_ptr<clang::FrontendAction> Action(new ActionTy());
      return std::unique_ptr<clang::FrontendAction>(
        new AdaptorTy(std::move(Action)));
    }
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new SimpleFrontendActionFactory());
}

/// Creates an frontend actions factory with adaptor.
template <class ActionTy, class AdaptorTy, class AdaptorArgTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newFrontendActionFactory(AdaptorArgTy AdaptorArg) {
  class SimpleFrontendActionFactory :
    public clang::tooling::FrontendActionFactory {
  public:
    SimpleFrontendActionFactory(AdaptorArgTy A) : mAdaptorArg(std::move(A)) {}
    std::unique_ptr<clang::FrontendAction> create() override {
      std::unique_ptr<clang::FrontendAction> Action(new ActionTy());
      return std::unique_ptr<clang::FrontendAction>(
        new AdaptorTy(std::move(Action), mAdaptorArg));
    }
  private:
    AdaptorArgTy mAdaptorArg;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new SimpleFrontendActionFactory(std::move(AdaptorArg)));
}
}
#endif//TSAR_ACTION_H
