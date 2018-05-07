//===------ tsar_query.h --------- Query Manager ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This defines a query manager that controls construction of response when
// analysis and transformation tool is launched.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_QUERY_H
#define TSAR_QUERY_H

#include "PassGroupRegistry.h"
#include "ASTImportInfo.h"
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <vector>

namespace llvm {
class Pass;
class PassInfo;
class Module;
class raw_pwrite_stream;

namespace legacy {
class PassManager;
}
}

namespace clang {
class CompilerInstance;
class CodeGenOptions;
}

namespace tsar {
struct GlobalOptions;
class TransformationContext;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// This is a query manager that controls construction of response when analysis
/// and transformation tool is launched.
///
/// At least run() method should be overridden to specify a whole sequence of
/// performed passes.
class QueryManager {
public:
  /// \brief Callback at the start of processing a single input.
  ///
  /// \return True on success, on failure run() passes will not be called.
  virtual bool beginSourceFile(clang::CompilerInstance &, llvm::StringRef) {
    return true;
  }

  /// \brief Callback at the end of processing a single input.
  ///
  /// This is guaranteed to only be called following a successful call to
  /// beginSourceFile().
  virtual void endSourceFile() {}

  /// Analysis the specified module and transforms source file associated with
  /// it if rewriter context is specified.
  ///
  /// \attention Neither module nor transformation context is not going to be
  /// taken under control.
  virtual void run(llvm::Module *M, tsar::TransformationContext *Ctx) = 0;

  /// Initializes external storage to access information about import process
  /// if necessary.
  virtual ASTImportInfo * initializeImportInfo() { return nullptr; }
};

/// This specify default pass sequence, which can be configured by command line
/// options in some ways.
class DefaultQueryManager: public QueryManager {
public:
  /// Available processing steps
  enum ProcessingStep : uint8_t {
    InitialStep = 0,
    BeforeTfmAnalysis = 1u << 0,
    AfterSroaAnalysis = 1u << 1,
    AfterLoopRotateAnalysis = 1u << 2,
    LLVM_MARK_AS_BITMASK_ENUM(AfterLoopRotateAnalysis)
  };

private:
  /// Recursively calculates the maximum number of available processing steps.
  static constexpr uint8_t numberOfSteps(
      std::underlying_type<ProcessingStep>::type Tail) noexcept {
    return Tail == 0 ? 0 : numberOfSteps(Tail >> 1) + 1;
  }

  /// Recursively calculates bitwise OR of all available steps.
  static constexpr std::underlying_type<ProcessingStep>::type allSteps(
      std::underlying_type<ProcessingStep>::type Curr, uint8_t StepNum) {
    // Note, that operator|() is overloaded for ProcessingStep, however, it
    // is not constexpr function. So, we use its underlying type instead.
    // function, other
    return StepNum == 0 ? Curr | 1u  :
      allSteps(Curr | (1u << StepNum), StepNum - 1);
  }

  /// Returns a list of initialized output passes.
  static PassGroupRegistry & getOutputPassRegistry() noexcept {
    static PassGroupRegistry Passes;
    return Passes;
  }

  /// Returns a list of initialized print passes.
  static PassGroupRegistry & getPrintPassRegistry() noexcept {
    static PassGroupRegistry Passes;
    return Passes;
  }

public:
  /// Returns the number of processing steps.
  static constexpr uint8_t numberOfSteps() noexcept {
    return numberOfSteps(LLVM_BITMASK_LARGEST_ENUMERATOR);
  }

  /// Returns bitwise OR of all available steps.
  static constexpr ProcessingStep allSteps() noexcept {
    return (ProcessingStep)allSteps(InitialStep, numberOfSteps() - 1);
  }

  /// List of passes.
  typedef std::vector<const llvm::PassInfo *> PassList;


  /// Returns a list of initialized output passes.
  struct OutputPassGroup {
    static inline PassGroupRegistry & getPassRegistry() noexcept {
      return DefaultQueryManager::getOutputPassRegistry();
    }
  };

