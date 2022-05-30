//=== TransformationContext.cpp - TSAR Transformation Engine (Flang) C++ *-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements Flang-based source level transformation engine which.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/Flang/Diagnostic.h"
#include <bcl/tuple_utils.h>
#include <flang/Lower/Mangler.h>
#include <flang/Parser/parse-tree-visitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DebugInfoMetadata.h>

#define DEBUG_TYPE "flang-transformation"

using namespace tsar;
using namespace llvm;
using namespace Fortran;

namespace {
class ProgramUnitCollector {
public:
  template <typename T> bool Pre(T &N) { return true; }
  template <typename T> void Post(T &N) {}

  bool Pre(parser::ProgramUnit &PU) {
    return std::visit(
        common::visitors{
            [](auto &) { return false; },
            [this, &PU](common::Indirection<parser::MainProgram> &X) {
              mParserMain = &PU;
              return true;
            },
            [this, &PU](common::Indirection<parser::FunctionSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::FunctionStmt>>(
                  X.value().t)};
              const auto &Name{std::get<parser::Name>(S.statement.t)};
              mUnits.try_emplace(Name.symbol, &PU);
              return true;
            },
            [this, &PU](common::Indirection<parser::SubroutineSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::SubroutineStmt>>(
                  X.value().t)};
              const auto &Name{std::get<parser::Name>(S.statement.t)};
              mUnits.try_emplace(Name.symbol, &PU);
              return true;
            },
            [](common::Indirection<parser::Module> &) { return true; }},
        PU.u);
  }

  bool Pre(parser::InternalSubprogram &PU) {
    return std::visit(
        common::visitors{
            [](auto &) { return false; },
            [this, &PU](common::Indirection<parser::FunctionSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::FunctionStmt>>(
                  X.value().t)};
              const auto &Name{std::get<parser::Name>(S.statement.t)};
              mUnits.try_emplace(Name.symbol, &PU);
              return true;
            },
            [this, &PU](common::Indirection<parser::SubroutineSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::SubroutineStmt>>(
                  X.value().t)};
              const auto &Name{std::get<parser::Name>(S.statement.t)};
              mUnits.try_emplace(Name.symbol, &PU);
              return true;
            }},
        PU.u);
  }

  bool Pre(parser::ModuleSubprogram &PU) {
    return std::visit(
        common::visitors{
            [](auto &) { return false; },
            [this, &PU](common::Indirection<parser::FunctionSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::FunctionStmt>>(
                  X.value().t)};
              const auto &Name{std::get<parser::Name>(S.statement.t)};
              mUnits.try_emplace(Name.symbol, &PU);
              return true;
            },
            [this, &PU](common::Indirection<parser::SubroutineSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::SubroutineStmt>>(
                  X.value().t)};
              const auto &Name{std::get<parser::Name>(S.statement.t)};
              mUnits.try_emplace(Name.symbol, &PU);
              return true;
            }},
        PU.u);
  }

  auto getMainParserUnit() const noexcept { return mParserMain; }

  auto findParserUnit(FlangASTUnitRef::SemanticsUnitT U) const {
    auto I{mUnits.find(U)};
    return I != mUnits.end() ? I->second : nullptr;
  }

private:
  FlangASTUnitRef::ParserUnitT mParserMain;
  DenseMap<FlangASTUnitRef::SemanticsUnitT, FlangASTUnitRef::ParserUnitT>
      mUnits;
};

using IntrinsicMapT = llvm::StringMap<SmallVector<const llvm::Function *, 1>>;
using MangledToSourceMapT = llvm::StringMap<FlangASTUnitRef>;

static std::string getMangledName(const Fortran::semantics::Symbol &S) {
  const std::string *BindName = S.GetBindName();
  return BindName ? *BindName : Fortran::lower::mangle::mangleName(S);
}

