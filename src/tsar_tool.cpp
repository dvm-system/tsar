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
#include "tsar_finliner.h"
#include "tsar_pragma.h"
#include "tsar_query.h"
#include "tsar_test.h"
#include "tsar_tool.h"
#include <clang/Frontend/FrontendActions.h>
#include <llvm/IR/LegacyPassNameParser.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

namespace {
extern const char gAllowedOutputPassArgs[] = "\
  -view-em -view-em-only\
  -dot-em -dot-em-only";

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

  llvm::cl::list<const llvm::PassInfo*, bool,
    llvm::FilteredPassNameParser<
    llvm::PassArgFilter<gAllowedOutputPassArgs>>> OutputPasses;

  llvm::cl::OptionCategory CompileCategory;
  llvm::cl::list<std::string> Includes;
  llvm::cl::list<std::string> MacroDefs;
  llvm::cl::opt<std::string> LanguageStd;
  llvm::cl::opt<bool> InstrLLVM;
  llvm::cl::opt<bool> EmitAST;
  llvm::cl::opt<bool> MergeAST;
  llvm::cl::alias MergeASTA;
  llvm::cl::opt<std::string> Output;
  llvm::cl::opt<std::string> Language;

  llvm::cl::OptionCategory DebugCategory;
  llvm::cl::opt<bool> EmitLLVM;
  llvm::cl::opt<bool> PrintAST;
  llvm::cl::opt<bool> DumpAST;
  llvm::cl::opt<bool> TimeReport;
  llvm::cl::opt<bool> Test;
  llvm::cl::opt<bool> Inline;
private:
  /// Default constructor.
  ///
  /// This structure is designed according to a singleton pattern, so all
  /// constructors are private.
  Options();
};
}

Options::Options() :
  Sources(cl::Positional, cl::desc("<source0> [... <sourceN>]"),
    cl::OneOrMore),
  OutputPasses(cl::desc("Analysis available:")),
  CompileCategory("Compilation options"),
  Includes("I", cl::cat(CompileCategory), cl::value_desc("path"),
    cl::desc("Add directory to include search path"), cl::Prefix),
  MacroDefs("D", cl::cat(CompileCategory), cl::value_desc("name=definition"),
    cl::desc("Predefine name as a macro"), cl::Prefix),
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
    cl::desc("Write output to <file>"), cl::Prefix),
  Language("x", cl::cat(CompileCategory), cl::value_desc("language"),
    cl::desc("Treat subsequent input files as having type <language>"),
    cl::Prefix),
  DebugCategory("Debugging options"),
  EmitLLVM("emit-llvm", cl::cat(DebugCategory),
    cl::desc("Emit llvm without analysis")),
  PrintAST("print-ast", cl::cat(DebugCategory),
    cl::desc("Build ASTs and then pretty - print them")),
  DumpAST("dump-ast", cl::cat(DebugCategory),
    cl::desc("Build ASTs and then debug dump them")),
  TimeReport("ftime-report", cl::cat(DebugCategory),
    cl::desc("Print some statistics about the time consumed by each pass when it finishes")),
  Test("test", cl::cat(DebugCategory),
    cl::desc("Insert results of analysis to a source file")),
  Inline("inline", cl::cat(DebugCategory),
    cl::desc("Dump inlined versions of source files")) {
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
    assert(Opts.count("debug-only") == 1 && "Option must be specified!");
    auto DebugOnly = Opts["debug-only"];
    DebugOnly->setCategory(DebugCategory);
    DebugOnly->setHiddenFlag(cl::NotHidden);
    auto DebugPass = Opts["debug-pass"];
    assert(Opts.count("debug-pass") == 1 && "Option must be specified!");
    DebugPass->setCategory(DebugCategory);
    DebugPass->setHiddenFlag(cl::NotHidden);
    assert(Opts.count("stats") == 1 && "Option must be specified!");
    Opts["stats"]->setCategory(DebugCategory);
  }