  /// Returns a list of initialized print passes.
  struct PrintPassGroup {
    static inline PassGroupRegistry & getPassRegistry() noexcept {
      return DefaultQueryManager::getPrintPassRegistry();
    }
  };

  /// Default constructor.
  DefaultQueryManager() {}

  /// Creates query manager.
  ///
  /// \param [in] GlobalOptions This specifies a list of global options,
  /// that may be accessed by multiple passes. Global options may be accessed
  /// not only in this query manager, so the manager should not owns their.
  /// \param [in] OutputPasses This specifies passes that should be run to
  /// show program exploration results.
  /// \param [in] PrintPasses This specifies passes, the results of which should
  /// be printed. Specified passes must override their print() methods.
  /// \param [in] PrintSteps This specifies processing steps, the result of
  /// which should be printed. This is a bit list of steps.
  DefaultQueryManager(const GlobalOptions *Options,
      const PassList &OutputPasses, const PassList &PrintPasses,
      ProcessingStep PrintSteps = allSteps()) : mGlobalOptions(Options),
    mOutputPasses(OutputPasses), mPrintPasses(PrintPasses),
    mPrintSteps(PrintSteps) {}

  /// Runs default sequence of passes.
  void run(llvm::Module *M, tsar::TransformationContext *Ctx) override;

private:
  /// Updates pass manager. Adds a specified pass and a pass to print its result
  // if `PrintResult` is set to 'true`.
  void addWithPrint(llvm::Pass *P, bool PrintResult,
    llvm::legacy::PassManager &Passes);

  PassList mOutputPasses;
  PassList mPrintPasses;
  ProcessingStep mPrintSteps;
  const GlobalOptions *mGlobalOptions;
};

/// This prints LLVM IR to the standard output stream.
class EmitLLVMQueryManager : public QueryManager {
public:
  bool beginSourceFile(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;
  void run(llvm::Module *M, tsar::TransformationContext *) override;

  void endSourceFile() override {
#if (LLVM_VERSION_MAJOR > 3)
    // An output stream attached to a temporary output file should be freed.
    // Otherwise it prevents renaming a temporary output file to a regular one.
    mOS.reset();
#endif
  }

protected:
#if (LLVM_VERSION_MAJOR < 4)
  llvm::raw_pwrite_stream *mOS = nullptr;
#else
  std::unique_ptr<llvm::raw_pwrite_stream > mOS;
#endif
  const clang::CodeGenOptions *mCodeGenOpts = nullptr;
};

/// This performs instrumentation of LLVM IR and prints it to the standard
/// output stream after instrumentation.
class InstrLLVMQueryManager : public EmitLLVMQueryManager {
  void run(llvm::Module *M, tsar::TransformationContext *) override;
};

/// This performs a specified source-level transformation.
class TransformationQueryManager : public QueryManager {
public:
  /// Returns a list of initialized transformations.
  static PassGroupRegistry & getPassRegistry() noexcept {
    static PassGroupRegistry Passes;
    return Passes;
  }

  explicit TransformationQueryManager(const llvm::PassInfo *TfmPass,
    llvm::StringRef OutputSuffix = "", bool NoFormat = false) :
    mTfmPass(TfmPass), mOutputSuffix(OutputSuffix), mNoFormat(NoFormat) {}

  void run(llvm::Module *M, TransformationContext *Ctx) override;

  ASTImportInfo * initializeImportInfo() override { return &mImportInfo; }

private:
  const llvm::PassInfo *mTfmPass;
  std::string mOutputSuffix;
  bool mNoFormat;
  ASTImportInfo mImportInfo;
};

/// This performs a check of user-define properties.
class CheckQueryManager : public QueryManager {
public:
  /// Returns a list of initialized checkers.
  static PassGroupRegistry & getPassRegistry() noexcept {
    static PassGroupRegistry Passes;
    return Passes;
  }

  CheckQueryManager() = default;

  void run(llvm::Module *M, TransformationContext *Ctx) override;
};
}

#endif//TSAR_QUERY_H
