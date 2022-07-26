//===------ Server.cpp ------ Traits Static Analyzer ------------*- C++ -*-===//
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
// parallelization SAPFOR. This file declares interfaces to execute analysis
// and to perform transformations.
//
// This file implements TSAR as a separated server which performs analysis when
// requests from client are received. Connection is based on
// bcl::IntrusiveConnection interface.
//
// The first request from client should be msg::CommandLine which specifies
// analysis options and targets for input/output redirection.
//
//===----------------------------------------------------------------------===//

#include "Messages.h"
#include "Passes.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/Tool.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Transform/IR/Passes.h"
#include "tsar/Support/GlobalOptions.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/Json.h>
#include <bcl/RedirectIO.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Signals.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <map>
#include <vector>

using namespace bcl;
using namespace llvm;
using namespace tsar;

namespace tsar {
namespace msg {
/// \brief This message represents a command line which is used to run a tool.
///
/// This consists of the following elements:
/// - list of arguments which contains options and input data,
/// - specification of an input/output redirection.
JSON_OBJECT_BEGIN(CommandLine)
JSON_OBJECT_ROOT_PAIR_5(CommandLine,
  Args, std::vector<const char *>,
  Query, const char *,
  Input, const char *,
  Output, const char *,
  Error, const char *)

  CommandLine() :
    JSON_INIT_ROOT,
    JSON_INIT(CommandLine,
      std::vector<const char *>(), nullptr, nullptr, nullptr, nullptr) {}

  ~CommandLine() {
    auto &This = *this;
    for (auto &Arg : This[CommandLine::Args])
      if (Arg)
        delete[] Arg;
    if (This[CommandLine::Query])
      delete[] This[CommandLine::Query];
    if (This[CommandLine::Input])
      delete[] This[CommandLine::Input];
    if (This[CommandLine::Output])
      delete[] This[CommandLine::Output];
    if (This[CommandLine::Error])
      delete[] This[CommandLine::Error];
  }

  CommandLine(const CommandLine &) = default;
  CommandLine & operator=(const CommandLine &) = default;
  CommandLine(CommandLine &&) = default;
  CommandLine & operator=(CommandLine &&) = default;

JSON_OBJECT_END(CommandLine)
}
}
JSON_DEFAULT_TRAITS(tsar::msg::, CommandLine)

namespace {
class ServerQueryManager : public QueryManager {
public:
  explicit ServerQueryManager(const GlobalOptions &GO, IntrusiveConnection &C,
      RedirectIO &StdIn, RedirectIO &StdOut, RedirectIO &StdErr)
    : mGlobalOptions(GO), mConnection(C), mStdIn(StdIn), mStdOut(StdOut),
      mStdErr(StdErr) {}

  void run(llvm::Module *M, TransformationInfo *TfmInfo) override {
    assert(M && "Module must not be null!");
    legacy::PassManager Passes;
    Passes.add(createGlobalOptionsImmutableWrapper(&mGlobalOptions));
    if (TfmInfo) {
      auto TEP = static_cast<TransformationEnginePass *>(
        createTransformationEnginePass());
      TEP->set(*TfmInfo);
      Passes.add(TEP);
      Passes.add(createImmutableASTImportInfoPass(mImportInfo));
    }
    addImmutableAliasAnalysis(Passes);
    addInitialTransformations(Passes);
    Passes.add(createMemoryMatcherPass());
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createGlobalsAccessStorage());
    Passes.add(createGlobalsAccessCollector());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(createDIMemoryAnalysisServer());
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createMemoryMatcherPass());
    // Analysis on server changes metadata-level
    // alias tree and invokes corresponding handles to update client to server
    // mapping. So, metadata-level memory mapping is a shared resource and
    // synchronization is necessary.
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createPrivateServerPass(mConnection, mStdErr));
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
    Passes.add(createVerifierPass());
    Passes.run(*M);
  }

  ASTImportInfo * initializeImportInfo() override { return &mImportInfo; }

