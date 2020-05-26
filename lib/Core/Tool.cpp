//===------ Tool.cpp --- Traits Static AnalyzeR (Library) -------*- C++ -*-===//
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
// Traits Static Analyzer (TSAR) is a part of a system for automated
// parallelization SAPFOR. This file implements interfaces to execute analysis
// and to perform transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/Query.h"
#include "tsar/Core/Passes.h"
#include "tsar/Core/Tool.h"
#include "tsar/Core/tsar-config.h"
#include "tsar/Frontend/Clang/Action.h"
#include "tsar/Frontend/Clang/ASTMergeAction.h"
#include "tsar/Patch/llvm/IR/LegacyPassNameParser.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/Clang/Pragma.h"
#ifdef APC_FOUND
# include "tsar/APC/Utils.h"
#endif
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/CommandLine.h>
#ifdef lp_solve_FOUND
# include <lp_solve/lp_solve_config.h>
#endif

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

namespace {
template<class PassGroupT>
struct PassGroupRegistryFilterTraits {
  static inline const PassGroupRegistry & getPassRegistry() {
    return PassGroupT::getPassRegistry();
  }
};

template<class PassGroupT,
  class Traits = PassGroupRegistryFilterTraits<PassGroupT>>
class PassFromGroupFilter {
public:
  bool operator()(const PassInfo &P) const {
    return Traits::getPassRegistry().exist(P);
  }
};

/// Represents possible options for TSAR.
struct Options : private bcl::Uncopyable {
  /// This is a version printer for TSAR.
  static void printVersion(raw_ostream &OS);

  /// Returns all possible analyzer options.
  static Options & get() {
    static Options Opts;
    return Opts;
  }

  llvm::cl::list<std::string> Sources;

  llvm::cl::list<const llvm::PassInfo*, bool,
    llvm::patch::FilteredPassNameParser<
      PassFromGroupFilter<DefaultQueryManager,
        DefaultQueryManager::OutputPassGroup>>> OutputPasses;

  llvm::cl::opt<const llvm::PassInfo *, false,
    llvm::patch::FilteredPassNameParser<
      PassFromGroupFilter<TransformationQueryManager>>> TfmPass;

  llvm::cl::OptionCategory CompileCategory;
  llvm::cl::list<std::string> Includes;
  llvm::cl::list<std::string> MacroDefs;
  llvm::cl::opt<std::string> LanguageStd;
  llvm::cl::opt<bool> InstrLLVM;
  llvm::cl::opt<std::string> InstrEntry;
  llvm::cl::list<std::string> InstrStart;
  llvm::cl::opt<bool> EmitAST;
  llvm::cl::opt<bool> MergeAST;
  llvm::cl::alias MergeASTA;
  llvm::cl::opt<std::string> Output;
  llvm::cl::opt<std::string> Language;
  llvm::cl::opt<bool> Verbose;
  llvm::cl::opt<bool> CaretDiagnostics;
  llvm::cl::opt<bool> NoCaretDiagnostics;
  llvm::cl::opt<bool> ShowSourceLocation;
  llvm::cl::opt<bool> NoShowSourceLocation;

  llvm::cl::OptionCategory DebugCategory;
  llvm::cl::opt<bool> EmitLLVM;
  llvm::cl::opt<bool> PrintAST;
  llvm::cl::opt<bool> DumpAST;
  llvm::cl::opt<bool> TimeReport;
  llvm::cl::opt<bool> UseServer;

  llvm::cl::opt<bool> PrintAll;
  llvm::cl::list<const PassInfo *, bool,
    llvm::patch::FilteredPassNameParser<
      PassFromGroupFilter<DefaultQueryManager,
        DefaultQueryManager::PrintPassGroup>>> PrintOnly;
  llvm::cl::list<unsigned> PrintStep;
  llvm::cl::opt<bool> PrintFilename;

