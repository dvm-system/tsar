//===-------- main.cpp ------ Traits Static Analyzer ------------*- C++ -*-===//
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
//===----------------------------------------------------------------------===//

#include <clang/Tooling/Tooling.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/Tooling.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassNameParser.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Analysis/Passes.h>
#include "tsar_exception.h"
#include "tsar_pass.h"
#include "tsar_action.h"

namespace Base {
const Base::TextAnsi TextToAnsi(const Base::Text &text) {
  return text;
}
}

using namespace tsar;
using namespace llvm;
using namespace clang;
using namespace clang::tooling;

/// This is a version printer for TSAR.
static void printVersion() {
  outs() << Base::TextToAnsi(TSAR::Acronym::Data() + TEXT(" ") +
    TSAR::Version::Field() + TEXT(" ") +
    TSAR::Version::Data()).c_str();
}

/// Parses command line options.
///
/// \param [in] Argc This is associated with a value specified in the
/// main(int Argc, char **Argv) function.
/// \param [in] Argv This is associated with a value specified in the
/// main(int Argc, char **Argv) function.
/// \param [out]  Sources The list of sources which have been specified in
///  a command line will be stored here.
/// \param [out] EmitOnly This will be set to true if analysis does not require.
/// \return Compilation data base.
static std::unique_ptr<CompilationDatabase> parseCLOptions(
    int Argc, const char **Argv, std::vector<std::string> &Sources,
    bool &EmitOnly) {
  auto &Help = *[]() {
    StringMap<llvm::cl::Option*> &opts = llvm::cl::getRegisteredOptions();
    assert(opts.count("help") == 1 && "Option '-help' must be specified!");
    return opts["help"];
  }();
  static cl::alias HelpA("h", cl::aliasopt(Help), cl::desc("Alias for -help"));
  static cl::list<std::string> SourcePaths(
    cl::Positional, cl::desc("<source0> [... <sourceN>]"), cl::OneOrMore);
  std::vector<cl::OptionCategory *> Categories;
  static cl::OptionCategory CompileCategory("Compilation options");
  Categories.push_back(&CompileCategory);
  static cl::list<std::string> IncludePaths("I", cl::cat(CompileCategory),
    cl::value_desc("path"), cl::desc("Add directory to include search path"));
  static cl::list<std::string> MacroDefs("D", cl::cat(CompileCategory),
    cl::value_desc("name=definition"), cl::desc("Predefine name as a macro"));
  static cl::opt<std::string> LanguageStd("std", cl::cat(CompileCategory),
    cl::value_desc("standard"), cl::desc("Language standard to compile for"));
  static cl::OptionCategory DebugCategory("Debugging options");
  Categories.push_back(&DebugCategory);
  static cl::opt<bool> EmitLLVM(
    "emit-llvm", cl::desc("Emit llvm without analysis"),
    cl::cat(DebugCategory));
  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions(Categories);
  cl::ParseCommandLineOptions(
    Argc, Argv, Base::TextToAnsi(TSAR::Title::Data() +
      TEXT("(") + TSAR::Acronym::Data() + TEXT(")")).c_str());
  Sources = SourcePaths;
  EmitOnly = EmitLLVM;
  std::vector<std::string> CommandLine;
  CommandLine.push_back("-g");
  if (!LanguageStd.empty())
    CommandLine.push_back("-std=" + LanguageStd);
  for (auto &Path : IncludePaths)
    CommandLine.push_back("-I" + Path);
  for (auto &Macro : MacroDefs)
    CommandLine.push_back("-D" + Macro);
  std::unique_ptr<CompilationDatabase> Compilations(
    new FixedCompilationDatabase(".", CommandLine));
  return std::move(Compilations);
}

int main(int Argc, const char** Argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram StackTraceProgram(Argc, Argv);
  EnableDebugBuffering = true;
  llvm_shutdown_obj ShutdownObj; //call llvm_shutdown() on exit
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  std::vector<std::string> Sources;
  bool EmitOnly;
  std::unique_ptr<CompilationDatabase> Compilations =
    parseCLOptions(Argc, Argv, Sources, EmitOnly);
  ClangTool Tool(*Compilations, Sources);
  if (EmitOnly)
    return Tool.run(newFrontendActionFactory<EmitLLVMAnalysisAction>().get());
  return Tool.run(newFrontendActionFactory<AnalysisAction>().get());
}