private:
  const GlobalOptions &mGlobalOptions;
  IntrusiveConnection &mConnection;
  RedirectIO &mStdIn;
  RedirectIO &mStdOut;
  RedirectIO &mStdErr;
  ASTImportInfo mImportInfo;
};

void run(IntrusiveConnection C) {
  typedef json::Parser<msg::Diagnostic> Parser;
  char *ArgvForStackTrace[] = {"TSARServer"};
  sys::PrintStackTraceOnErrorSignal(ArgvForStackTrace[0]);
  PrettyStackTraceProgram StackTraceProgram(1, ArgvForStackTrace);
  EnableDebugBuffering = true;
  llvm::llvm_shutdown_obj ShutdownObj;
  std::unique_ptr<Tool> Analyzer;
  RedirectIO StdIn, StdOut, StdErr;
  bool IsQuerySet = false;
  C.answer([&Analyzer, &StdIn, &StdOut, &StdErr, &IsQuerySet](
      const std::string &Request) -> std::string {
    Parser P(Request);
    msg::CommandLine CL;
    msg::Diagnostic Diag(msg::Status::Error);
    if (!P.parse(CL)) {
      Diag.insert(msg::Diagnostic::Error, P.errors());
      return Parser::unparseAsObject(Diag);
    }
    if (CL[msg::CommandLine::Error])
      StdErr = std::move(
        RedirectIO(STDERR_FILENO, CL[msg::CommandLine::Error]));
    if (CL[msg::CommandLine::Output])
      StdOut = std::move(
        RedirectIO(STDOUT_FILENO, CL[msg::CommandLine::Output]));
    if (CL[msg::CommandLine::Input])
      StdIn = std::move(
        RedirectIO(STDOUT_FILENO, CL[msg::CommandLine::Input],
          RedirectIO::Mode::Read));
    auto InOutError = [&StdIn, &StdOut, &StdErr](msg::Diagnostic &D) {
      if (StdErr.hasErrors())
        D.insert(msg::Diagnostic::Error, StdErr.errors());
      if (StdOut.hasErrors())
        D.insert(msg::Diagnostic::Error, StdOut.errors());
      if (StdIn.hasErrors())
        D.insert(msg::Diagnostic::Error, StdIn.errors());
    };
    InOutError(Diag);
    if (!Diag[msg::Diagnostic::Error].empty())
      return Parser::unparseAsObject(Diag);
    if (IsQuerySet = CL[msg::CommandLine::Query]) {
      CL[msg::CommandLine::Args].push_back(CL[msg::CommandLine::Query]);
      // Set query to nullptr to avoid multiple memory deletion.
      CL[msg::CommandLine::Query] = nullptr;
    }
    Analyzer = std::move(std::make_unique<Tool>(
      CL[msg::CommandLine::Args].size(),
      CL[msg::CommandLine::Args].data()));
    if (StdErr.isDiff())
      Diag[msg::Diagnostic::Terminal] += StdErr.diff();
    InOutError(Diag);
    if (!Diag[msg::Diagnostic::Error].empty() ||
        !Diag[msg::Diagnostic::Terminal].empty())
      return Parser::unparseAsObject(Diag);
    Diag[msg::Diagnostic::Status] = msg::Status::Success;
    return Parser::unparseAsObject(Diag);
  });
  if (!Analyzer)
    return;
  if (IsQuerySet) {
    Analyzer->run();
  } else {
    ServerQueryManager QM(Analyzer->getGlobalOptions(),
      C, StdIn, StdOut, StdErr);
    Analyzer->run(&QM);
  }
  C.answer([&StdErr](const std::string &) {
    msg::Diagnostic Diag(StdErr.isDiff() ? msg::Status::Error
                                         : msg::Status::Done);
    if (StdErr.isDiff())
      Diag[msg::Diagnostic::Terminal] += StdErr.diff();
    return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
  });
}
}

namespace bcl {
template<> BCL_DECLSPEC
void createServer<std::string>(const Socket<std::string> *S) {
  IntrusiveConnection::connect(S, '$', run);
}
}
