//===- ClangFormatPass.cpp - Source-level Reformat Pass (Clang) -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines a pass to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#include "ClangFormatPass.h"
#include "Diagnostic.h"
#include "tsar_transformation.h"
#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Format/Format.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>
#include <vector>

using namespace clang;
using namespace clang::format;
using namespace clang::tooling;
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

void ClangFormatPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

static inline FilenameAdjuster getFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    SmallString<128> Path = Filename;
    sys::path::replace_extension(
      Path, ".inl" + sys::path::extension(Path));
    return Path.str();
  };
}

static void reformat(Rewriter &FormatRewriter, FileID FID) {
  auto &SM = FormatRewriter.getSourceMgr();
  llvm::MemoryBuffer* Code = SM.getBuffer(FID);
  if (Code->getBufferSize() == 0)
    return;
  StringRef Buffer = Code->getBuffer();
  StringRef FName = SM.getFileEntryForID(FID)->getName();
  unsigned Offset = SM.getFileOffset(SM.getLocForStartOfFile(FID));
  unsigned Length = SM.getFileOffset(SM.getLocForEndOfFile(FID)) - Offset;
  std::vector<Range> Ranges({ Range(Offset, Length) });
  FormatStyle Style = format::getStyle("LLVM", "", "LLVM");
  Replacements Replaces = sortIncludes(Style, Code->getBuffer(), Ranges, FName);
  auto ChangedCode = applyAllReplacements(Code->getBuffer(), Replaces);
  if (!ChangedCode)
    toDiag(SM.getDiagnostics(), SM.getLocForStartOfFile(FID),
      diag::warn_reformat_include);
  else
    Buffer = ChangedCode.get();
  for (const auto &R : Replaces)
    Ranges.emplace_back(R.getOffset(), R.getLength());
  Replacements FormatChanges = reformat(Style, Buffer, Ranges, FName);
  Replaces = Replaces.merge(FormatChanges);
  if (!applyAllReplacements(Replaces, FormatRewriter))
    toDiag(SM.getDiagnostics(), SM.getLocForStartOfFile(FID),
      diag::warn_reformat);
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
  Rewriter FormatRewriter(SrcMgr, LangOpts);
  for (auto &Buffer :
      make_range(TfmRewriter.buffer_begin(), TfmRewriter.buffer_end())) {
    auto *OrigFile = SrcMgr.getFileEntryForID(Buffer.first);
    auto FID = SrcMgr.createFileID(
      SrcMgr.getFileManager().getFile(getFilenameAdjuster()(OrigFile->getName())),
      SourceLocation(), clang::SrcMgr::C_User);
    reformat(FormatRewriter, FID);
  }
  FormatRewriter.overwriteChangedFiles();
  return false;
}