  llvm::cl::OptionCategory AnalysisCategory;
  llvm::cl::opt<bool> Check;
  llvm::cl::opt<bool> SafeTypeCast;
  llvm::cl::opt<bool> NoSafeTypeCast;
  llvm::cl::opt<bool> InBoundsSubscripts;
  llvm::cl::opt<bool> NoInBoundsSubscripts;
  llvm::cl::opt<bool> AnalyzeLibFunc;
  llvm::cl::opt<bool> NoAnalyzeLibFunc;
  llvm::cl::opt<bool> IgnoreRedundantMemory;
  llvm::cl::opt<bool> NoIgnoreRedundantMemory;
  llvm::cl::opt<bool> UnsafeTfmAnalysis;
  llvm::cl::opt<bool> NoUnsafeTfmAnalysis;
  llvm::cl::opt<bool> ExternalCalls;
  llvm::cl::opt<bool> NoExternalCalls;
  llvm::cl::opt<bool> MathErrno;
  llvm::cl::opt<bool> NoMathErrno;
  llvm::cl::opt<std::string> AnalysisUse;
  llvm::cl::list<std::string> OptRegion;

  llvm::cl::OptionCategory TransformCategory;
  llvm::cl::opt<bool> NoFormat;
  llvm::cl::opt<std::string> OutputSuffix;
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
  TfmPass(cl::desc("Transformations available (one at a time):")),
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
  InstrEntry("instr-entry", cl::cat(CompileCategory), cl::value_desc("function"),
    cl::desc("Name of a function where to place metadata initialization")),
  InstrStart("instr-start", cl::cat(CompileCategory), cl::value_desc("functions"),
    cl::ZeroOrMore, cl::ValueRequired, cl::CommaSeparated,
    cl::desc("Add start point for instrumentation")),
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
  Verbose("v", cl::cat(CompileCategory),
    cl::desc("Show commands to run and use verbose output")),
  CaretDiagnostics("fcaret-diagnostics", cl::cat(CompileCategory),
    cl::desc("Print source line and ranges from source code in diagnostic")),
  NoCaretDiagnostics("fno-caret-diagnostics", cl::cat(CompileCategory),
    cl::desc("Do not print source line and ranges from source code in diagnostic")),
  ShowSourceLocation("fshow-source-location", cl::cat(CompileCategory),
    cl::desc("Print source file/line/column information in diagnostic")),
  NoShowSourceLocation("fno-show-source-location", cl::cat(CompileCategory),
    cl::desc("Do not print source file/line/column information in diagnostic.")),
  DebugCategory("Debugging options"),
  EmitLLVM("emit-llvm", cl::cat(DebugCategory),
    cl::desc("Emit llvm without analysis")),
  PrintAST("print-ast", cl::cat(DebugCategory),
    cl::desc("Build ASTs and then pretty - print them")),
  DumpAST("dump-ast", cl::cat(DebugCategory),
    cl::desc("Build ASTs and then debug dump them")),
  TimeReport("ftime-report", cl::cat(DebugCategory),
    cl::desc("Print some statistics about the time consumed by each pass when it finishes")),
  UseServer("use-analysis-server", cl::cat(DebugCategory),
    cl::desc("Run default workflow on analysis server")),
  PrintAll("print-all", cl::cat(DebugCategory),
    cl::desc("Print all available results")),
  PrintOnly("print-only", cl::cat(DebugCategory), cl::CommaSeparated,
    cl::desc("Print results for specified passes (comma separated list of passes)")),
  PrintStep("print-step", cl::cat(DebugCategory), cl::CommaSeparated,
    cl::desc("Print results for a specified processing steps (comma separated list of steps)")),
  PrintFilename("print-filename", cl::cat(DebugCategory),
    cl::desc("Print only names of files instead of full paths")),
  AnalysisCategory("Analysis options"),
  Check("check", cl::cat(AnalysisCategory),
    cl::desc("Check user-defined properties")),
  SafeTypeCast("fsafe-type-cast", cl::cat(AnalysisCategory),
    cl::desc("Disallow unsafe integer type cast in analysis passes")),
  NoSafeTypeCast("fno-safe-type-cast", cl::cat(AnalysisCategory),
    cl::desc("Allow unsafe integer type cast in analysis passes(default)")),
  InBoundsSubscripts("finbounds-subscripts", cl::cat(AnalysisCategory),
    cl::desc("Assume that subscript expression is in bounds value of an array dimension")),
  NoInBoundsSubscripts("fno-inbounds-subscripts", cl::cat(AnalysisCategory),
    cl::desc("Check that subscript expression is in bounds value of an array dimension(default)")),
  AnalyzeLibFunc("fanalyze-library-functions", cl::cat(AnalysisCategory),
    cl::desc("Perform analysis of library functions(default)")),
  NoAnalyzeLibFunc("fno-analyze-library-functions", cl::cat(AnalysisCategory),
    cl::desc("Do not perform analysis of library functions")),
  IgnoreRedundantMemory("fignore-redundant-memory", cl::cat(AnalysisCategory),
    cl::desc("Try to discard influence of redundant memory on the analysis results")),
  NoIgnoreRedundantMemory("fno-ignore-redundant-memory", cl::cat(AnalysisCategory),
    cl::desc("Do not discard influence of redundant memory on the analysis results(default)")),
  UnsafeTfmAnalysis("funsafe-tfm-analysis", cl::cat(AnalysisCategory),
    cl::desc("Perform analysis after unsafe transformations")),
  NoUnsafeTfmAnalysis("fno-unsafe-tfm-analysis", cl::cat(AnalysisCategory),
    cl::desc("Disable analysis after unsafe transformations(default)")),
  ExternalCalls("fexternal-calls", cl::cat(AnalysisCategory),
    cl::desc("Check whether a function could be called outside the analyzed module(default)")),
  NoExternalCalls("fno-external-calls", cl::cat(AnalysisCategory),
    cl::desc("Assume that functions are never called outside the analyzed module")),
  MathErrno("fmath-errno", cl::cat(AnalysisCategory),
     cl::desc("Require math functions to indicate errors by setting errno")),
  NoMathErrno("fno-math-errno", cl::cat(AnalysisCategory),
     cl::desc("Prevent math functions to indicate errors by setting errno")),
  AnalysisUse("fanalysis-use", cl::cat(AnalysisCategory),
    cl::value_desc("filename"),
    cl::desc("Use external analysis results to clarify analysis")),
  OptRegion("foptimize-only", cl::cat(AnalysisCategory), cl::value_desc("regions"),
    cl::ZeroOrMore, cl::ValueRequired, cl::CommaSeparated,
    cl::desc("Allow optimization of specified regions (comma separated list of region names")),
  TransformCategory("Transformation options"),
  NoFormat("no-format", cl::cat(TransformCategory),
    cl::desc("Disable format of transformed sources")),
  OutputSuffix("output-suffix", cl::cat(TransformCategory), cl::value_desc("suffix"),
    cl::desc("Filename suffix (between name and extension) for transformed sources")) {
  StringMap<cl::Option*> &Opts = cl::getRegisteredOptions();
  assert(Opts.count("help") == 1 && "Option '-help' must be specified!");
  auto Help = Opts["help"];
  static cl::alias HelpA("h", cl::aliasopt(*Help), cl::desc("Alias for -help"));
#ifdef LLVM_DEBUG
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
  Categories.push_back(&AnalysisCategory);
  Categories.push_back(&TransformCategory);
  cl::HideUnrelatedOptions(Categories);
}

void Options::printVersion(raw_ostream &OS) {
  OS << "TSAR (" << TSAR_HOMEPAGE_URL << "):\n";
  OS << "  version " << TSAR_VERSION_STRING << "\n";
#ifdef APC_FOUND
  OS << "  with "; printAPCVersion(OS);
#endif
#ifdef lp_solve_FOUND
  OS << "  with lp_solve(" << LP_SOLVE_HOMEPAGE_URL << "):\n";
  OS << "    version " << LP_SOLVE_VERSION_STRING << "\n";
#endif
#ifndef __OPTIMIZE__
  OS << "  DEBUG build";
#else
  OS << "  Optimized build";
#endif
#ifndef NDEBUG
  OS << " with assertions";
#endif
  OS << ".\n";
  OS << "  Built " << __DATE__ << " (" << __TIME__ << ").\n";
  std::string CPU = sys::getHostCPUName();
  OS << "  Host CPU: " << ((CPU != "generic") ? CPU : "(unknown)") << "\n";
}

/// Add special arguments for LLVM passes. This arguments should not be
/// inserted manually in a command line.
///
/// It seems that there is no other way to set options for LLVM passes
/// because '-mllvm <arg>' option works only for Clang.
static std::vector<const char *> addInternalArgs(int Argc, const char **Argv) {
  std::vector<const char *> Args(Argc);
  std::copy(Argv, Argv + Argc, Args.begin());
  if (!std::count_if(Argv, Argv + Argc, [](const char *Arg) {
    const char Opt[] = "-instcombine-lower-dbg-declare";
    return bcl::array_sizeof(Opt) - 1 < std::strlen(Arg) &&
      std::strncmp(Opt, Arg, bcl::array_sizeof(Opt) - 1) == 0;
  }))
    Args.emplace_back("-instcombine-lower-dbg-declare=0");
  return Args;
}

Tool::Tool(int Argc, const char **Argv) {
  assert(Argv && "List of command line arguments must not be null!");
  Options::get(); // At first, initialize command line options.
  std::string Descr = std::string(TSAR_DESCRIPTION) + "(TSAR)";
  // Passes should be initialized previously then command line options are
  // parsed, due to initialize list of available passes.
  initializeTSAR(*PassRegistry::getPassRegistry());
  auto Args = addInternalArgs(Argc, Argv);
  cl::ParseCommandLineOptions(Args.size(), Args.data(), Descr);
  storeCLOptions();
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  cl::PrintOptionValues();
}

inline static QueryManager * getDefaultQM(bool UseServer,
    const DefaultQueryManager::PassList &OutputPasses,
    const DefaultQueryManager::PassList &PrintPasses,
    const DefaultQueryManager::ProcessingStep PrintSteps,
    const GlobalOptions &GlobalOpts) {
  static DefaultQueryManager QM(UseServer, &GlobalOpts,
    OutputPasses, PrintPasses, PrintSteps);
  return &QM;
}

inline static EmitLLVMQueryManager * getEmitLLVMQM() {
  static EmitLLVMQueryManager QM;
  return &QM;
}

inline static InstrLLVMQueryManager * getInstrLLVMQM(
    StringRef InstrEntry, ArrayRef<std::string> InstrStart) {
  static InstrLLVMQueryManager QM(InstrEntry, InstrStart);
  return &QM;
}

inline static TransformationQueryManager * getTransformationQM(
    const llvm::PassInfo *TfmPass, const GlobalOptions &GlobalOpts) {
  static TransformationQueryManager QM(TfmPass, &GlobalOpts);
  return &QM;
}

inline static CheckQueryManager * getCheckQM() {
  static CheckQueryManager QM;
  return &QM;
}

void Tool::storePrintOptions(OptionList &IncompatibleOpts) {
  mPrint = false;
  mPrintPasses = Options::get().PrintOnly;
  if (!mPrintPasses.empty()) {
    mPrint = true;
    cl::Option &O = Options::get().PrintOnly;
    IncompatibleOpts.push_back(&O);
  } else if (Options::get().PrintAll) {
    mPrint = true;
    IncompatibleOpts.push_back(&Options::get().PrintAll);
    mPrintPasses.insert(mPrintPasses.begin(),
      DefaultQueryManager::PrintPassGroup::getPassRegistry().begin(),
      DefaultQueryManager::PrintPassGroup::getPassRegistry().end());
  }
  mGlobalOpts.PrintFilenameOnly = Options::get().PrintFilename;
  if (mGlobalOpts.PrintFilenameOnly && !mPrint)
    errs() << "WARNING: The -print-filename option is ignored when "
      "passes to be printed are not set.\n";
  if (Options::get().PrintStep.empty()) {
    mPrintSteps = DefaultQueryManager::allSteps();
  } else {
    if (!mPrint && mOutputPasses.empty())
      errs() << "WARNING: The -print-step option is ignored when "
        "passes to be printed are not set.\n";
    mPrintSteps = DefaultQueryManager::InitialStep;
    for (auto Step : Options::get().PrintStep) {
      if (Step > DefaultQueryManager::numberOfSteps()) {
        Options::get().PrintStep.error(
          "error - exceeded the number of available steps (maximum number is " +
          Twine((unsigned)DefaultQueryManager::numberOfSteps()) + ")");
        exit(1);
      }
      mPrintSteps |= 1u << (Step - 1);
    }
  }

}

void Tool::storeCLOptions() {
  mSources = Options::get().Sources;
  mCommandLine.emplace_back("-O1");
  mCommandLine.emplace_back("-Xclang");
  mCommandLine.emplace_back("-disable-llvm-passes");
  mCommandLine.emplace_back("-g");
  mCommandLine.emplace_back("-fstandalone-debug");
  mCommandLine.emplace_back("-Wunknown-pragmas");
  if (Options::get().CaretDiagnostics)
    mCommandLine.emplace_back("-fcaret-diagnostics");
  if (Options::get().NoCaretDiagnostics)
    mCommandLine.emplace_back("-fno-caret-diagnostics");
  if (Options::get().ShowSourceLocation)
    mCommandLine.emplace_back("-fshow-source-location");
  if (Options::get().NoShowSourceLocation)
    mCommandLine.emplace_back("-fno-show-source-location");
  if (Options::get().Verbose)
    mCommandLine.emplace_back("-v");
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
  if (Options::get().MathErrno && Options::get().NoMathErrno) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoMathErrno.ArgStr);
    Options::get().MathErrno.error(Msg);
    exit(1);
  }
  if (Options::get().MathErrno)
    mCommandLine.emplace_back("-fmath-errno");
  if (Options::get().NoMathErrno)
    mCommandLine.emplace_back("-fno-math-errno");
  mCompilations = std::unique_ptr<CompilationDatabase>(
    new FixedCompilationDatabase(".", mCommandLine));
  OptionList IncompatibleOpts;
  auto addIfSet = [&IncompatibleOpts](cl::opt<bool> &O) -> cl::opt<bool> & {
    if (O)
      IncompatibleOpts.push_back(&O);
    return O;
  };
  auto addIfSetIf = [&IncompatibleOpts](cl::opt<bool> &O, bool Predicat)
      -> cl::opt<bool> & {
    if (O && Predicat)
      IncompatibleOpts.push_back(&O);
    return O;
  };
  SmallVector<cl::Option *, 8> LLIncompatibleOpts;
  auto addLLIfSet = [&LLIncompatibleOpts](cl::opt<bool> &O) -> cl::opt<bool> & {
    if (O)
      LLIncompatibleOpts.push_back(&O);
    return O;
  };
  if (mTfmPass = Options::get().TfmPass) {
    LLIncompatibleOpts.push_back(&Options::get().TfmPass);
    IncompatibleOpts.push_back(&Options::get().TfmPass);
  }
  mGlobalOpts.IsSafeTypeCast = Options::get().SafeTypeCast;
  if (Options::get().SafeTypeCast && Options::get().NoSafeTypeCast) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoSafeTypeCast.ArgStr);
    Options::get().SafeTypeCast.error(Msg);
    exit(1);
  }
  mGlobalOpts.InBoundsSubscripts = Options::get().InBoundsSubscripts;
  if (Options::get().InBoundsSubscripts && Options::get().NoInBoundsSubscripts) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoInBoundsSubscripts.ArgStr);
    Options::get().InBoundsSubscripts.error(Msg);
    exit(1);
  }
  mGlobalOpts.AnalyzeLibFunc = Options::get().AnalyzeLibFunc;
  if (Options::get().AnalyzeLibFunc && Options::get().NoAnalyzeLibFunc) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoAnalyzeLibFunc.ArgStr);
    Options::get().AnalyzeLibFunc.error(Msg);
    exit(1);
  }
  mGlobalOpts.IgnoreRedundantMemory =
    Options::get().IgnoreRedundantMemory;
  if (Options::get().IgnoreRedundantMemory &&
      Options::get().NoIgnoreRedundantMemory) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoIgnoreRedundantMemory.ArgStr);
    Options::get().IgnoreRedundantMemory.error(Msg);
    exit(1);
  }
  mGlobalOpts.UnsafeTfmAnalysis = Options::get().UnsafeTfmAnalysis;
  if (Options::get().UnsafeTfmAnalysis &&
      Options::get().NoUnsafeTfmAnalysis) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoUnsafeTfmAnalysis.ArgStr);
    Options::get().UnsafeTfmAnalysis.error(Msg);
    exit(1);
  }
  mGlobalOpts.NoExternalCalls = Options::get().NoExternalCalls;
  if (Options::get().ExternalCalls && Options::get().NoExternalCalls) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().NoExternalCalls.ArgStr);
    Options::get().ExternalCalls.error(Msg);
    exit(1);
  }
  mGlobalOpts.OptRegions = Options::get().OptRegion;
  mGlobalOpts.AnalysisUse = Options::get().AnalysisUse;
  mEmitAST = addLLIfSet(addIfSet(Options::get().EmitAST));
  mMergeAST = mEmitAST ?
    addLLIfSet(addIfSet(Options::get().MergeAST)) :
    addLLIfSet(Options::get().MergeAST);
  mPrintAST = addLLIfSet(addIfSet(Options::get().PrintAST));
  mDumpAST = addLLIfSet(addIfSet(Options::get().DumpAST));
  mOutputPasses = Options::get().OutputPasses;
  mEmitLLVM = addIfSet(Options::get().EmitLLVM);
  mInstrLLVM = addIfSet(Options::get().InstrLLVM);
  mInstrEntry = Options::get().InstrEntry;
  mInstrStart = Options::get().InstrStart;
  if (!mInstrLLVM && (!mInstrEntry.empty() || !mInstrStart.empty()))
    errs() << "WARNING: Instrumentation options are ignored when "
              "-instr-llvm is not set.\n";
  mCheck = addLLIfSet(Options::get().Check);
  mOutputFilename = Options::get().Output;
  storePrintOptions(IncompatibleOpts);
  mLanguage = Options::get().Language;
  /// TODO (kaniandr@gmail.com): allow to use -output-suffix option for
  /// instrumentation and emit LLVM passes.
  bool NoTfmPass = !mTfmPass && !mInstrLLVM && !mEmitLLVM;
  mServer =
      addIfSetIf(Options::get().UseServer, !mPrint && (!NoTfmPass || mCheck));
  if (!Options::get().PrintStep.empty() && mServer) {
    std::string Msg("error - this option is incompatible with");
    Msg.append(" -").append(Options::get().PrintStep.ArgStr);
    Options::get().UseServer.error(Msg);
    exit(1);
  }
  mGlobalOpts.NoFormat = addIfSetIf(Options::get().NoFormat, NoTfmPass);
  mGlobalOpts.OutputSuffix = Options::get().OutputSuffix;
  if (NoTfmPass && !mGlobalOpts.OutputSuffix.empty()) {
    IncompatibleOpts.push_back(&Options::get().OutputSuffix);
    LLIncompatibleOpts.push_back(&Options::get().OutputSuffix);
  }
  if (IncompatibleOpts.size() > 1) {
    std::string Msg("error - this option is incompatible with");
    for (unsigned I = 1; I < IncompatibleOpts.size(); ++I)
      Msg.append(" -").append(IncompatibleOpts[1]->ArgStr);
    IncompatibleOpts[0]->error(Msg);
    exit(1);
  }
  // Now, we check that there are no options which are incompatible with .ll
  // source file (if such file exists in the command line).
  if (mLanguage.empty() && !LLIncompatibleOpts.empty()) {
    auto LLSrcItr = std::find_if(mSources.begin(), mSources.end(),
      [](StringRef Src) {
        return FrontendOptions::getInputKindForExtension(
          sys::path::extension(Src)).getLanguage() == InputKind::LLVM_IR; });
    if (LLSrcItr != mSources.end()) {
      std::string Msg();
      LLIncompatibleOpts[0]->error(
        Twine("error - this option is incompatible with ") + *LLSrcItr);
      exit(1);
    }
  }
}

