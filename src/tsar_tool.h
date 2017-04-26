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

#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <string>
#include <vector>
#include <utility.h>

namespace llvm {
class PassInfo;
}

namespace tsar {
class QueryManager;

/// A tool which performs different analysis and transformations actions.
///
/// It is possible to use Options and QueryManager to configure it.
class Tool : private bcl::Uncopyable {
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
  void storeCLOptions();

  std::vector<std::string> mCommandLine;
  std::vector<std::string> mSources;
  std::vector<const llvm::PassInfo *> mOutputPasses;
  std::unique_ptr<clang::tooling::CompilationDatabase> mCompilations;
  bool mEmitAST;
  bool mMergeAST;
  bool mEmitLLVM;
  bool mInstrLLVM;
  bool mTest;
  std::string mOutputFilename;
  std::string mLanguage;
};
}
#endif//TSAR_TOOL_H
