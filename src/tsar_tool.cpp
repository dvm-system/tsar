//===------ tsar_tool.cpp ---- Traits Static Analyzer -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements main interface for Traits Static Analyzer.
//
//===----------------------------------------------------------------------===//

#include "tsar_action.h"
#include "ASTMergeAction.h"
#include "tsar_exception.h"
#include "tsar_query.h"
#include "tsar_test.h"
#include "tsar_tool.h"
#include <clang/Frontend/FrontendActions.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>

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
  EmitAST("emit-ast", cl::cat(CompileCategory),
    cl::desc("Emit Clang AST files for source inputs")),
  MergeAST("merge-ast", cl::cat(CompileCategory),
    cl::desc("Merge Clang AST for source inputs before analysis")),
  MergeASTA("m", cl::aliasopt(MergeAST), cl::desc("Alias for -merge-ast")),
  Output("o", cl::cat(CompileCategory), cl::value_desc("file"),
    cl::desc("Write output to <file>")),
  Language("x", cl::cat(CompileCategory), cl::value_desc("language"),
    cl::desc("Treat subsequent input files as having type <language>")),
  DebugCategory("Debugging options"),
  EmitLLVM("emit-llvm", cl::cat(DebugCategory),
    cl::desc("Emit llvm without analysis")),
  TimeReport("ftime-report", cl::cat(DebugCategory),
    cl::desc("Print some statistics about the time consumed by each pass when it finishes")),
  Test("test", cl::cat(DebugCategory),
    cl::desc("Insert results of analysis to a source file")) {
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
  Options::get(); // At first, initialize command line options.
  std::string Descr = TSAR::Title::Data() + "(" + TSAR::Acronym::Data() + ")";
  cl::ParseCommandLineOptions(Argc, Argv, Descr);
  storeCLOptions();
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
}

Tool::Tool() {
  storeCLOptions();
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
}

void Tool::storeCLOptions() {
  mSources = Options::get().Sources;
  mCommandLine.emplace_back("-g");
  if (!Options::get().LanguageStd.empty())
    mCommandLine.push_back("-std=" + Options::get().LanguageStd);
  if (Options::get().TimeReport)
    mCommandLine.emplace_back("-ftime-report");
  if (!Options::get().Language.empty())
    mCommandLine.push_back("-x" + Options::get().Language);
  for (auto &Path : Options::get().Includes)
    mCommandLine.push_back("-I" + Path);
  for (auto &Macro : Options::get().MacroDefs)
    mCommandLine.push_back("-D" + Macro);
  mCompilations = std::unique_ptr<CompilationDatabase>(
    new FixedCompilationDatabase(".", mCommandLine));
  mEmitAST = Options::get().EmitAST;
  mMergeAST = Options::get().MergeAST;
  mEmitLLVM = Options::get().EmitLLVM;
  mInstrLLVM = Options::get().InstrLLVM;
  mTest = Options::get().Test;
  mOutputFilename = Options::get().Output;
  mLanguage = Options::get().Language;
}

inline static QueryManager * getDefaultQM() {
  static QueryManager QM;
  return &QM;
}
inline static EmitLLVMQueryManager * getEmitLLVMQM() {
  static EmitLLVMQueryManager QM;
  return &QM;
}

inline static InstrLLVMQueryManager * getInstrLLVMQM() {
  static InstrLLVMQueryManager QM;
  return &QM;
}

inline static TestQueryManager * getTestQM() {
  static TestQueryManager QM;
  return &QM;
}

int Tool::run(QueryManager *QM) {
  std::vector<std::string> NoASTSources;
  std::vector<std::string> SourcesToMerge;
  for (auto &Src : mSources)
    if (mLanguage != "ast" && sys::path::extension(Src) != ".ast")
      NoASTSources.push_back(Src);
    else
      SourcesToMerge.push_back(Src);
  // Evaluation of Clang AST files by this tool leads an error,
  // so these sources should be excluded.
  ClangTool EmitPCHTool(*mCompilations, NoASTSources);
  EmitPCHTool.appendArgumentsAdjuster(
    [&SourcesToMerge, this](
        const CommandLineArguments &CL, StringRef Filename) {
      CommandLineArguments Adjusted;
      for (std::size_t I = 0; I < CL.size(); ++I) {
        StringRef Arg = CL[I];
        // If `-fsyntax-only` is set all output files will be ignored.
        if (Arg.startswith("-fsyntax-only"))
          Adjusted.emplace_back("-emit-ast");
        else
          Adjusted.push_back(Arg);
      }
      Adjusted.emplace_back("-o");
      if (mOutputFilename.empty()) {
        SmallString<128> PCHFile = Filename;
        sys::path::replace_extension(PCHFile, ".ast");
        Adjusted.push_back(PCHFile.str());
        SourcesToMerge.push_back(PCHFile.str());
      } else {
        Adjusted.push_back(mOutputFilename);
        SourcesToMerge.push_back(mOutputFilename);
      }
      return Adjusted;
  });
  if (mEmitAST) {
    if (!mOutputFilename.empty() && NoASTSources.size() > 1) {
      errs() << "WARNING: The -o (output filename) option is ignored when "
                "generating multiple output files.\n";
      mOutputFilename.clear();
    }
    return EmitPCHTool.run(newFrontendActionFactory<GeneratePCHAction>().get());
  }
  if (!mOutputFilename.empty())
    errs() << "WARNING: The -o (output filename) option is ignored when "
              "the -emit-ast option is not used.\n";
  // Name of output should be unset to ignore this option when argument adjuster
  // for EmitPCHTool will be invoked.
  mOutputFilename.clear();
  // Emit Clang AST files for source inputs if inputs should be merged before
  // analysis. AST files will be stored in SourcesToMerge collection.
  // If an input file already contains Clang AST it will be pushed into
  // the SourcesToMerge collection only.
  if (mMergeAST)
    EmitPCHTool.run(newFrontendActionFactory<GeneratePCHAction>().get());
  if (!QM) {
    if (mEmitLLVM)
      QM = getEmitLLVMQM();
    else if (mInstrLLVM)
      QM = getInstrLLVMQM();
    else if (mTest)
      QM = getTestQM();
    else
      QM = getDefaultQM();
  }
  if (mMergeAST) {
    ClangTool CTool(*mCompilations, SourcesToMerge.back());
    SourcesToMerge.pop_back();
    return CTool.run(newAnalysisActionFactory<MainAction, tsar::ASTMergeAction>(
      mCommandLine, QM, SourcesToMerge).get());
  }
  ClangTool CTool(*mCompilations, mSources);
  return CTool.run(newAnalysisActionFactory<MainAction>(
    mCommandLine, QM).get());
}