int Tool::run(QueryManager *QM) {
  std::vector<std::string> NoASTSources;
  std::vector<std::string> SourcesToMerge;
  std::vector<std::string> LLSources;
  std::vector<std::string> NoLLSources;
  bool IsLLVMSources = false;
  for (auto &Src : mSources) {
    auto InputKind = FrontendOptions::getInputKindForExtension(
        sys::path::extension(Src).substr(1)); // ignore first . in extension
    if (mLanguage != "ast" && InputKind.getLanguage() == InputKind::LLVM_IR)
      LLSources.push_back(Src);
    else
      NoLLSources.push_back(Src);
    if (mLanguage != "ast" && InputKind.getFormat() != InputKind::Precompiled)
      NoASTSources.push_back(Src);
    else
      SourcesToMerge.push_back(Src);
  }
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
    return EmitPCHTool.run(
      newFrontendActionFactory<GeneratePCHAction, GenPCHPragmaAction>().get());
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
      QM = getInstrLLVMQM(mInstrEntry, mInstrStart);
    else if (mTfmPass)
      QM = getTransformationQM(mTfmPass, mGlobalOpts);
    else if (mCheck)
      QM = getCheckQM();
    else
      QM = getDefaultQM(mServer, mOutputPasses, mPrintPasses,
        (DefaultQueryManager::ProcessingStep)mPrintSteps, mGlobalOpts);
  }
  auto ImportInfoStorage = QM->initializeImportInfo();
  if (mMergeAST) {
    ClangTool CTool(*mCompilations, SourcesToMerge.back());
    SourcesToMerge.pop_back();
    if (mDumpAST)
      return CTool.run(newFrontendActionFactory<
        tsar::ASTDumpAction, tsar::ASTMergeAction>(SourcesToMerge).get());
    if (mPrintAST)
      return CTool.run(newFrontendActionFactory<
        tsar::ASTPrintAction, tsar::ASTMergeAction>(SourcesToMerge).get());
    if (!ImportInfoStorage)
      return CTool.run(
        newAnalysisActionFactory<MainAction, tsar::ASTMergeAction>(
          mCommandLine, QM, SourcesToMerge).get());
    return CTool.run(
      newAnalysisActionFactory<MainAction, ASTMergeActionWithInfo>(
      mCommandLine, QM, SourcesToMerge, ImportInfoStorage).get());
  }
  ClangTool CTool(*mCompilations, NoLLSources);
  if (mDumpAST)
    return CTool.run(newFrontendActionFactory<
      tsar::ASTDumpAction, tsar::GenPCHPragmaAction>().get());
  if (mPrintAST)
    return CTool.run(newFrontendActionFactory<
      tsar::ASTPrintAction, tsar::GenPCHPragmaAction>().get());
  // Do not search pragmas in .ll file to avoid internal assertion fails.
  ClangTool CLLTool(*mCompilations, LLSources);
  return
    CTool.run(newAnalysisActionFactory<MainAction, GenPCHPragmaAction>(
      mCommandLine, QM).get()) ||
    CLLTool.run(newAnalysisActionFactory<MainAction>(mCommandLine, QM).get()) ?
    1 : 0;
}
