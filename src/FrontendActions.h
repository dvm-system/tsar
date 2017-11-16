//===-- FrontendActions.h - Useful Frontend Actions -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements useful frontend actions.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FRONTEND_ACTIONS_H
#define TSAR_FRONTEND_ACTIONS_H

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
}
#endif//TSAR_FRONTEND_ACTIONS_H
