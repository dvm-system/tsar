//===- FormatPass.cpp ---- Source-level Reformat Pass -----------*- C++ -*-===//
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
// This file defines a pass to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/tsar-config.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Transform/Clang/Format.h"
#ifdef FLANG_FOUND
# include "tsar/Frontend/Flang/TransformationContext.h"
# include "tsar/Transform/Flang/Format.h"
#endif
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/AST/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "ast-format"

namespace {
/// This pass tries to reformat sources which have been transformed by
/// previous passes.
class ASTFormatPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  /// Create the pass which adds a specified suffix to transformed sources
  /// (see GlobalOptions) and performs formatting if
  /// (GlobalOptions::NoFormat) is not set.
  ///
  /// Note, if suffix is specified it will be added before the file extension.
  /// If suffix is empty, the original will be stored to
  /// <filenmae>.<extension>.orig and then it will be overwritten.
  ASTFormatPass() : ModulePass(ID) {
    initializeASTFormatPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

char ASTFormatPass::ID = 0;
INITIALIZE_PASS_BEGIN(ASTFormatPass, "ast-format",
  "Source-level Formatting", false, false)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper);
INITIALIZE_PASS_END(ASTFormatPass, "ast-format",
  "Source-level Formatting", false, false)

ModulePass* llvm::createASTFormatPass() { return new ASTFormatPass(); }

void ASTFormatPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

bool ASTFormatPass::runOnModule(llvm::Module& M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto &GlobalOpts{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  auto Adjuster = GlobalOpts.OutputSuffix.empty() ? getPureFilenameAdjuster() :
    [this, &GlobalOpts](llvm::StringRef Filename) -> std::string {
      SmallString<128> Path = Filename;
      sys::path::replace_extension(Path,
        "." + GlobalOpts.OutputSuffix + sys::path::extension(Path));
    return std::string(Path);
  };
  auto *CUs{M.getNamedMetadata("llvm.dbg.cu")};
  bool IsAllValid{true};
  for (auto *Op : CUs->operands())
    if (auto *CU{dyn_cast<DICompileUnit>(Op)}) {
      if (auto *TfmCtx{TfmInfo->getContext(*CU)};
          TfmCtx && TfmCtx->hasInstance()) {
        if (auto *CtxImpl{dyn_cast<ClangTransformationContext>(TfmCtx)})
          IsAllValid &=
              formatSourceAndPrepareToRelease(GlobalOpts, *CtxImpl, Adjuster);
#ifdef FLANG_FOUND
        else if (auto *CtxImpl{dyn_cast<FlangTransformationContext>(TfmCtx)})
          IsAllValid &=
              formatSourceAndPrepareToRelease(GlobalOpts, *CtxImpl, Adjuster);
#endif
        if (IsAllValid)
          TfmCtx->release(Adjuster);
      } else {
        M.getContext().emitError(
            "cannot transform " + CU->getFilename() +
            ": transformation context is not available");
      }
    }
  return false;
}
