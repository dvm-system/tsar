//===- Tooling.cpp --------- Flang Based Tool (Flang) ------------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file implements functions to run flang tools standalone instead of
// running them as a plugin.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Flang/Tooling.h"
// TODO (kaniandr@gmail.com): remove the following #undef. It's a trick to avoid
// LLVM compilation issues. LLVM uses LLVM_CLANG_BASIC_CODEGENOPTIONS_H as
// a guard in two .h files that define CodeGenOptions for Clang and Flang.
#undef LLVM_CLANG_BASIC_CODEGENOPTIONS_H
#include "tsar/Frontend/Flang/Action.h"
#include <clang/Driver/DriverDiagnostic.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <flang/Frontend/CompilerInstance.h>
#include <flang/Frontend/FrontendActions.h>
#include <flang/Frontend/TextDiagnosticBuffer.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <string>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

namespace llvm {
// TODO (kaniandr@gmail.com) : there is no corresponding option in flang,
// so we make some kind of hack and initialize it here expclitily.
// It's better to move initialization in ...Action, if possible.
extern bool TimePassesIsEnabled;
}

static CommandLineArguments removeUnusedOptions(const CommandLineArguments &CL,
                                                StringRef Filename) {
  CommandLineArguments AdjustedArgs;
  for (std::size_t I = 0; I < CL.size(); ++I) {
    // Silently remove unsupported internal options.
    auto Arg{StringSwitch<StringRef>(CL[I])
                 .Cases("-O1", "-Xclang", "-disable-llvm-passes", "-g",
                        "-fstandalone-debug", "-gcolumn-info",
                        "-Qunused-arguments", "")
                 .Default(CL[I])};
    if (Arg.empty())
      continue;
    Arg = StringSwitch<StringRef>(CL[I])
              .Cases("-ftime-report", "-fcaret-diagnostics",
                     "-fno-caret-diagnostics", "-fshow-source-location",
                     "-fno-show-source-location", "-fdiscard-value-names",
                     "-fno-discard-value-names", "-v", "")
              .Default(CL[I]);
    // TODO (kaniandr@gmail.com) : there is no cooresponding option in flang,
    // so we make some kind of hack and initialize it here expclitily.
    // It's better to move initialization in ...Action, if possible.
    if (CL[I] == "-ftime-report")
      TimePassesIsEnabled = true;
    if (!Arg.empty())
      AdjustedArgs.push_back(Arg.str());
    else
      errs() << "Skipping unsupported option " << CL[I] << " while processing "
             << Filename << "\n";
  }
  return AdjustedArgs;
}

static CommandLineArguments addLLVMOptions(const CommandLineArguments &CL,
                                           StringRef Filename) {
  CommandLineArguments AdjustedArgs(CL);
  AdjustedArgs.push_back("-mllvm");
  AdjustedArgs.push_back("-disable-external-name-interop");
  return AdjustedArgs;
}

int FlangTool::run(FlangFrontendActionFactory *Factory) {
  appendArgumentsAdjuster(removeUnusedOptions);
  appendArgumentsAdjuster(addLLVMOptions);
  std::vector<std::string> AbsolutePaths;
  AbsolutePaths.reserve(mSourcePaths.size());
  for (const auto &Path : mSourcePaths) {
    auto APath{getAbsolutePath(*mOverlayFileSystem, Path)};
    if (!APath) {
      errs() << "Skipping " << Path
             << ". Error while getting an absolute path: "
             << llvm::toString(APath.takeError()) << "\n";
      continue;
    }
    AbsolutePaths.push_back(std::move(*APath));
  }
  std::string InitialWorkingDir;
  if (mRestoreCWD)
    if (auto CWD{mOverlayFileSystem->getCurrentWorkingDirectory()})
      InitialWorkingDir = std::move(*CWD);
    else
      errs() << "Could not get working directory: " << CWD.getError().message()
             << "\n";
  bool ProcessingFailed{false}, FileSkipped{false};
  for (StringRef File : AbsolutePaths) {
    auto CompileCommandsForFile{mCompilations.getCompileCommands(File)};
    if (CompileCommandsForFile.empty()) {
      errs() << "Skipping " << File << ". Compile command not found.\n";
      FileSkipped = true;
      continue;
    }
    for (auto &CompileCommand : CompileCommandsForFile) {
      if (mOverlayFileSystem->setCurrentWorkingDirectory(
              CompileCommand.Directory))
        report_fatal_error("Cannot chdir int \"" +
                           Twine(CompileCommand.Directory) + "\"!");
      std::vector<std::string> CommandLine{CompileCommand.CommandLine};
      if (mArgsAdjuster)
        CommandLine = mArgsAdjuster(CommandLine, CompileCommand.Filename);
      assert(!CommandLine.empty() && "Command line must not be empty!");
      auto Flang{std::make_unique<Fortran::frontend::CompilerInstance>()};
      Flang->createDiagnostics();
      if (!Flang->hasDiagnostics())
        report_fatal_error(
            "Cannot create diagnostic engine for the frontend driver!");
      auto *DiagsBuffer{new Fortran::frontend::TextDiagnosticBuffer};
      IntrusiveRefCntPtr<DiagnosticIDs> DiagID{new DiagnosticIDs};
      IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts{new DiagnosticOptions};
      DiagnosticsEngine Diags{DiagID, &*DiagOpts, DiagsBuffer};
      std::vector<const char *> RawCommandLine;
      RawCommandLine.reserve(CommandLine.size());
      transform(CommandLine, std::back_inserter(RawCommandLine),
                [](auto &V) { return V.c_str(); });
      auto Success{Fortran::frontend::CompilerInvocation::createFromArgs(
          Flang->getInvocation(), makeArrayRef(RawCommandLine).slice(1), Diags)};
      DiagsBuffer->flushDiagnostics(Flang->getDiagnostics());
      if (!Flang->getFrontendOpts().llvmArgs.empty()) {
        unsigned NumArgs = Flang->getFrontendOpts().llvmArgs.size();
        auto Args{std::make_unique<const char *[]>(NumArgs + 2)};
        Args[0] = "flang (LLVM option parsing)";
        for (unsigned I = 0; I < NumArgs; ++I)
          Args[I + 1] = Flang->getFrontendOpts().llvmArgs[I].c_str();
        // Add additional argument because at least on positional argument is
        // required for TSAR.
        Args[NumArgs + 1] = "";
        llvm::cl::ParseCommandLineOptions(NumArgs + 2, Args.get());
      }
      if (Success) {
        Flang->getInvocation()
            .getFrontendOpts()
            .needProvenanceRangeToCharBlockMappings = true;
        auto Action{Factory->create()};
        Action->setWorkingDir(CompileCommand.Directory);
        Success = Flang->executeAction(*Action);
      }
      if (!Success) {
        errs() << "Error while processing " << File << "\n";
        ProcessingFailed = true;
      }
      Flang->clearOutputFiles(false);
    }
  }
  if (!InitialWorkingDir.empty()) {
    if (auto EC{
            mOverlayFileSystem->setCurrentWorkingDirectory(InitialWorkingDir)})
      llvm::errs() << "Error when trying to restore working dir: "
                   << EC.message() << "\n";
  }
  return ProcessingFailed ? 1 : (FileSkipped ? 2 : 0);
}

void FlangTool::appendArgumentsAdjuster(ArgumentsAdjuster Adjuster) {
  mArgsAdjuster =
      combineAdjusters(std::move(mArgsAdjuster), std::move(Adjuster));
}