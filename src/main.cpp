#include <llvm/InitializePasses.h>
#include "llvm/IR/LegacyPassNameParser.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include "llvm/IR/Verifier.h"
#include <llvm/IRReader/IRReader.h>
#include <llvm/PassManager.h>
#include "llvm/IR/IRPrintingPasses.h"
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>

#include "tsar_exception.h"
#include "tsar_pass.h"

namespace Base
{
    const Base::TextAnsi TextToAnsi(const Base::Text &text)
    {
        return text;
    }
}

using namespace Analyzer;
using namespace llvm;

void PrintVersion( )
{
    outs() << Base::TextToAnsi(Analyzer::TSAR::Acronym::Data() + TEXT(" ") +
                               Analyzer::TSAR::Version::Field( ) + TEXT(" ") +
                               Analyzer::TSAR::Version::Data( )).c_str( );
}

static cl::list<const PassInfo*, bool, PassNameParser> passList(cl::desc("Analysis available (use with -debug-analyzer):"));

static cl::opt<std::string> inputProject(cl::Positional, cl::desc("<analyzed project name>"),
                                         cl::init("-"), cl::value_desc("project"));

static cl::opt<std::string> outputFilename("o", cl::desc("Override output filename"),
                                           cl::value_desc("filename"), cl::Hidden);

static cl::opt<bool> debugMode("debug-analyzer", cl::desc("Enable analysis debug mode"));

int main(int argc, char** argv)
{
    sys::PrintStackTraceOnErrorSignal( );
    PrettyStackTraceProgram stackTracePtrogram(argc, argv);

    EnableDebugBuffering = true;

    llvm_shutdown_obj shutdownObj; //call llvm_shutdown() on exit.

    LLVMContext &context = llvm::getGlobalContext( );

    PassRegistry &registry = *llvm::PassRegistry::getPassRegistry( );
    initializeCore(registry);
    initializeDebugIRPass(registry);
    initializeAnalysis(registry);
    initializeTSAR(registry);

    cl::SetVersionPrinter(PrintVersion);
    cl::ParseCommandLineOptions(argc, argv,
                                Base::TextToAnsi(TSAR::Title::Data( ) +
                                TEXT("(") + TSAR::Acronym::Data( ) + TEXT(")")).c_str( ));

    SMDiagnostic error;

    std::unique_ptr<llvm::Module> module;
    module.reset(ParseIRFile(inputProject, error, context));

    if (!module.get( ))
    {
        error.print(argv[0], errs( ));
        return 1;
    }

    if (outputFilename.empty( ))
        outputFilename = "-";
    std::unique_ptr<tool_output_file> out;
    std::string errorInfo;
    out.reset(new tool_output_file(outputFilename.c_str( ), errorInfo, sys::fs::F_None));
    if (!errorInfo.empty( ))
    {
        error = SMDiagnostic(outputFilename, SourceMgr::DK_Error, errorInfo);
        error.print(argv[0], errs( ));
        return 1;
    }

    PassManager passes;

    if (!debugMode && passList.size( ) > 0)
        errs( ) << argv[0] << ": warning: the pass specification option is ignored in no debug mode (use -debug-analyzer)";

    if (debugMode)
    {
        for (unsigned i = 0; i < passList.size( ); ++i)
        {
            const PassInfo *passInfo = passList[i];
            Pass *pass = passInfo->getNormalCtor( )();
            if (!pass)
            {
                errs( ) << argv[0] << ": error: cannot create pass: " << passInfo->getPassName( ) << "\n";
                return 1;
            }
            passes.add(pass);
        }
    }
    else
    {
        passes.add(createPrivateRecognitionPass( ));

    }

    passes.add(createVerifierPass( ));
    passes.add(createDebugInfoVerifierPass( ));
 
    cl::PrintOptionValues( );

    passes.run(*module.get( ));

    out->keep( );

    return 0;
}