#endif
  assert(Opts.count("print-before") == 1 && "Option must be specified!");
  assert(Opts.count("print-after") == 1 && "Option must be specified!");
  assert(Opts.count("print-before-all") == 1 && "Option must be specified!");
  assert(Opts.count("print-after-all") == 1 && "Option must be specified!");
  assert(Opts.count("filter-print-funcs") == 1 && "Option must be specified!");
  Opts["print-before"]->setCategory(DebugCategory);
  Opts["print-after"]->setCategory(DebugCategory);
  Opts["print-before-all"]->setCategory(DebugCategory);
  Opts["print-after-all"]->setCategory(DebugCategory);
  Opts["filter-print-funcs"]->setCategory(DebugCategory);
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
  // Passes should be initialized previously then command line options are
  // parsed, due to initialize list of available passes.
  initializeTSAR(*PassRegistry::getPassRegistry());
  cl::ParseCommandLineOptions(Argc, Argv, Descr);
  storeCLOptions();
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  cl::PrintOptionValues();
}

void Tool::storeCLOptions() {
  mSources = Options::get().Sources;
  mCommandLine.emplace_back("-g");
  mCommandLine.emplace_back("-Wunknown-pragmas");
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
  SmallVector<cl::Option *, 8> IncompatibleOpts;
  auto addIfSet = [&IncompatibleOpts](cl::opt<bool> &O) -> bool {
    if (O)
      IncompatibleOpts.push_back(&O);
    return O;
  };
  mEmitAST = addIfSet(Options::get().EmitAST);
  mMergeAST = mEmitAST ?
    addIfSet(Options::get().MergeAST) : Options::get().MergeAST;
  mPrintAST = addIfSet(Options::get().PrintAST);
  mDumpAST = addIfSet(Options::get().DumpAST);
  for (auto PI : Options::get().OutputPasses)
    mOutputPasses.push_back(PI);
  mEmitLLVM = addIfSet(Options::get().EmitLLVM);
  mInstrLLVM = addIfSet(Options::get().InstrLLVM);
  mTest = addIfSet(Options::get().Test);
  mInline = addIfSet(Options::get().Inline);
  mOutputFilename = Options::get().Output;
  mLanguage = Options::get().Language;
  if (IncompatibleOpts.size() > 1) {
    std::string Msg("error - this option is incompatible with");
    for (unsigned I = 1; I < IncompatibleOpts.size(); ++I)
      Msg.append(" -").append(IncompatibleOpts[1]->ArgStr);
    IncompatibleOpts[0]->error(Msg);
    exit(1);
  }
}

inline static QueryManager * getDefaultQM(
    const DefaultQueryManager::PassList &OutputPasses) {
  static DefaultQueryManager QM(OutputPasses);
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

inline static FunctionInlinerQueryManager * getInlineQM() {
  static FunctionInlinerQueryManager QM;
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
  auto ArgumentsAdjuster = [&SourcesToMerge, this](
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
  };
  EmitPCHTool.appendArgumentsAdjuster(ArgumentsAdjuster);
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
  if (mMergeAST) {
    EmitPCHTool.run(
      newFrontendActionFactory<GeneratePCHAction, GenPCHPragmaAction>().get());
  }
  if (!QM) {
    if (mEmitLLVM)
      QM = getEmitLLVMQM();
    else if (mInstrLLVM)
      QM = getInstrLLVMQM();
    else if (mTest)
      QM = getTestQM();
    else if (mInline)
      QM = getInlineQM();
    else
      QM = getDefaultQM(mOutputPasses);
  }
  if (mMergeAST) {
    ClangTool CTool(*mCompilations, SourcesToMerge.back());
    SourcesToMerge.pop_back();
    if (mDumpAST)
      return CTool.run(newFrontendActionFactory<
        tsar::ASTDumpAction, tsar::ASTMergeAction>(SourcesToMerge).get());
    if (mPrintAST)
      return CTool.run(newFrontendActionFactory<
        tsar::ASTPrintAction, tsar::ASTMergeAction>(SourcesToMerge).get());
    return CTool.run(newAnalysisActionFactory<MainAction, tsar::ASTMergeAction>(
      mCommandLine, QM, SourcesToMerge).get());
  }
  ClangTool CTool(*mCompilations, mSources);
  if (mDumpAST)
    return CTool.run(newFrontendActionFactory<
      tsar::ASTDumpAction, tsar::GenPCHPragmaAction>().get());
  if (mPrintAST)
    return CTool.run(newFrontendActionFactory<
      tsar::ASTPrintAction, tsar::GenPCHPragmaAction>().get());
  return CTool.run(newAnalysisActionFactory<MainAction, GenPCHPragmaAction>(
    mCommandLine, QM).get());
}

