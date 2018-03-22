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
// parallelization SAPFOR. The main goal of analyzer is to determine
// data dependences, privatizable and induction variables and other traits of
// analyzed program which could be helpful to parallelize program automatically.
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
#include "tsar_query.h"
#include "tsar_tool.h"
#include "tsar_transformation.h"
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
#include <llvm/Support/raw_ostream.h>
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
JSON_OBJECT_ROOT_PAIR_4(CommandLine,
  Args, std::vector<const char *>,
  Input, const char *,
  Output, const char *,
  Error, const char *)

  CommandLine() :
    JSON_INIT_ROOT,
    JSON_INIT(CommandLine,
      std::vector<const char *>(), nullptr, nullptr, nullptr) {}

  ~CommandLine() {
    auto &This = *this;
    for (auto &Arg : This[CommandLine::Args])
      if (Arg)
        delete[] Arg;
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
  explicit ServerQueryManager(IntrusiveConnection &C,
      RedirectIO &StdIn, RedirectIO &StdOut, RedirectIO &StdErr) :
    mConnection(C), mStdIn(StdIn), mStdOut(StdOut), mStdErr(StdErr) {}

  void run(llvm::Module *M, TransformationContext *Ctx) override {
    assert(M && "Module must not be null!");
    legacy::PassManager Passes;
    if (Ctx) {
      auto TEP = static_cast<TransformationEnginePass *>(
        createTransformationEnginePass());
      TEP->setContext(*M, Ctx);
      Passes.add(TEP);
    }
    Passes.add(createUnreachableBlockEliminationPass());
    Passes.add(createPostOrderFunctionAttrsLegacyPass());
    Passes.add(createMemoryMatcherPass());
    Passes.add(createPrivateServerPass(mConnection, mStdErr));
    Passes.add(createVerifierPass());
    Passes.run(*M);
  }
private:
  IntrusiveConnection &mConnection;
  RedirectIO &mStdIn;
  RedirectIO &mStdOut;
  RedirectIO &mStdErr;
};

void run(IntrusiveConnection C) {
  typedef json::Parser<msg::Diagnostic> Parser;
  llvm::llvm_shutdown_obj ShutdownObj;
  std::unique_ptr<Tool> Analyzer;
  RedirectIO StdIn, StdOut, StdErr;
  C.answer([&Analyzer, &StdIn, &StdOut, &StdErr](
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
    Analyzer = std::move(llvm::make_unique<Tool>(
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
  ServerQueryManager QM(C, StdIn, StdOut, StdErr);
  Analyzer->run(&QM);
  if (StdErr.isDiff()) {
    C.answer([&StdErr](const std::string &) {
      msg::Diagnostic Diag(msg::Status::Error);
      Diag[msg::Diagnostic::Terminal] += StdErr.diff();
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    });
  }
}
}

namespace bcl {
template<> BCL_DECLSPEC
void createServer<std::string>(const Socket<std::string> *S) {
  IntrusiveConnection::connect(S, '$', run);
}
}
