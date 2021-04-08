//===- Format.cpp ----- Source-level Reformat Pass (Clang) ------*- C++ -*-===//
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
// This file defines functions to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/Format.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include <clang/AST/ASTContext.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>
#include <vector>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "ast-format"

bool tsar::formatSourceAndPrepareToRelease(
    const GlobalOptions &GlobalOpts, ClangTransformationContext &TfmCtx,
    const FilenameAdjuster &Adjuster) {
  auto &TfmRewriter{TfmCtx.getRewriter()};
  auto &SrcMgr{TfmRewriter.getSourceMgr()};
  auto &LangOpts{TfmRewriter.getLangOpts()};
  auto &Diags{SrcMgr.getDiagnostics()};
#ifdef LLVM_DEBUG
  StringSet<> TransformedFiles;
#endif
  bool IsAllValid{true};
  for (auto &Buffer :
       make_range(TfmRewriter.buffer_begin(), TfmRewriter.buffer_end())) {
    auto StartLoc{SrcMgr.getLocForStartOfFile(Buffer.first)};
    if (SrcMgr.getFileCharacteristic(StartLoc) != clang::SrcMgr::C_User) {
      toDiag(Diags, StartLoc, diag::err_transform_system);
      IsAllValid = false;
      continue;
    }
    auto *OrigFile{SrcMgr.getFileEntryForID(Buffer.first)};
    assert(TransformedFiles.insert(OrigFile->getName()).second &&
      "Multiple rewriter buffers for the same file does not allowed!");
    // Backup original files if they will be overwritten due to empty output
    // suffix.
    if (GlobalOpts.OutputSuffix.empty()) {
      std::error_code Err = sys::fs::copy_file(OrigFile->getName(),
        getBackupFilenameAdjuster()(OrigFile->getName()));
      if (Err) {
        toDiag(Diags, StartLoc, tsar::diag::err_backup_file);
        toDiag(Diags, StartLoc, tsar::diag::note_not_transform);
        IsAllValid = false;
        continue;
      }
    }
    if (!GlobalOpts.NoFormat) {
      std::string TfmSrc(Buffer.second.begin(), Buffer.second.end());
      auto EndLoc = SrcMgr.getLocForEndOfFile(Buffer.first);
      auto ReformatSrc = reformat(TfmSrc, Adjuster(OrigFile->getName()));
      if (!ReformatSrc) {
        toDiag(Diags, StartLoc, tsar::diag::warn_reformat);
        continue;
      }
      auto CurrSize = Buffer.second.size();
      Buffer.second.InsertTextBefore(0, ReformatSrc.get());
      Buffer.second.RemoveText(0, CurrSize);
    }
  }
  return IsAllValid;
}
