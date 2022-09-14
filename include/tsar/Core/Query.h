//===------ Query.h -------------- Query Manager ----------------*- C++ -*-===//
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
// This defines a query manager that controls construction of response when
// analysis and transformation tool is launched.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_QUERY_H
#define TSAR_QUERY_H

#include "tsar/Frontend/Clang/ASTImportInfo.h"
#include "tsar/Support/OutputFile.h"
#include "tsar/Support/PassGroupRegistry.h"
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/StringRef.h>
#include <vector>

namespace llvm {
class DataLayout;
class Pass;
class PassInfo;
class Module;
class raw_pwrite_stream;

namespace legacy {
class PassManager;
}
}

namespace clang {
class DiagnosticsEngine;
}

namespace tsar {
struct GlobalOptions;
class DIMemoryTrait;
class TransformationInfo;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Print versions of all available tools to a specified stream.
void printToolVersion(llvm::raw_ostream &OS);

/// Add immutable alias analysis passes.
void addImmutableAliasAnalysis(llvm::legacy::PassManager &Passes);

/// Add transformations which do not influence source-level to IR-level
/// correspondence (add some attributes, remove unreachable code, etc).
void addInitialTransformations(llvm::legacy::PassManager &Passes);

/// Add preliminary analysis of privatizable variables.
///
/// This analysis is necessary to prevent lost of result of optimized values.
/// The memory promotion may remove some variables with attached source-level
/// debug information. Consider an example:
/// `int *P, X; P = &X; for (...) { *P = 0; X = 1; }`
/// Memory promotion removes P, so X will be recognized as private variable.
/// However in the original program data-dependency exists because different
/// pointers refer the same memory.
/// \param AnalysisUse External analysis results which should be used to
/// clarify analysis. If it is empty GlobalOptions::AnalysisUse value is used
/// if not empty.
void addBeforeTfmAnalysis(llvm::legacy::PassManager &Passes,
                          llvm::StringRef AnalysisUse = "");

/// Perform SROA and repeat variable privatization. After that reduction and
/// induction recognition will be performed. Flow/anti/output dependencies
/// also analyses.
void addAfterSROAAnalysis(const GlobalOptions &GO, const llvm::DataLayout &DL,
                          llvm::legacy::PassManager &Passes);

/// Perform function inlining then repeat SROA and repeat dependence analysis.
///
/// Use 'Unlock' to unlock some traits before function inlining if necessary.
/// For example, it may be useful to unset 'lock' trait for not promoted
/// scalar, because function inlining may help SROA to promote them.
void addAfterFunctionInlineAnalysis(
    const GlobalOptions &GO, const llvm::DataLayout &DL,
    const std::function<void(tsar::DIMemoryTrait &)> &Unlock,
    llvm::legacy::PassManager &Passes);

/// Perform loop rotation to enable reduction recognition if for-loops.
void addAfterLoopRotateAnalysis(llvm::legacy::PassManager &Passes);

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
  virtual bool beginSourceFile(clang::DiagnosticsEngine &Diags,
                               llvm::StringRef InputFile,
                               llvm::StringRef OutputFile,
                               llvm::StringRef WorkingDir) {
    return true;
  }

  /// \brief Callback at the end of processing a single input.
  ///
  /// This is guaranteed to only be called following a successful call to
  /// beginSourceFile().
  virtual void endSourceFile(bool HasErrorOccurred) {}

  /// Analysis the specified module and transforms source file associated with
  /// it if rewriter context is specified.
  ///
  /// \attention Neither module nor transformation context is not going to be
  /// taken under control.
  virtual void run(llvm::Module *M, tsar::TransformationInfo *TfmInfo) = 0;

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
    AfterFunctionInlineAnalysis = 1u << 2,
    AfterLoopRotateAnalysis = 1u << 3,
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
  /// \param [in] UseServer Use analysis server to improve analysis quality.
  /// \param [in] GlobalOptions This specifies a list of global options,
  /// that may be accessed by multiple passes. Global options may be accessed
  /// not only in this query manager, so the manager should not owns their.
  /// \param [in] OutputPasses This specifies passes that should be run to
  /// show program exploration results.
  /// \param [in] PrintPasses This specifies passes, the results of which should
  /// be printed. Specified passes must override their print() methods.
  /// \param [in] PrintSteps This specifies processing steps, the result of
  /// which should be printed. This is a bit list of steps.
  /// \param [in] AnalysisUse Name of a file with external analysis results
  /// which should be used to clarify analysis.
  DefaultQueryManager(bool UseServer, const GlobalOptions *Options,
      const PassList &OutputPasses, const PassList &PrintPasses,
      ProcessingStep PrintSteps = allSteps(),
      llvm::StringRef AnalysisUse = "") :
    mUseServer(UseServer),
    mGlobalOptions(Options),
    mOutputPasses(OutputPasses), mPrintPasses(PrintPasses),
    mPrintSteps(PrintSteps) {}

  /// Runs default sequence of passes.
  void run(llvm::Module *M, tsar::TransformationInfo *TfmInfo) override;

  /// Initializes external storage to access information about import process.
  ASTImportInfo * initializeImportInfo() override { return &mImportInfo; }

private:
  /// Updates pass manager. Adds a specified pass and a pass to print its result
  // if `PrintResult` is set to 'true`.
  void addWithPrint(llvm::Pass *P, bool PrintResult,
    llvm::legacy::PassManager &Passes);

  bool mUseServer = false;
  PassList mOutputPasses;
  PassList mPrintPasses;
  ProcessingStep mPrintSteps;
  const GlobalOptions *mGlobalOptions;
  ASTImportInfo mImportInfo;
};

/// This prints LLVM IR to the standard output stream.
class EmitLLVMQueryManager : public QueryManager {
public:
  bool beginSourceFile(clang::DiagnosticsEngine &Diags,
                       llvm::StringRef InputFile, llvm::StringRef OutputFIle,
                       llvm::StringRef WorkingDir) override;
  void run(llvm::Module *M, tsar::TransformationInfo *) override;

  void endSourceFile(bool HasErrorOccurred) override;

protected:
  llvm::Optional<tsar::OutputFile> mOutputFile;
  std::string mWorkingDir;
  clang::DiagnosticsEngine *mDiags{nullptr};
};

/// This performs instrumentation of LLVM IR and prints it to the standard
/// output stream after instrumentation.
class InstrLLVMQueryManager : public EmitLLVMQueryManager {
public:
  explicit InstrLLVMQueryManager(const GlobalOptions *GO,
      llvm::StringRef InstrEntry = "",
      llvm::ArrayRef<std::string> InstrStart = {}) :
    mInstrEntry(InstrEntry),
    mInstrStart(InstrStart.begin(), InstrStart.end()), mGlobalOptions(GO) {}

  void run(llvm::Module *M, tsar::TransformationInfo *) override;

private:
  std::string mInstrEntry;
  std::vector<std::string> mInstrStart;
  const GlobalOptions *mGlobalOptions;
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
      const GlobalOptions *Options) :
    mTfmPass(TfmPass), mGlobalOptions(Options) {}

  void run(llvm::Module *M, TransformationInfo *TfmInfo) override;

  ASTImportInfo * initializeImportInfo() override { return &mImportInfo; }

private:
  const llvm::PassInfo *mTfmPass;
  const GlobalOptions *mGlobalOptions;
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

  void run(llvm::Module *M, TransformationInfo *TfmInfo) override;
};
}

#endif//TSAR_QUERY_H
