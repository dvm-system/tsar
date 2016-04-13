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
class RewriterContext;
}

namespace clang {
/// Main front-end action to analyse and transform sources.
class AnalysisActionBase : public ASTFrontendAction {
public:
  /// This is a list of supported actions.
  enum Kind {
    FIRST_KIND,
    /// Perform analysis.
    KIND_ANALYSIS = FIRST_KIND,
    /// Emit only LLVM IR and do not perform any analysis.
    KIND_EMIT_LLVM,
    /// Supplemented rewriter context by additional information to configure
    /// rewriter.
    KIND_CONFIGURE_REWRITER,
    LAST_KIND = KIND_CONFIGURE_REWRITER,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };

  /// Creates specified analysis action.
  ///
  /// \attention
  /// - If kind of new action is KIND_CONFIGURE_REWRITER then rewriter
  /// context must be specified. It will be supplemented by additional
  /// information to configure rewrite for an evaluated source. The context will
  /// not be taken under control of this class, so do not free it separately.
  /// - If a rewriter context and a command line both are specified the command
  /// line from the rewriter context must be equal to the specified command
  /// line.
  AnalysisActionBase(Kind AK, tsar::RewriterContext *Ctx,
    ArrayRef<std::string> CL);

  /// Creates AST Consumer.
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
    StringRef InFile) override;

  /// Return true because this action supports use with IR files.
  bool hasIRSupport() const override;

  /// Executes action, evaluateion of IR files is also supported.
  void ExecuteAction() override;

private:
  Kind mKind;
  tsar::RewriterContext *mRewriterContext;
  std::vector<std::string> mCommandLine;
};

class AnalysisAction : public AnalysisActionBase {
public:
  explicit AnalysisAction(ArrayRef<std::string> CL);
};

class EmitLLVMAnalysisAction : public AnalysisActionBase {
public:
  explicit EmitLLVMAnalysisAction();
};

class RewriterInitAction : public AnalysisActionBase {
public:
  explicit RewriterInitAction(tsar::RewriterContext *Ctx);
};

/// Creats an analysis actions factory.
template <class ActionTy, class ArgTy>
std::unique_ptr<tooling::FrontendActionFactory> newAnalysisActionFactory(
    ArgTy &Arg) {
  class AnalysisActionFactory : public tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(ArgTy &Arg) : mArg(Arg) {}
    clang::FrontendAction *create() override { return new ActionTy(mArg); }
  private:
    ArgTy mArg;
  };
  return std::unique_ptr<tooling::FrontendActionFactory>(
    new AnalysisActionFactory(Arg));
}
}
#endif//TSAR_ACTION_H
