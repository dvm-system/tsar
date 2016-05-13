//===------ tsar_tool.cpp ---- Traits Static Analyzer -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements main interface for Traits Static Analyzer.
//
//===----------------------------------------------------------------------===//

#include <llvm/Support/Debug.h>
#include <llvm/Support/TargetSelect.h>
#include "tsar_action.h"
#include "tsar_tool.h"
#include "tsar_exception.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

Options::Options() :
  Sources(cl::Positional, cl::desc("<source0> [... <sourceN>]"),
    cl::OneOrMore),
  CompileCategory("Compilation options"),
  Includes("I", cl::cat(CompileCategory), cl::value_desc("path"),
    cl::desc("Add directory to include search path")),
  MacroDefs("D", cl::cat(CompileCategory), cl::value_desc("name=definition"),
    cl::desc("Predefine name as a macro")),
  LanguageStd("std", cl::cat(CompileCategory), cl::value_desc("standard"),
    cl::desc("Language standard to compile for")),
  InstrLLVM("instr-llvm", cl::cat(CompileCategory),
    cl::desc("Perform low-level (LLVM IR) instrumentation")),
  DebugCategory("Debugging options"),
  EmitLLVM("emit-llvm", cl::cat(DebugCategory),
    cl::desc("Emit llvm without analysis")),
  TimeReport("ftime-report", cl::cat(DebugCategory),
    cl::desc("Print some statistics about the time consumed by each pass when it finishes")) {
  StringMap<cl::Option*> &Opts = cl::getRegisteredOptions();
  assert(Opts.count("help") == 1 && "Option '-help' must be specified!");
  auto Help = Opts["help"];
  static cl::alias HelpA("h", cl::aliasopt(*Help), cl::desc("Alias for -help"));
#ifndef NDEBUG
  // Debug options are not available if LLVM has been built in release mode.
  if (Opts.count("debug") == 1) {
    auto Debug = Opts["debug"];
    Debug->setCategory(DebugCategory);
    Debug->setHiddenFlag(cl::NotHidden);
    assert(Opts.count("debug-only") == 1 &&
      "Option '-debug-only' must be specified!");
    auto DebugOnly = Opts["debug-only"];
    DebugOnly->setCategory(DebugCategory);
    DebugOnly->setHiddenFlag(cl::NotHidden);
    assert(Opts.count("stats") == 1 && "Option '-stats' must be specified!");
    Opts["stats"]->setCategory(DebugCategory);
  }
#endif
  cl::AddExtraVersionPrinter(printVersion);
  std::vector<cl::OptionCategory *> Categories;
  Categories.push_back(&CompileCategory);
  Categories.push_back(&DebugCategory);
  cl::HideUnrelatedOptions(Categories);
}

void Options::printVersion() {
  raw_ostream &OS = outs();
  OS << TSAR::Acronym::Data() + " (" + TSAR::URL::Data() + "):\n";
  OS << "  " << TSAR::Version::Field() + " " + TSAR::Version::Data() << "\n";
#ifndef __OPTIMIZE__
  OS << "  DEBUG build";
#else
  OS << "Optimized build";
#endif
#ifndef NDEBUG
  OS << " with assertions";
#endif
  OS << ".\n";
  OS << "  Built " << __DATE__ << " (" << __TIME__ << ").\n";
  std::string CPU = sys::getHostCPUName();
  OS << "  Host CPU: " << ((CPU != "generic") ? CPU : "(unknown)") << "\n";
}

Tool::Tool(int Argc, const char **Argv) {
  assert(Argv && "List of command line arguments must not be null!");
  parseCLOptions(Argc, Argv);
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
}

void Tool::parseCLOptions(int Argc, const char **Argv) {
  getOptions(); // At first, initialize command line options.
  std::string Descr = TSAR::Title::Data() + "(" + TSAR::Acronym::Data() + ")";
  cl::ParseCommandLineOptions(Argc, Argv, Descr.c_str());
  mSources = getOptions().Sources;
  mCommandLine.push_back("-g");
  if (!getOptions().LanguageStd.empty())
    mCommandLine.push_back("-std=" + getOptions().LanguageStd);
  if (getOptions().TimeReport)
    mCommandLine.push_back("-ftime-report");
  for (auto &Path : getOptions().Includes)
    mCommandLine.push_back("-I" + Path);
  for (auto &Macro : getOptions().MacroDefs)
    mCommandLine.push_back("-D" + Macro);
  mCompilations = std::unique_ptr<CompilationDatabase>(
    new FixedCompilationDatabase(".", mCommandLine));
}

int Tool::run() {
  std::unique_ptr<FrontendActionFactory> Factory;
  if (getOptions().EmitLLVM)
    Factory = newFrontendActionFactory<EmitLLVMAnalysisAction>();
  else if (getOptions().InstrLLVM)
    Factory = newFrontendActionFactory<InstrumentationAction>();
  else
    Factory = newAnalysisActionFactory<MainAction>(std::move(mCommandLine));
  ClangTool CTool(*mCompilations, mSources);
  return CTool.run(Factory.get());
}
