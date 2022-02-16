//=== DiagnosticPriner.cpp - Diagnostic Printer (Clang) ----------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements passes to print diagnostics.
//
//===----------------------------------------------------------------------===//

#include "APCContextImpl.h"
#include "DistributionUtils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <bcl/utility.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
class APCClangDiagnosticPrinter : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  APCClangDiagnosticPrinter() : ModulePass(ID) {
    initializeAPCClangDiagnosticPrinterPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

char APCClangDiagnosticPrinter::ID = 0;
INITIALIZE_PASS_BEGIN(APCClangDiagnosticPrinter, "apc-clang-diag-printer",
                      "Diagnostic Printer (APC, Clang)", true, true)
INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(APCClangDiagnosticPrinter, "apc-clang-diag-printer",
                      "Diagnostic Printer (APC, Clang)", true, true)

ModulePass* llvm::createAPCClangDiagnosticPrinter() {
  return new APCClangDiagnosticPrinter;
}

void APCClangDiagnosticPrinter::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

bool APCClangDiagnosticPrinter::runOnModule(llvm::Module &M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  if (!TfmInfo) {
    M.getContext().emitError("cannot transform sources"
                             ": transformation context is not available");
    return false;
  }
  auto *CUs{M.getNamedMetadata("llvm.dbg.cu")};
  auto &APCCtx{getAnalysis<APCContextWrapper>().get()};
  StringSet<> VisitedFiles;
  bool WasError{false};
  for (auto *MD : CUs->operands()) {
    auto *CU{cast<DICompileUnit>(MD)};
    auto *TfmCtx{
        dyn_cast_or_null<ClangTransformationContext>(TfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance()) {
      M.getContext().emitError("cannot transform sources"
                               ": transformation context is not available");
      return false;
    }
    auto &SrcMgr{TfmCtx->getRewriter().getSourceMgr()};
    auto &FileMgr{SrcMgr.getFileManager()};
    SmallVector<const FileEntry *, 32> FEs;
    FileMgr.GetUniqueIDMapping(FEs);
    for (auto *FE : FEs) {
      SmallString<128> PathToFile{FE->getName()};
      FileMgr.makeAbsolutePath(PathToFile);
      if (VisitedFiles.count(PathToFile))
        continue;
      auto DiagsItr{APCCtx.mImpl->Diags.find(PathToFile.str().str())};
      if (DiagsItr == APCCtx.mImpl->Diags.end())
        continue;
      auto FID{SrcMgr.translateFile(FE)};
      if (FID.isInvalid())
        continue;
      VisitedFiles.insert(DiagsItr->first);
      auto &Diags{DiagsItr->second};
      auto DiagIDs{SrcMgr.getDiagnostics().getDiagnosticIDs()};
      for (auto &Msg : Diags) {
        SourceLocation Loc;
        std::pair<unsigned, unsigned> RawLoc;
        bcl::restoreShrinkedPair(Msg.line, RawLoc.first, RawLoc.second);
        if (RawLoc.first == 0) {
          Loc = SrcMgr.getLocForStartOfFile(FID);
        } else {
          // Line and column have to start from 1, so if we do not know column
          // just point to the line beginning.
          if (RawLoc.second == 0)
            ++RawLoc.second;
          Loc = SrcMgr.translateLineCol(FID, RawLoc.first, RawLoc.second);
        }
        SmallString<256> Str;
        messageToString(Msg, Str);
        Str += " (";
        Twine(Msg.group).toVector(Str);
        Str += ")";
        unsigned CustomId{0};
        switch (Msg.type) {
        default:
          llvm_unreachable("Unsupported diagnostic kind!");
        case ERROR:
          WasError = true;
          CustomId = DiagIDs->getCustomDiagID(clang::DiagnosticIDs::Error, Str);
          break;
        case WARR:
          CustomId =
              DiagIDs->getCustomDiagID(clang::DiagnosticIDs::Warning, Str);
          break;
        case NOTE:
          CustomId =
              DiagIDs->getCustomDiagID(clang::DiagnosticIDs::Remark, Str);
          break;
        }
        SrcMgr.getDiagnostics().Report(Loc, CustomId);
      }
    }
  }
  if (WasError)
    M.getContext().emitError("unable to construct a parallel program");
  return false;
}
