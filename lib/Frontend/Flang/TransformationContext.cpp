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
#include <flang/Parser/parse-tree-visitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DebugInfoMetadata.h>

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

using NameHierarchyMapT = std::map<SmallVector<std::string, 3>, std::string>;
using MangledToSourceMapT = llvm::StringMap<FlangASTUnitRef>;

void collect(const Module &M, const DICompileUnit &CU,
    NameHierarchyMapT &NameHierarchy) {
  for (auto &F : M) {
    if (auto *DISub = F.getSubprogram(); DISub && DISub->getUnit() == &CU) {
      NameHierarchyMapT::key_type Key;
      DIScope *Scope{DISub};
      do {
        if (Scope->getName().empty() &&
            (!FlangTransformationContext::UnnamedProgramStub.empty() ||
             !isa<DISubprogram>(Scope) ||
             !cast<DISubprogram>(Scope)->isMainSubprogram()))
          break;
        Key.push_back(std::string{Scope->getName()});
        Scope = Scope->getScope();
      } while (Scope != &CU && Scope);
      if (Scope != &CU)
        continue;
      std::reverse(Key.begin(), Key.end());
      NameHierarchy.try_emplace(Key, F.getName());
    }
  }
}

void match(semantics::Scope &Parent, NameHierarchyMapT::key_type &Names,
           const NameHierarchyMapT &NameHierarchy,
           const ProgramUnitCollector &Collector, MangledToSourceMapT &Map) {
  if (auto *S{Parent.symbol()}) {
    Names.push_back((Parent.kind() != semantics::Scope::Kind::MainProgram ||
                     !S->name().empty())
                        ? S->name().ToString()
                        : FlangTransformationContext::UnnamedProgramStub.str());
    if (Parent.kind() == semantics::Scope::Kind::Subprogram ||
        Parent.kind() == semantics::Scope::Kind::MainProgram)
      if (auto I = NameHierarchy.find(Names); I != NameHierarchy.end()) {
        auto ParserUnit{Parent.kind() == semantics::Scope::Kind::MainProgram
                            ? Collector.getMainParserUnit()
                            : Collector.findParserUnit(S)};
        assert(ParserUnit &&
               "Representation of a program unit in AST must be known!");
        Map.try_emplace(I->second, ParserUnit, S);
      }
    for (auto &Child : Parent.children())
      match(Child, Names, NameHierarchy, Collector, Map);
    Names.pop_back();
  }
}
}

void FlangTransformationContext::initialize(
    const Module &M, const DICompileUnit &CU) {
  assert(hasInstance() && "Transformation context is not configured!");
  NameHierarchyMapT NameHierarchy;
  collect(M, CU, NameHierarchy);
  ProgramUnitCollector V;
  parser::Walk(mParsing->parseTree(), V);
  for (auto &Child : mContext->globalScope().children()) {
    NameHierarchyMapT::key_type Names;
    match(Child, Names, NameHierarchy, V, mGlobals);
  }
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
