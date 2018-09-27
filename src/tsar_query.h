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
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <vector>

namespace llvm {
class Pass;
class PassInfo;
class Module;
class raw_pwrite_stream;
}

namespace clang {
class CompilerInstance;
class CodeGenOptions;
}

namespace tsar {
class TransformationContext;

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
};

/// This specify default pass sequence, which can be configured by command line
/// options in some ways.
class DefaultQueryManager: public QueryManager {
public:
  /// List of passes.
  typedef std::vector<const llvm::PassInfo *> PassList;

  /// Returns a list of initialized output passes.
  static PassGroupRegistry & getPassRegistry() noexcept {
    static PassGroupRegistry Passes;
    return Passes;
  }

  /// Creates query manager.
  ///
  /// \param [in] OutputPasses This specifies passes that should be run to
  /// show program exploration results.
  explicit DefaultQueryManager(const PassList &OutputPasses) :
    mOutputPasses(OutputPasses) {}

  /// Runs default sequence of passes.
  void run(llvm::Module *M, tsar::TransformationContext *Ctx) override;

private:
  PassList mOutputPasses;
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

private:
  const llvm::PassInfo *mTfmPass;
  std::string mOutputSuffix;
  bool mNoFormat;
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
