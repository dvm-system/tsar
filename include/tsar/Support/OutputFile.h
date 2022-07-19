//===----- OutputFile.h ---------- Output File ------------------*- C++ -*-===//
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
// This file defines classes to write output files.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SUPPORT_OUTPUT_FILE_H
#define TSAR_SUPPORT_OUTPUT_FILE_H

#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <memory>

namespace tsar {
class OutputFile {
public:
  /// Create a new output file.
  ///
  /// Implementation is copied from clang::CompilerInstance.
  static llvm::Expected<OutputFile>
  create(llvm::StringRef OutputPath, bool Binary = true,
         bool RemoveFileOnSignal = true, bool UseTemporary = true,
         bool CreateMissingDirectories = true);

  OutputFile(OutputFile &&) = default;
  OutputFile &operator=(OutputFile &&) = default;

  OutputFile(const OutputFile &) = delete;
  OutputFile &operator=(const OutputFile &) = delete;

  ~OutputFile() {
    if (isValid())
      llvm::consumeError(clear("", true));
  }

  /// Finish processing of an output file, keep a temporary file with a given
  /// name.
  ///
  /// If EraseFile is true, attempt to erase a file from disk.
  /// Implementation is copied from clang::CompilerInstance.
  llvm::Error clear(llvm::StringRef WorkingDir = "", bool EraseFile = false);

  bool isBinary() const noexcept { return mBinary; }
  bool isRemoveFileOnSignal() const noexcept { return mRemoveFileOnSignal; }
  bool isCreateMssingDirectories() const noexcept {
    return mCreateMissingDirectories;
  }
  bool useTemporary() const { return !mTemp.empty(); }
  llvm::StringRef getFilename() const { return mFilename; }
  llvm::raw_pwrite_stream &getStream() {
    assert(isValid() && "The file has been already cleared!");
    return *mOS;
  }
  const llvm::sys::fs::TempFile & getTemporary() const {
     assert(useTemporary() && "Temporary file is not used!");
    return mTemp.front();
  }

  bool isValid() const noexcept { return mOS != nullptr; }
  operator bool() const noexcept { return isValid(); }

private:
  OutputFile(llvm::StringRef Filename, bool Binary, bool RemoveFileOnSignal,
             bool CreateMissingDirectories,
             std::unique_ptr<llvm::raw_pwrite_stream> OS,
             llvm::Optional<llvm::sys::fs::TempFile> Temp)
      : mFilename(Filename), mBinary(Binary),
        mRemoveFileOnSignal(RemoveFileOnSignal),
        mCreateMissingDirectories(CreateMissingDirectories),
        mOS(std::move(OS)) {
    if (Temp)
      mTemp.emplace_back(std::move(Temp.getValue()));
  }

  bool mBinary{true};
  bool mRemoveFileOnSignal{true};
  bool mCreateMissingDirectories{true};
  llvm::SmallVector<llvm::sys::fs::TempFile, 1> mTemp;
  std::string mFilename;
  std::unique_ptr<llvm::raw_pwrite_stream> mOS;
};
}
#endif//TSAR_SUPPORT_OUTPUT_FILE_H