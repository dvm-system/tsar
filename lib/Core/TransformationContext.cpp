//===- TransformationContext.cpp - TSAR Transformation Engine ---*- C++ -*-===//
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
// This file implements source level transformation engine which is necessary to
// transform high level and low-level representation of program correlated.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/TransformationContext.h"
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>

using namespace tsar;
using namespace llvm;
using namespace clang;

bool AtomicallyMovedFile::checkStatus() {
  llvm::sys::fs::file_status Status;
  llvm::sys::fs::status(mFilename, Status);
  if (llvm::sys::fs::exists(Status)) {
    if (!llvm::sys::fs::can_write(mFilename)) {
      std::error_code EC;
      mDiagnostics.Report(diag::err_fe_unable_to_open_output)
        << mFilename << EC.message();
      return false;
    }
    if (!llvm::sys::fs::is_regular_file(Status))
      mUseTemporary = false;
  }
  return true;
}


AtomicallyMovedFile::AtomicallyMovedFile(DiagnosticsEngine &DE, StringRef File) :
  mDiagnostics(DE), mFilename(File), mUseTemporary(true) {
  if (!checkStatus())
    return;
  if (mUseTemporary) {
    mTempFilename = mFilename;
    mTempFilename += "-%%%%%%%%";
    int FD;
    std::error_code EC =
      llvm::sys::fs::createUniqueFile(mTempFilename.str(), FD, mTempFilename);
    if (!EC)
      mFileStream.reset(new llvm::raw_fd_ostream(FD, true));
    // If it is unable to use temporary, so write to the file directly.
    if (!mFileStream)
      mUseTemporary = false;
  }
  if (!mUseTemporary) {
    std::error_code EC;
    mFileStream.reset(
      new llvm::raw_fd_ostream(mFilename, EC, llvm::sys::fs::F_Text));
    if (EC)
      mDiagnostics.Report(diag::err_fe_unable_to_open_output)
      << mFilename << EC.message();
  }
}

AtomicallyMovedFile::~AtomicallyMovedFile() {
  if (!hasStream()) return;
  mFileStream->flush();
#ifdef LLVM_ON_WIN32
  // Win32 does not allow rename/removing opened files.
  mFileStream.reset();
#endif
  if (!mUseTemporary)
    return;
  if (std::error_code EC =
    llvm::sys::fs::rename(mTempFilename.str(), mFilename)) {
    mDiagnostics.Report(clang::diag::err_unable_to_rename_temp)
      << mTempFilename << mFilename << EC.message();
    llvm::sys::fs::remove(mTempFilename.str());
  }
}

#undef DEBUG_TYPE
#define DEBUG_TYPE "transform"

char TransformationEnginePass::ID = 0;
INITIALIZE_PASS(TransformationEnginePass, "transform",
  "Transformation Engine Accessor", true, true)

ImmutablePass * llvm::createTransformationEnginePass() {
  return new TransformationEnginePass();
}

FilenameAdjuster tsar::getDumpFilenameAdjuster() {
  static FilenameAdjuster FA = [](StringRef Filename) -> std::string {
    static StringMap<unsigned short> DumpFiles;
    auto Pair = DumpFiles.insert(std::make_pair(Filename, 1));
    if (!Pair.second)
      ++Pair.first->getValue();
    auto constexpr MaxDigits =
      std::numeric_limits<unsigned short>::digits10 + 1;
    char Buf[MaxDigits];
    snprintf(Buf, MaxDigits, "%d", Pair.first->getValue());
    SmallString<128> Path = Filename;
    sys::path::replace_extension(Path, Buf + sys::path::extension(Path));
    return Path.str();
  };
  return FA;
}

TransformationContext::TransformationContext(
    ArrayRef<std::string> CL)
  : mCommandLine(CL), mCI(nullptr), mGen(nullptr), mCtx(nullptr) {}

TransformationContext::TransformationContext(CompilerInstance &CI,
  ASTContext &Ctx, CodeGenerator &Gen, llvm::ArrayRef<std::string> CL) :
  mRewriter(Ctx.getSourceManager(), Ctx.getLangOpts()),
  mCtx(&Ctx), mGen(&Gen), mCommandLine(CL) { }

llvm::StringRef TransformationContext::getInput() const {
  assert(hasInstance() && "Rewriter is not configured!");
  SourceManager &SM = mRewriter.getSourceMgr();
  FileID FID = SM.getMainFileID();
  const FileEntry *File = SM.getFileEntryForID(FID);
  assert(File && "Main file must not be null!");
  return File->getName();
}

clang::Decl * TransformationContext::getDeclForMangledName(StringRef Name) {
  assert(hasInstance() && "Rewriter is not configured!");
  return const_cast<Decl *>(mGen->GetDeclForMangledName(Name));
}

void TransformationContext::reset(clang::CompilerInstance &CI,
    clang::ASTContext &Ctx, clang::CodeGenerator &Gen,
    llvm::ArrayRef<std::string> CL) {
  mRewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
  mCI = &CI;
  mCtx = &Ctx;
  mGen = &Gen;
  mCommandLine = CL;
}

void TransformationContext::reset(clang::CompilerInstance &CI,
    clang::ASTContext &Ctx, clang::CodeGenerator &Gen) {
  mRewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
  mCI = &CI;
  mCtx = &Ctx;
  mGen = &Gen;
}

std::pair<std::string, bool> TransformationContext::release(
    const FilenameAdjuster &FA) const {
  DiagnosticsEngine &Diagnostics = mRewriter.getSourceMgr().getDiagnostics();
  std::unique_ptr<llvm::raw_fd_ostream> OS;
  bool AllWritten = true;
  std::string MainFile;
  for (auto I = mRewriter.buffer_begin(), E = mRewriter.buffer_end();
    I != E; ++I) {
    const FileEntry *Entry =
      mRewriter.getSourceMgr().getFileEntryForID(I->first);
    std::string Name = FA(Entry->getName());
    AtomicallyMovedFile File(Diagnostics, Name);
    if (File.hasStream()) {
      I->second.write(File.getStream());
      if (I->first == mRewriter.getSourceMgr().getMainFileID())
        MainFile = Name;
    } else {
      AllWritten = false;
    }
  }
  return std::make_pair(std::move(MainFile), AllWritten);
}

void TransformationContext::release(StringRef Filename,
    const RewriteBuffer &Buffer) {
  DiagnosticsEngine &Diagnostics = mRewriter.getSourceMgr().getDiagnostics();
  std::unique_ptr<llvm::raw_fd_ostream> OS;
  AtomicallyMovedFile File(Diagnostics, Filename);
  if (File.hasStream())
    Buffer.write(File.getStream());
}
