//===--- tsar_action.h ------ TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file contains front-end actions which is necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ACTION_H
#define TSAR_ACTION_H

#include <clang/Tooling/Tooling.h>
#include <memory>

namespace tsar {
class TransformationContext;
class QueryManager;

/// Base front-end action to analyze and transform sources. Concrete actions
/// must inherit this.
class ActionBase : public clang::ASTFrontendAction {
public:
  /// \brief Callback at the start of processing a single input.
  ///
  /// \return True on success; on failure ExecutionAction() and
  /// EndSourceFileAction() will not be called.
  bool BeginSourceFileAction(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;

  /// \brief Callback at the end of processing a single input.
  ///
  /// This is guaranteed to only be called following a successful call to
  /// BeginSourceFileAction (and BeginSourceFile).
  void EndSourceFileAction() override;

  /// Return true because this action supports use with IR files.
  bool hasIRSupport() const override { return true; }

  /// Executes action, evaluation of IR files is also supported.
  void ExecuteAction() override;

protected:
  /// Creates specified action.
  explicit ActionBase(QueryManager *QM);

  tsar::QueryManager *mQueryManager;
};

/// Main action to perform analysis and transformation for a single
/// compilation unit.
class MainAction : public ActionBase {
public:
  /// Constructor.
  MainAction(llvm::ArrayRef<std::string> CL, QueryManager *QM);

  /// Creates AST Consumer.
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;

private:
  std::unique_ptr<TransformationContext> mTfmCtx;
};

/// Creates an analysis/transformations actions factory.
template <class ActionTy, class FirstTy, class SecondTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newAnalysisActionFactory(FirstTy First, SecondTy Second) {
  class AnalysisActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(FirstTy F, SecondTy S) :
      mFirst(std::move(F)), mSecond(std::move(S)) {}
    clang::FrontendAction *create() override {
      return new ActionTy(mFirst, mSecond);
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
    clang::FrontendAction *create() override {
      std::unique_ptr<clang::FrontendAction> Action(
        new ActionTy(mFirst, mSecond));
      return new AdaptorTy(std::move(Action), mAdaptorArg);
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
}
#endif//TSAR_ACTION_H
