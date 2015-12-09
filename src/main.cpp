//===-------- main.cpp ------ Traits Static Analyzer ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Traits Static Analyzer (TSAR) is a part of a system for automated
// parallelization SAPFOR. The main goal of analyzer is to determine
// data dependences, privatizable and induction variables and other traits of
// analyzed program which could be helpfull to parallelize program automatically.
//
//===----------------------------------------------------------------------===//

#include <llvm/InitializePasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/PassManager.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>

#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include "llvm/Analysis/Verifier.h"
#include "llvm/Support/PassNameParser.h"
#else
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassNameParser.h"
#endif

#include "tsar_exception.h"
#include "tsar_pass.h"

namespace Base {
const Base::TextAnsi TextToAnsi(const Base::Text &text) {
  return text;
}
}

using namespace tsar;
using namespace llvm;

void printVersion() {
  outs() << Base::TextToAnsi(TSAR::Acronym::Data() + TEXT(" ") +
                             TSAR::Version::Field() + TEXT(" ") +
                             TSAR::Version::Data()).c_str();
}

static cl::list<const PassInfo*, bool, PassNameParser> gPassList(
  cl::desc("Analysis available (use with -debug-analyzer):"));
static cl::opt<std::string> gInputProject(
  cl::Positional, cl::desc("<analyzed project name>"), cl::init("-"),
  cl::value_desc("project"));
static cl::opt<std::string> gOutputFilename(
  "o", cl::desc("Override output filename"), cl::value_desc("filename"),
  cl::Hidden);
static cl::opt<bool> gDebugMode(
  "debug-analyzer", cl::desc("Enable analysis debug mode"));

int main(int Argc, char** Argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram StackTraceProgram(Argc, Argv);
  EnableDebugBuffering = true;
  llvm_shutdown_obj ShutdownObj; //call llvm_shutdown() on exit
  LLVMContext &Context = getGlobalContext();
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeDebugIRPass(Registry);
  initializeAnalysis(Registry);
  initializeTSAR(Registry);
  cl::SetVersionPrinter(printVersion);
  cl::ParseCommandLineOptions(Argc, Argv,
                              Base::TextToAnsi(TSAR::Title::Data() +
                              TEXT("(") + TSAR::Acronym::Data() + TEXT(")")).c_str());
  SMDiagnostic Error;
  std::unique_ptr<Module> M;
  M.reset(ParseIRFile(gInputProject, Error, Context));
  if (!M.get()) {
    Error.print(Argv[0], errs());
    return 1;
  }
  if (gOutputFilename.empty())
    gOutputFilename = "-";
  std::unique_ptr<tool_output_file> Out;
  std::string ErrorInfo;
  Out.reset(new tool_output_file(gOutputFilename.c_str(), ErrorInfo, sys::fs::F_None));
  if (!ErrorInfo.empty()) {
    Error = SMDiagnostic(gOutputFilename, SourceMgr::DK_Error, ErrorInfo);
    Error.print(Argv[0], errs());
    return 1;
  }
  PassManager Passes;
  if (!gDebugMode && gPassList.size() > 0)
    errs() << Argv[0] << ": warning: the pass specification option is ignored in no debug mode (use -debug-analyzer)";
  if (gDebugMode) {
    for (unsigned i = 0; i < gPassList.size(); ++i) {
      const PassInfo *PI = gPassList[i];
      Pass *P = PI->getNormalCtor()();
      if (!P) {
        errs() << Argv[0] << ": error: cannot create pass: " << PI->getPassName() << "\n";
        return 1;
      }
      Passes.add(P);
    }
  } else {
    Passes.add(createPrivateRecognitionPass());
  }
  Passes.add(createVerifierPass());
#if (!(LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5))
  Passes.add(createDebugInfoVerifierPass());
#endif
  cl::PrintOptionValues();
  Passes.run(*M.get());
  Out->keep();
  return 0;
}
