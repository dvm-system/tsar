//===- Format.cpp ----- Source-level Reformat Pass (Flang) ------*- C++ -*-===//
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
// This file defines functions to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Flang/Format.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/Flang/Diagnostic.h"
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/FileSystem.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "ast-format"

bool tsar::formatSourceAndPrepareToRelease(
    const GlobalOptions &GlobalOpts, FlangTransformationContext &TfmCtx,
    const FilenameAdjuster &Adjuster) {
  auto &TfmRewriter{TfmCtx.getRewriter()};
#ifdef LLVM_DEBUG
  StringSet<> TransformedFiles;
#endif
  bool IsAllValid{true};
  for (auto &[File, Info] :
       make_range(TfmRewriter.buffer_begin(), TfmRewriter.buffer_end())) {
    if (!Info->hasModification())
      continue;
    assert(TransformedFiles.insert(File->path()).second &&
           "Multiple rewriter buffers for the same file does not allowed!");
    // Backup original files if they will be overwritten due to empty output
    // suffix.
    if (GlobalOpts.OutputSuffix.empty()) {
      std::error_code Err = sys::fs::copy_file(
          File->path(), getBackupFilenameAdjuster()(File->path()));
      if (Err) {
        auto Pos{*TfmCtx.getParsing().cooked().GetCharBlock(Info->getRange())};
        toDiag(TfmCtx.getContext(), Pos, tsar::diag::err_backup_file);
        toDiag(TfmCtx.getContext(), Pos, tsar::diag::note_not_transform);
        IsAllValid = false;
        continue;
      }
    }
    if (!GlobalOpts.NoFormat) {
      //TODO (kaniandr@gmail.com): reformat transformed sources
    }
  }
  return IsAllValid;
}