void match(semantics::Scope &Parent, const ProgramUnitCollector &Collector,
           IntrinsicMapT &Intrinsics, MangledToSourceMapT &Map) {
#ifdef LLVM_DEBUG
  auto log = [](semantics::Symbol &S, StringRef MangledName, bool Add) {
    dbgs() << "[FLANG TRANSFORMATION]: " << (Add ? "add" : "ignore")
           << " demangled symbol '" << S.GetUltimate().name().ToString()
           << "' for '" << MangledName << "' [" << &S << "]\n";
  };
#endif
  for (auto &&[Name, S] : Parent)
    if (S->test(semantics::Symbol::Flag::Function) ||
        S->test(semantics::Symbol::Flag::Subroutine) ||
        S->has<semantics::MainProgramDetails>())
      if (!S->attrs().test(semantics::Attr::INTRINSIC)) {
        auto ParserUnit{S->has<semantics::MainProgramDetails>()
                            ? Collector.getMainParserUnit()
                            : Collector.findParserUnit(&*S)};
        [[maybe_unused]] auto Info{
            Map.try_emplace(getMangledName(S->GetUltimate()), ParserUnit, &*S)};
        LLVM_DEBUG(log(*S, getMangledName(S->GetUltimate()), Info.second));
      } else if (auto I{Intrinsics.find(S->GetUltimate().name().ToString())};
                 I != Intrinsics.end()) {
        for (auto *F : I->second) {
          [[maybe_unused]] auto Info{
              Map.try_emplace(F->getName(), nullptr, &*S)};
          LLVM_DEBUG(log(*S, F->getName(), Info.second));
        }
      }
  for (auto &&[Name, S] : Parent.commonBlocks()) {
    [[maybe_unused]] auto Info{
        Map.try_emplace(getMangledName(S->GetUltimate()), nullptr, &*S)};
    LLVM_DEBUG(log(*S, getMangledName(S->GetUltimate()), Info.second));
  }
  for (auto &Child : Parent.children())
    match(Child, Collector, Intrinsics, Map);
}
}

void FlangTransformationContext::initialize(
    const Module &M, const DICompileUnit &CU) {
  assert(hasInstance() && "Transformation context is not configured!");
  ProgramUnitCollector V;
  parser::Walk(mParsing->parseTree(), V);
  IntrinsicMapT Intrinsics;
  for (auto &F : M) {
    StringRef Prefix{"fir."};
    if (F.getName().startswith(Prefix)) {
      auto GeneralName{F.getName().drop_front(Prefix.size())};
      auto GeneralNameEnd(F.getName().find("."));
      GeneralName = GeneralName.substr(0, GeneralNameEnd);
      Intrinsics.try_emplace(GeneralName).first->second.push_back(&F);
    }
  }
  match(mContext->globalScope(), V, Intrinsics, mGlobals);
  mRewriter = std::make_unique<FlangRewriter>(mParsing->cooked(),
                                              mParsing->allCooked());
}

std::pair<std::string, bool> FlangTransformationContext::release(
    const FilenameAdjuster &FA) {
  assert(hasInstance() && "Rewriter is not configured!");
  std::unique_ptr<llvm::raw_fd_ostream> OS;
  bool AllWritten = true;
  std::string MainFile;
  for (auto I = mRewriter->buffer_begin(), E = mRewriter->buffer_end(); I != E;
       ++I) {
    if (!I->second->hasModification())
      continue;
    const auto *File = I->first;
    std::string Name = FA(File->path());
    AtomicallyMovedFile::ErrorT Error;
    {
      AtomicallyMovedFile File(Name, &Error);
      if (File.hasStream())
        I->second->write(File.getStream());
    }
    if (Error) {
      auto Pos{*mParsing->cooked().GetCharBlock(I->second->getRange())};
      AllWritten = false;
      std::visit(
          [this, &Error, Pos](const auto &Args) {
            bcl::forward_as_args(
                Args, [this, &Error, Pos](const auto &...Args) {
                  toDiag(*mContext, Pos, std::get<unsigned>(*Error), Args...);
                });
          },
          std::get<AtomicallyMovedFile::ErrorArgsT>(*Error));
    } else {
      if (I->second.get() == mRewriter->getMainBuffer())
        MainFile = Name;
    }
  }
  mContext->messages().Emit(errs(), mParsing->allCooked());
  return std::make_pair(std::move(MainFile), AllWritten);
}
