//===--- Action.cpp --------- TSAR Frontend Action --------------*- C++ -*-===//
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
// This file implements front-end action which is necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/Action.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Timer.h>
#include <memory>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace clang {
/// This consumer builds LLVM IR for the specified file and launch analysis of
/// the LLVM IR.
class AnalysisConsumer : public ASTConsumer {
public:
  /// Constructor.
  AnalysisConsumer(CompilerInstance &CI, StringRef InFile,
                   TransformationInfo &TfmInfo, QueryManager &QM)
      : mLLVMIRGeneration("mLLVMIRGeneration", "LLVM IR Generation Time"),
        mCI(&CI), mASTContext(nullptr), mLLVMContext(new LLVMContext),
        mGen(CreateLLVMCodeGen(
            CI.getDiagnostics(), InFile, &CI.getVirtualFileSystem(),
            CI.getHeaderSearchOpts(), CI.getPreprocessorOpts(),
            CI.getCodeGenOpts(), *mLLVMContext)),
        mTransformInfo(&TfmInfo), mQueryManager(&QM) {}

  void HandleCXXStaticMemberVarInstantiation(VarDecl *VD) override {
    mGen->HandleCXXStaticMemberVarInstantiation(VD);
  }

  void Initialize(ASTContext &Ctx) override {
    if (mASTContext) {
      assert(mASTContext == &Ctx &&
        "Existed context must be equal with the specified!");
      return;
    }
    mASTContext = &Ctx;
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->Initialize(Ctx);
    mModule.reset(mGen->GetModule());
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
  }

  bool HandleTopLevelDecl(DeclGroupRef D) override {
    PrettyStackTraceDecl CrashInfo(*D.begin(), SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of declaration");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleTopLevelDecl(D);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
    return true;
  }

  void HandleInlineFunctionDefinition(FunctionDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of inline method");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleInlineFunctionDefinition(D);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
  }

  void HandleTranslationUnit(ASTContext &ASTCtx) override {
    {
      PrettyStackTraceString CrashInfo("Per-file LLVM IR generation");
      if (llvm::TimePassesIsEnabled)
        mLLVMIRGeneration.startTimer();
      mGen->HandleTranslationUnit(ASTCtx);
      if (llvm::TimePassesIsEnabled)
        mLLVMIRGeneration.stopTimer();
    }
    // Silently ignore if we weren't initialized for some reason.
    if (!mModule)
      return;
    // Make sure IR generation is happy with the module. This is released by
    // the module provider.
    llvm::Module *M = mGen->ReleaseModule();
    if (!M) {
      // The module has been released by IR gen on failures, do not double
      // free.
      mModule.release();
      return;
    }
    assert(mModule.get() == M &&
      "Unexpected module change during IR generation");
    Timer LLVMIRAnalysis(
      "LLVMIRAnalysis",
      "LLVM IR Analysis Time");
    if (llvm::TimePassesIsEnabled)
      LLVMIRAnalysis.startTimer();
    auto CUs = M->getNamedMetadata("llvm.dbg.cu");
    if (CUs->getNumOperands() == 1) {
      auto *CU = cast<DICompileUnit>(*CUs->op_begin());
      IntrusiveRefCntPtr<TransformationContextBase> TfmCtx{
          new ClangTransformationContext{*mCI, ASTCtx, *mGen}};
      mTransformInfo->setContext(*CU, std::move(TfmCtx));
    }
    mQueryManager->run(M, mTransformInfo);
    if (llvm::TimePassesIsEnabled)
      LLVMIRAnalysis.stopTimer();
  }

  void HandleTagDeclDefinition(TagDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of declaration");
    mGen->HandleTagDeclDefinition(D);
  }

  void HandleTagDeclRequiredDefinition(const TagDecl *D) override {
    mGen->HandleTagDeclRequiredDefinition(D);
  }

  void CompleteTentativeDefinition(VarDecl *D) override {
    mGen->CompleteTentativeDefinition(D);
  }

  void AssignInheritanceModel(CXXRecordDecl *RD) override {
    mGen->AssignInheritanceModel(RD);
  }

  void HandleVTable(CXXRecordDecl *RD) override {
    mGen->HandleVTable(RD);
  }

private:
  CompilerInstance *mCI;
  Timer mLLVMIRGeneration;
  ASTContext *mASTContext;
  std::unique_ptr<llvm::LLVMContext> mLLVMContext;
  std::unique_ptr<CodeGenerator> mGen;
  TransformationInfo *mTransformInfo;
  QueryManager *mQueryManager;
  std::unique_ptr<llvm::Module> mModule;
};
}

bool ClangMainAction::BeginSourceFileAction(CompilerInstance &CI) {
  TimePassesIsEnabled = CI.getCodeGenOpts().TimePasses;
  return mQueryManager.beginSourceFile(CI.getDiagnostics(), getCurrentFile(),
                                       CI.getFrontendOpts().OutputFile,
                                       CI.getFileSystemOpts().WorkingDir);
}

bool ClangMainAction::shouldEraseOutputFiles() {
  mQueryManager.endSourceFile(
      getCompilerInstance().getDiagnostics().hasErrorOccurred());
  return clang::ASTFrontendAction::shouldEraseOutputFiles();
}

std::unique_ptr<ASTConsumer>
ClangMainAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return std::make_unique<AnalysisConsumer>(CI, InFile, mTfmInfo,
                                            mQueryManager);
}
