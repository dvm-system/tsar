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

namespace tsar {
class QueryManager;

/// Represents possible options for TSAR.
struct Options : private bcl::Uncopyable {
  /// This is a version printer for TSAR.
  static void printVersion();

  /// Returns all possible analyzer options.
  static Options & get() {
    static Options Opts;
    return Opts;
  }

  llvm::cl::list<std::string> Sources;

  llvm::cl::OptionCategory CompileCategory;
  llvm::cl::list<std::string> Includes;
  llvm::cl::list<std::string> MacroDefs;
  llvm::cl::opt<std::string> LanguageStd;
  llvm::cl::opt<bool> InstrLLVM;

  llvm::cl::OptionCategory DebugCategory;
  llvm::cl::opt<bool> EmitLLVM;
  llvm::cl::opt<bool> TimeReport;
  llvm::cl::opt<bool> Test;
private:
  /// Default constructor.
  ///
  /// This structure is designed according to a singleton pattern, so all
  /// constructors are private.
  Options();
};

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

  /// \brief Creates analyzer according to specified options.
  ///
  /// It is possible to set options explicitly via access to Options structure.
  /// For example, Options::get().EmitLLVM = true.
  /// \attention Be careful when use this constructor because no checks for
  /// options correctness will be performed.
  Tool();

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
  std::unique_ptr<clang::tooling::CompilationDatabase> mCompilations;
  bool mEmitLLVM;
  bool mInstrLLVM;
  bool mTest;
};
}
#endif//TSAR_TOOL_H
