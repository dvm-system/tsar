//===--- tsar_action.h ------ TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file contains front-end action which is necessary to analyze sources.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ACTION_H
#define TSAR_ACTION_H

#include "clang/Frontend/FrontendAction.h"
#include <memory>

namespace clang {

class AnalysisAction : public ASTFrontendAction {
public:
  /// This is a list of supported actions.
  enum Kind {
    FIRST_KIND,
    KIND_ANALYSIS = FIRST_KIND,
    KIND_DEFAULT = KIND_ANALYSIS,
    KIND_EMIT_LLVM,
    LAST_KIND = KIND_EMIT_LLVM,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };

  /// Creates specified analysis action.
  explicit AnalysisAction(Kind AK = KIND_DEFAULT);

private:
  /// Creates AST Consumer.
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
    StringRef InFile) override;

  /// Return true if this action supports use with IR files.
  bool hasIRSupport() const override;

  /// Execute action.
  void ExecuteAction() override;

  Kind mKind;
};

class EmitLLVMAnalysisAction : public AnalysisAction {
public:
  EmitLLVMAnalysisAction();
};
}

#endif//TSAR_ACTION_H
