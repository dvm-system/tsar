//===- Rewriter.h -----Rewriter for Fortran Programs (Flang)-----*- C++ -*-===//
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
// This file provides implementation of Clang-like rewriter for Fortran
// programs. The rewriter relies on Flang to parse and represent sources.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_REWRITER_H
#define TSAR_FLANG_REWRITER_H

#include "tsar/Support/RewriterBase.h"
#include <flang/Parser/provenance.h>
#include <flang/Parser/source.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/IntervalMap.h>

namespace tsar {
class FlangRewriter {
  struct FileRewriterInfo {
    using SourceLocationT = Fortran::parser::Provenance;
    using SourceRangeT = Fortran::parser::ProvenanceRange;
  };

  class FileRewriter : public RewriterBase<FlangRewriter, FileRewriterInfo> {
  public:

    FileRewriter(const Fortran::parser::SourceFile *F,
        Fortran::parser::ProvenanceRange R) : mFile(F), mRange(R) {
      assert(F && "File must not be null!");
    }

    Fortran::parser::ProvenanceRange getRange() const { return mRange; }
    const Fortran::parser::SourceFile & getFile() const noexcept {
        return *mFile;
    }
    bool hasModification() const {
      return !mFile->content().equals(
          llvm::makeArrayRef(mBuffer.data(), mBuffer.length()));
    }

    llvm::raw_ostream & write(llvm::raw_ostream &OS) const {
      return OS << mBuffer;
    }

    void ExtendRange(Fortran::parser::ProvenanceRange R) {
      mRange.ExtendToCover(R);
    }

    void InitBuffer() {
      mBuffer.assign(mFile->content().begin(), mFile->content().end());
      mMapping.resize(2 * mRange.size());
    }

    void Map(Fortran::parser::Provenance P, Fortran::parser::SourcePosition SP);

    bool ReplaceText(Fortran::parser::ProvenanceRange PR,
        llvm::StringRef NewStr) {
      if (!mRange.Contains(PR))
        return true;
      ReplaceText(PR.start().offset() - mRange.start().offset(), PR.size(),
                  NewStr);
      return false;
    }

    bool InsertText(Fortran::parser::Provenance P, llvm::StringRef NewStr,
        bool InsertAfter) {
      if (!mRange.Contains(P))
        return true;
      InsertText(P.offset() - mRange.start().offset(), NewStr, InsertAfter);
      return false;
    }

    std::size_t ReplacedSize(Fortran::parser::ProvenanceRange PR) {
      assert(mRange.Contains(PR) && "File must contain a specified range!");
      auto OrigBegin{PR.start().offset() - mRange.start().offset()};
      auto OrigEnd{OrigBegin + PR.size() - 1};
      return mMapping[2 * OrigEnd] - mMapping[2 * OrigBegin + 1] + 1;
    }

  private:
    using RewriterBaseImpl::ReplaceText;
    using RewriterBaseImpl::InsertText;

    const Fortran::parser::SourceFile *mFile;
    Fortran::parser::ProvenanceRange mRange;
  };

  using FileMap = llvm::DenseMap<
    const Fortran::parser::SourceFile *, std::unique_ptr<FileRewriter>>;
  using ProvenanceMap = llvm::IntervalMap<std::size_t, FileRewriter *>;

public:
  using buffer_iterator = FileMap::const_iterator;

  explicit FlangRewriter(const Fortran::parser::CookedSource &Cooked,
                         Fortran::parser::AllCookedSources &AllCooked);

  bool isMacroOrCompiler(Fortran::parser::Provenance P) const {
    if (mMainFile && mMainFile->getRange().Contains(P))
      return false;
    auto I{mIntervals.find(P.offset())};
    return I == mIntervals.end();
  }

  bool isInclude(Fortran::parser::Provenance P) const {
    if (mMainFile && mMainFile->getRange().Contains(P))
      return false;
    auto I{mIntervals.find(P.offset())};
    return I != mIntervals.end();
  }

  bool hasModification() const {
    if (mHasModification)
      return true;
    for (auto &[File, Info]: mFiles)
      if (mHasModification |= Info->hasModification())
        return true;
    return false;
  }

  bool ReplaceText(Fortran::parser::ProvenanceRange PR,
      llvm::StringRef NewStr) {
    if (auto *File{findFile(PR)})
      return File->ReplaceText(PR, NewStr);
    return true;
  }

  bool InsertText(Fortran::parser::Provenance P, llvm::StringRef NewStr,
      bool InsertAfter = true) {
    if (auto *File{findFile(P)})
      File->InsertText(P, NewStr, InsertAfter);
    return true;
  }

  bool InsertTextBefore(Fortran::parser::Provenance P, llvm::StringRef NewStr) {
    return InsertText(P, NewStr, false);
  }

  bool InsertTextAfter(Fortran::parser::Provenance P, llvm::StringRef NewStr) {
    return InsertText(P, NewStr, true);
  }

  buffer_iterator buffer_begin() const { return mFiles.begin(); }
  buffer_iterator buffer_end() const { return mFiles.end(); }

  const auto * getMainBuffer() const noexcept { return mMainFile; }

  void print(llvm::raw_ostream &OS);
  void dump();

private:
  FileRewriter *findFile(Fortran::parser::ProvenanceRange PR) {
    if (mMainFile && mMainFile->getRange().Contains(PR))
      return mMainFile;
    else if (auto FileItr{mIntervals.find(PR.start().offset())};
             FileItr != mIntervals.end())
      return FileItr.value();
    return nullptr;
  }

  FileMap mFiles;
  ProvenanceMap::Allocator mAllocator;
  ProvenanceMap mIntervals{mAllocator};
  FileRewriter *mMainFile{nullptr};
  mutable bool mHasModification{false};
};
}
#endif//TSAR_FLANG_REWRITER_H
