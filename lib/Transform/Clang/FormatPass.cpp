//===- FormatPass.cpp - Source-level Reformat Pass (Clang) ------*- C++ -*-===//
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
// This file defines a pass to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/FormatPass.h"
#include "ClangUtils.h"
#include "Diagnostic.h"
#include "tsar_transformation.h"
#include <clang/AST/ASTContext.h>
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
#define DEBUG_TYPE "clang-format"

char ClangFormatPass::ID = 0;
INITIALIZE_PASS_BEGIN(ClangFormatPass, "clang-format",
  "Source-level Formatting (Clang)", false, false)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangFormatPass, "clang-format",
  "Source-level Formatting (Clang)", false, false)

ModulePass* llvm::createClangFormatPass() { return new ClangFormatPass(); }

ModulePass* llvm::createClangFormatPass(StringRef OutputSuffix, bool NoFormat) {
  return new ClangFormatPass(OutputSuffix, NoFormat);
}

void ClangFormatPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

bool ClangFormatPass::runOnModule(llvm::Module& M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto &TfmRewriter = TfmCtx->getRewriter();
  auto &SrcMgr = TfmRewriter.getSourceMgr();
  auto &LangOpts = TfmRewriter.getLangOpts();
  auto &Diags = SrcMgr.getDiagnostics();
  auto Adjuster = mOutputSuffix.empty() ? getPureFilenameAdjuster() :
    [this](llvm::StringRef Filename) -> std::string {
      SmallString<128> Path = Filename;
      sys::path::replace_extension(Path,
        "." + mOutputSuffix + sys::path::extension(Path));
    return Path.str();
  };
  Rewriter FormatRewriter(SrcMgr, LangOpts);
#ifdef LLVM_DEBUG
  StringSet<> TransformedFiles;
#endif
  for (auto &Buffer :
      make_range(TfmRewriter.buffer_begin(), TfmRewriter.buffer_end())) {
    auto StartLoc = SrcMgr.getLocForStartOfFile(Buffer.first);
    if (SrcMgr.getFileCharacteristic(StartLoc) != clang::SrcMgr::C_User) {
      toDiag(Diags, StartLoc, diag::err_transform_system);
      continue;
    }
    auto *OrigFile = SrcMgr.getFileEntryForID(Buffer.first);
    assert(TransformedFiles.insert(OrigFile->getName()).second &&
      "Multiple rewriter buffers for the same file does not allowed!");
    // Backup original files if they will be overwritten due to empty output
    // suffix.
    if (mOutputSuffix.empty()) {
      std::error_code Err = sys::fs::copy_file(OrigFile->getName(),
        getBackupFilenameAdjuster()(OrigFile->getName()));
      if (Err) {
        toDiag(Diags, StartLoc, diag::err_backup_file);
        toDiag(Diags, StartLoc, diag::note_not_transform);
        continue;
      }
    }
    if (!mNoFormat) {
      std::string TfmSrc(Buffer.second.begin(), Buffer.second.end());
      auto EndLoc = SrcMgr.getLocForEndOfFile(Buffer.first);
      auto ReformatSrc = reformat(TfmSrc, Adjuster(OrigFile->getName()));
      if (!ReformatSrc) {
        toDiag(Diags, StartLoc, diag::warn_reformat);
        continue;
      }
      auto CurrSize = Buffer.second.size();
      Buffer.second.InsertTextBefore(0, ReformatSrc.get());
      Buffer.second.RemoveText(0, CurrSize);
    }
  }
  TfmCtx->release(Adjuster);
  return false;
}
