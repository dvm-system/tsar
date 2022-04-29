
//===- Rewriter.cpp----Rewriter for Fortran Programs (Flang)-----*- C++ -*-===//
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

#include "tsar/Support/Flang/Rewriter.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "flang-rewriter"

using namespace Fortran;
using namespace llvm;
using namespace tsar;

void FlangRewriter::FileRewriter::Map(Fortran::parser::Provenance P,
    Fortran::parser::SourcePosition SP) {
  auto Offset{mFile->GetLineStartOffset(SP.line) + SP.column - 1};
  auto Idx{2 * (P.offset() - mRange.start().offset())};
  mMapping[Idx] = mMapping[Idx + 1] = Offset;
  LLVM_DEBUG(dbgs() << "[REWRITER]: map provenance offset " << P.offset()
                    << " to buffer offset " << Offset << " of "
                    << mBuffer[Offset] << "\n");
 }

FlangRewriter::FlangRewriter(const Fortran::parser::CookedSource &Cooked,
                             Fortran::parser::AllCookedSources &AllCooked) {
  for (auto &Ch : Cooked.AsCharBlock()) {
    auto Range{AllCooked.GetProvenanceRange(&Ch)};
    assert(Range && "Provenance range must be known!");
    auto Position{AllCooked.GetSourcePositionRange(&Ch)};
    // Check for compiler insertions, beginning spaces and macros.
    if (Position) {
      auto *File{AllCooked.allSources().GetSourceFile(Range->start())};
      assert(File && "File must not be null!");
      auto [Itr, IsNew] = mFiles.try_emplace(File);
      if (IsNew)
        Itr->second = std::make_unique<FileRewriter>(File, *Range);
      else
        Itr->second->ExtendRange(*Range);
    }
  }
  for (auto & [File, Info] : mFiles) {
    LLVM_DEBUG(dbgs() << "[REWRITER]: map provenance "
                      << "[" << Info->getRange().start().offset()
                      << "," << Info->getRange().start().offset() +
                                    Info->getRange().size() - 1
                      << "] to " << Info->getFile().path() << "\n");
    Info->InitBuffer();
    mIntervals.insert(Info->getRange().start().offset(),
      Info->getRange().start().offset() + Info->getRange().size() - 1,
      Info.get());
  }
  for (auto &Ch : Cooked.AsCharBlock()) {
    auto Range{AllCooked.GetProvenanceRange(&Ch)};
    auto Position{AllCooked.GetSourcePositionRange(&Ch)};
    // Check for compiler insertions, beginning spaces and macros.
    if (Position) {
      auto *FI{mIntervals.find(Range->start().offset()).value()};
      LLVM_DEBUG(dbgs() << "[REWRITER]: map " << Ch << " at "
                        << Position->first.file.path() << ":"
                        << Position->first.line << ":"
                        << Position->first.column << "\n");
      FI->Map(Range->start(), Position->first);
    }
  }
  auto FirstRange{AllCooked.allSources().GetFirstFileProvenance()};
  auto *FirstFile{AllCooked.allSources().GetSourceFile(FirstRange->start())};
  assert(FirstFile && mFiles.count(FirstFile) &&
      "Main file must not be null!");
  mMainFile = mFiles[FirstFile].get();
  LLVM_DEBUG(dbgs() << "[REWRITER]: set main file to "
                    << mMainFile->getFile().path() << "\n");
}
void FlangRewriter::print(llvm::raw_ostream &OS) {
  for (auto &[File, Info]: mFiles) {
    dbgs() << "Print " << File->path() << " after transformation\n";
    OS << Info->getBuffer();
    dbgs() << "Done\n";
  }
}

LLVM_DUMP_METHOD void FlangRewriter::dump() { print(dbgs()); }
