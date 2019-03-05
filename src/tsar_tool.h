//===------ tsar_tool.h ----- Traits Static Analyzer ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Traits Static Analyzer (TSAR) is a part of a system for automated
// parallelization SAPFOR. The main goal of analyzer is to determine
// data dependences, privatizable and induction variables and other traits of
// analyzed program which could be helpful to parallelize program automatically.
//
// This file declares interfaces to execute analysis and obtain its results.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_TOOL_H
#define TSAR_TOOL_H

#include <clang/Tooling/CompilationDatabase.h>
#include <llvm/ADT/SmallVector.h>
#include <string>
#include <vector>
#include <bcl/utility.h>

namespace llvm {
class PassInfo;
namespace cl {
class Option;
}
}

namespace tsar {
class QueryManager;

/// A tool which performs different analysis and transformations actions.
///
/// It is possible to use Options and QueryManager to configure it.
class Tool : private bcl::Uncopyable {
  using OptionList = llvm::SmallVector<llvm::cl::Option *, 8>;
public:
  /// \brief Creates analyzer according to the specified command line.
  ///
  /// Argc and Argv parameters are similar to values specified in the
  /// int main(int Argc, char **Argv) function.
  Tool(int Argc, const char **Argv);

  /// \brief Performs analysis.
  ///
  /// \param [in, out] QM This is a query manager for this tool, that specifies
  /// which analysis and transformations need to be performed. Result of
  /// execution will be accessed through this object. If it is not set than
  /// default sequence of analysis and transformations will be performed.
  /// \return Zero on success.
  int run(QueryManager *QM = nullptr);

private:
  /// \brief Stores command line options.
  ///
  /// The Options::get() method returns an object accessed from different
  /// places. To avoid redefinition of options after creation of this tool it
  /// is necessary to store these options separately.
  /// \post
  /// - The list of sources which have been specified in a command line will be
  /// stored in mSources.
  /// - The parsed command line to compile all sources will be stored in
  /// mCommandLine.
  /// - Each flag will be stored in a member associated with it.
  /// - Options which should be accessed from different places,
  /// will be stored in GlobalOptions structure.
  ///
  /// TODO (kaniandr@gmail.com): disallow multiple creation of Tool objects
  void storeCLOptions();

  /// \brief Store command line options that determine information
  /// to be printed.
  ///
  /// \post
  /// - If some of such options are set it will be added to `IncompatibleOpts`
  /// list. Note, that only one option will be added to this list.
  /// - If some of such options are set `mPrint` flag will be set to `true`.
  ///
  void storePrintOptions(OptionList &IncompatibleOpts);

  std::vector<std::string> mCommandLine;
  std::vector<std::string> mSources;
  std::vector<const llvm::PassInfo *> mOutputPasses;
  std::vector<const llvm::PassInfo *> mPrintPasses;
  ///Bit set of steps that should be printed.
  uint8_t mPrintSteps = 0;
  const llvm::PassInfo * mTfmPass;
  std::unique_ptr<clang::tooling::CompilationDatabase> mCompilations;
  bool mEmitAST;
  bool mMergeAST;
  bool mPrintAST;
  bool mDumpAST;
  bool mEmitLLVM;
  bool mInstrLLVM;
  bool mNoFormat;
  bool mCheck;
  bool mTest;
  bool mPrint;
  std::string mOutputFilename;
  std::string mLanguage;
  std::string mOutputSuffix;
  std::string mInstrEntry;
};
}
#endif//TSAR_TOOL_H
