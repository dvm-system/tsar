//=== TransformationContext.cpp  TSAR Transformation Engine (Clang) C++ -*-===//
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
// This file implements Clang-based source level transformation engine which.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include <bcl/tuple_utils.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/FrontendDiagnostic.h>

using namespace tsar;
using namespace llvm;
using namespace clang;

ClangTransformationContext::ClangTransformationContext(CompilerInstance &CI,
    ASTContext &Ctx, CodeGenerator &Gen)
  : TransformationContextBase(TC_Clang)
  , mRewriter(Ctx.getSourceManager(), Ctx.getLangOpts())
  , mCI(&CI), mCtx(&Ctx), mGen(&Gen) {}

llvm::StringRef ClangTransformationContext::getInput() const {
  assert(hasInstance() && "Rewriter is not configured!");
  SourceManager &SM = mRewriter.getSourceMgr();
  FileID FID = SM.getMainFileID();
  const FileEntry *File = SM.getFileEntryForID(FID);
  assert(File && "Main file must not be null!");
  return File->getName();
}

Decl * ClangTransformationContext::getDeclForMangledName(StringRef Name) {
  assert(hasInstance() && "Rewriter is not configured!");
  return const_cast<Decl *>(mGen->GetDeclForMangledName(Name));
}

void ClangTransformationContext::reset(clang::CompilerInstance &CI,
    clang::ASTContext &Ctx, clang::CodeGenerator &Gen) {
  mRewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
  mCI = &CI;
  mCtx = &Ctx;
  mGen = &Gen;
}

std::pair<std::string, bool> ClangTransformationContext::release(
    const FilenameAdjuster &FA) {
  assert(hasInstance() && "Rewriter is not configured!");
  DiagnosticsEngine &Diagnostics = mRewriter.getSourceMgr().getDiagnostics();
  std::unique_ptr<llvm::raw_fd_ostream> OS;
  bool AllWritten = true;
  std::string MainFile;
  for (auto I = mRewriter.buffer_begin(), E = mRewriter.buffer_end();
    I != E; ++I) {
    const FileEntry *Entry =
      mRewriter.getSourceMgr().getFileEntryForID(I->first);
    std::string Name = FA(Entry->getName());
    AtomicallyMovedFile::ErrorT Error;
    {
      AtomicallyMovedFile File(Name, &Error);
      if (File.hasStream())
        I->second.write(File.getStream());
    }
    if (Error) {
      AllWritten = false;
      std::visit(
          [this, &Diagnostics, &Error](const auto &Args) {
            bcl::forward_as_args(Args, [this, &Diagnostics,
                                        &Error](const auto &... Args) {
              (toDiag(Diagnostics, std::get<unsigned>(*Error)) << ... << Args);
            });
          },
          std::get<AtomicallyMovedFile::ErrorArgsT>(*Error));
    } else {
      if (I->first == mRewriter.getSourceMgr().getMainFileID())
        MainFile = Name;
    }
  }
  return std::make_pair(std::move(MainFile), AllWritten);
}

void ClangTransformationContext::release(StringRef Filename,
    const RewriteBuffer &Buffer) {
  assert(hasInstance() && "Rewriter is not configured!");
  DiagnosticsEngine &Diagnostics = mRewriter.getSourceMgr().getDiagnostics();
  std::unique_ptr<llvm::raw_fd_ostream> OS;
  AtomicallyMovedFile::ErrorT Error;
  {
    AtomicallyMovedFile File(Filename, &Error);
    if (File.hasStream())
      Buffer.write(File.getStream());
  }
  if (Error) {
    std::visit(
        [this, &Diagnostics, &Error](const auto &Args) {
          bcl::forward_as_args(Args, [this, &Diagnostics,
                                      &Error](const auto &... Args) {
            (toDiag(Diagnostics, std::get<unsigned>(*Error)) << ... << Args);
          });
        },
        std::get<AtomicallyMovedFile::ErrorArgsT>(*Error));
  }
}
