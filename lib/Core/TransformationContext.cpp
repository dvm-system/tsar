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
#include "tsar/Support/Diagnostic.h"
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "tsar/Frontend/Clang/TransformationContext.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "transform"

template<> char TransformationEnginePass::ID = 0;
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
    return std::string(Path);
  };
  return FA;
}

bool AtomicallyMovedFile::checkStatus() {
  llvm::sys::fs::file_status Status;
  llvm::sys::fs::status(mFilename, Status);
  if (llvm::sys::fs::exists(Status)) {
    if (!llvm::sys::fs::can_write(mFilename)) {
      std::error_code EC;
      if (mError)
        *mError = std::tuple{tsar::diag::err_fe_unable_to_open_output,
                             std::tuple{std::string{mFilename}, EC.message()}};
      return false;
    }
    if (!llvm::sys::fs::is_regular_file(Status))
      mUseTemporary = false;
  }
  return true;
}


AtomicallyMovedFile::AtomicallyMovedFile(StringRef File, ErrorT *Error) :
  mFilename(File), mError(Error), mUseTemporary(true) {
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
      new llvm::raw_fd_ostream(mFilename, EC, llvm::sys::fs::OF_Text));
    if (EC && mError)
      *mError = std::tuple{tsar::diag::err_fe_unable_to_open_output,
                           std::tuple{std::string{mFilename}, EC.message()}};
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
    if (mError)
      *mError = std::tuple{tsar::diag::err_unable_to_rename_temp,
                           std::tuple{std::string{mTempFilename},
                                      std::string{mFilename}, EC.message()}};
    llvm::sys::fs::remove(mTempFilename.str());
  }
}
