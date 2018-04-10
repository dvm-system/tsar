//===-- tsar_pragma_action.h - Pragma Action -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements pragma action.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRAGMA_ACTION_H
#define TSAR_PRAGMA_ACTION_H

#include "FrontendActions.h"

#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/Pragma.h>
#include <clang/Tooling/Tooling.h>

namespace tsar {

/// This action is used to setup pragma handlers.
class PragmaAction : public clang::PreprocessOnlyAction {
private:
  std::vector<std::unique_ptr<clang::PragmaHandler>>& mPragmaHandlers;
  clang::Preprocessor* mPP;
public:
  PragmaAction(
    std::vector<std::unique_ptr<clang::PragmaHandler>>& PragmaHandlers)
    : mPragmaHandlers(PragmaHandlers) {}

  bool BeginSourceFileAction(clang::CompilerInstance& CI,
    llvm::StringRef Filename) override;

  void EndSourceFileAction() override;
};

/// This action is used to setup pragma handlers.
class GenPCHPragmaAction : public PublicWrapperFrontendAction {
private:
  std::vector<std::unique_ptr<clang::PragmaHandler>>& mPragmaHandlers;
  clang::Preprocessor* mPP;
public:
  GenPCHPragmaAction(std::unique_ptr<clang::FrontendAction> WrappedAction,
    std::vector<std::unique_ptr<clang::PragmaHandler>>& PragmaHandlers)
    : PublicWrapperFrontendAction(WrappedAction.release()),
    mPragmaHandlers(PragmaHandlers) {}

  bool BeginSourceFileAction(clang::CompilerInstance& CI,
    llvm::StringRef Filename) override;

  void EndSourceFileAction() override;
};

/// Creates an analysis/transformations actions factory.
template <class ActionTy, class FirstTy>
std::unique_ptr<clang::tooling::FrontendActionFactory>
newPragmaActionFactory(FirstTy First) {
  class PragmaActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    PragmaActionFactory(FirstTy F) :
      mFirst(std::move(F)) {}
    clang::FrontendAction *create() override {
      return new ActionTy(mFirst);
    }
  private:
    FirstTy mFirst;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new PragmaActionFactory(std::move(First)));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <class ActionTy, class AdaptorTy, class AdaptorArgTy>
  std::unique_ptr<clang::tooling::FrontendActionFactory>
  newPragmaActionFactory(AdaptorArgTy AdaptorArg) {
  class PragmaActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    PragmaActionFactory(AdaptorArgTy A) :
      mAdaptorArg(std::move(A)) {}
    clang::FrontendAction *create() override {
      std::unique_ptr<clang::FrontendAction> Action(
        new ActionTy());
      return new AdaptorTy(std::move(Action), mAdaptorArg);
    }
  private:
    AdaptorArgTy mAdaptorArg;
  };
  return std::unique_ptr<clang::tooling::FrontendActionFactory>(
    new PragmaActionFactory(std::move(AdaptorArg)));
}

}
#endif//TSAR_PRAGMA_ACTION_H
