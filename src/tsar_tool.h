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
struct Options : private bcl::Uncopyable {
  /// Default constructor
  Options();

  /// This is a version printer for TSAR.
  static void printVersion();

  llvm::cl::list<std::string> Sources;

  llvm::cl::OptionCategory CompileCategory;
  llvm::cl::list<std::string> Includes;
  llvm::cl::list<std::string> MacroDefs;
  llvm::cl::opt<std::string> LanguageStd;
  llvm::cl::opt<bool> InstrLLVM;

  llvm::cl::OptionCategory DebugCategory;
  llvm::cl::opt<bool> EmitLLVM;
  llvm::cl::opt<bool> TimeReport;
};

class Tool : private bcl::Uncopyable {
public:
  /// \brief Creates analyzer according to the specified command line.
  ///
  /// Argc and Argv parameters are similar to values specified in the
  /// int main(int Argc, char **Argv) function.
  Tool(int Argc, const char **Argv);

  /// Returns all analyzer options.
  const Options & getOptions() const { const_cast<Tool *>(this)->getOptions(); }

  /// \brief Executes analysis.
  ///
  /// \return Zero on success, one otherwise.
  int run();

private:
  /// Returns all analyzer options.
  Options & getOptions() {
    static Options Opts;
    return Opts;
  }

  /// \brief Parses command line options.
  ///
  /// Argc and Argv parameters are similar to values specified in the
  /// int main(int Argc, char **Argv) function.
  /// \post
  /// - The list of sources which have been specified in a command line will be
  /// stored in mSources.
  /// - The parsed command line to compile all sources will be stored in
  /// mCommandLine.
  /// Parsed options can be accessed through the getOptions() method.
  void parseCLOptions(int Argc, const char **Argv);

  std::vector<std::string> mCommandLine;
  std::vector<std::string> mSources;
  std::unique_ptr<clang::tooling::CompilationDatabase> mCompilations;
};
}
#endif//TSAR_TOOL_H
