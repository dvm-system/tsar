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
}

namespace clang {
/// Main front-end action to analyse and transform sources.
class AnalysisActionBase : public ASTFrontendAction {
public:
  /// This is a list of supported actions.
  enum Kind {
    FIRST_KIND,
    /// Perform a main analysis and transformations action.
    KIND_ANALYSIS = FIRST_KIND,
    /// Emit only LLVM IR and do not perform any analysis.
    KIND_EMIT_LLVM,
    /// Perform low-level (LLVM IR) instrumentation.
    KIND_INSTRUMENT,
    /// Perform set of transformation passese with the same
    /// transformation context.
    KIND_TRANSFORM,
    LAST_KIND = KIND_TRANSFORM,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };

  /// Creates AST Consumer.
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
    StringRef InFile) override;

  /// Return true because this action supports use with IR files.
  bool hasIRSupport() const override;

  /// Executes action, evaluateion of IR files is also supported.
  void ExecuteAction() override;

protected:
  /// Creates specified action.
  ///
  /// \attention
  /// - If kind of new action is KIND_TRANSFORM then transformation context
  /// must be specified. It will be used to access to transformation engine from
  /// LLVM transformation passes. The context will not be taken under control of
  /// this class.
  /// - If transformation context and command line both are specified the
  /// command line will  be ignored and command line from transformation context
  /// will be used.
  AnalysisActionBase(Kind AK, tsar::TransformationContext *Ctx,
    ArrayRef<std::string> CL);

private:
  Kind mKind;
  tsar::TransformationContext *mTransformContext;
  std::vector<std::string> mCommandLine;
};

class MainAction : public AnalysisActionBase {
public:
  explicit MainAction(ArrayRef<std::string> CL);
};

class EmitLLVMAnalysisAction : public AnalysisActionBase {
public:
  explicit EmitLLVMAnalysisAction();
};

class TransformationAction : public AnalysisActionBase {
public:
  explicit TransformationAction(tsar::TransformationContext &Ctx);
};

class InstrumentationAction : public AnalysisActionBase {
public:
  explicit InstrumentationAction();
};

/// Creats an analysis/transformations actions factory.
template <class ActionTy, class ArgTy>
std::unique_ptr<tooling::FrontendActionFactory>
newAnalysisActionFactory(ArgTy Arg) {
  class AnalysisActionFactory : public tooling::FrontendActionFactory {
  public:
    AnalysisActionFactory(ArgTy Arg) : mArg(std::move(Arg)) {}
    clang::FrontendAction *create() override { return new ActionTy(mArg); }
  private:
    ArgTy mArg;
  };
  return std::unique_ptr<tooling::FrontendActionFactory>(
    new AnalysisActionFactory(std::move(Arg)));
}
}
#endif//TSAR_ACTION_H
