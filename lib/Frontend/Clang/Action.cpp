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
#include "tsar/Core/TransformationContext.h"
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Sema/Sema.h>
#include <clang/Serialization/ASTReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Timer.h>
#include <memory>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

namespace clang {
/// This consumer builds LLVM IR for the specified file and launch analysis of
/// the LLVM IR.
class AnalysisConsumer : public ASTConsumer {
public:
  /// Constructor.
  AnalysisConsumer(CompilerInstance &CI, StringRef InFile,
    TransformationContext *TfmCtx, QueryManager *QM)
    : mLLVMIRGeneration(
      "mLLVMIRGeneration",
      "LLVM IR Generation Time"
    ),
    mCI(&CI), mASTContext(nullptr), mLLVMContext(new LLVMContext),
    mGen(CreateLLVMCodeGen(CI.getDiagnostics(), InFile,
      CI.getHeaderSearchOpts(), CI.getPreprocessorOpts(),
      CI.getCodeGenOpts(), *mLLVMContext)),
    mTransformContext(TfmCtx), mQueryManager(QM) {
    assert(mTransformContext && "Transformation context must not be null!");
  }

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
    mTransformContext->reset(*mCI, Ctx, *mGen);
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
    mQueryManager->run(M, mTransformContext);
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
  TransformationContext *mTransformContext;
  QueryManager *mQueryManager;
  std::unique_ptr<llvm::Module> mModule;
};
}

ActionBase::ActionBase(QueryManager *QM) : mQueryManager(QM) {
  assert(QM && "Query manager must be specified!");
}

bool ActionBase::BeginSourceFileAction(CompilerInstance &CI) {
  TimePassesIsEnabled = CI.getFrontendOpts().ShowTimers;
  return mQueryManager->beginSourceFile(CI, getCurrentFile());
}

void ActionBase::EndSourceFileAction() {
  mQueryManager->endSourceFile();
}

void ActionBase::ExecuteAction() {
  // If this is an IR file, we have to treat it specially.
  if (getCurrentFileKind().getLanguage() != InputKind::LLVM_IR) {
    ASTFrontendAction::ExecuteAction();
    return;
  }
  if (!hasIRSupport()) {
    errs() << getCurrentFile() << " error: requested action is not available\n";
    return;
  }
  bool Invalid;
  CompilerInstance &CI = getCompilerInstance();
  SourceManager &SM = CI.getSourceManager();
  FileID FID = SM.getMainFileID();
  llvm::MemoryBuffer *MainFile = SM.getBuffer(FID, &Invalid);
  if (Invalid)
    return;
  llvm::SMDiagnostic Err;
  LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M =
    parseIR(MainFile->getMemBufferRef(), Err, Ctx);
  if (!M) {
    // Translate from the diagnostic info to the SourceManager location if
    // available.
    SourceLocation Loc;
    if (Err.getLineNo() > 0) {
      assert(Err.getColumnNo() >= 0);
      Loc = SM.translateFileLineCol(SM.getFileEntryForID(FID),
        Err.getLineNo(), Err.getColumnNo() + 1);
    }
    // Strip off a leading diagnostic code if there is one.
    StringRef Msg = Err.getMessage();
    if (Msg.startswith("error: "))
      Msg = Msg.substr(7);
    unsigned DiagID =
      CI.getDiagnostics().getCustomDiagID(DiagnosticsEngine::Error, "%0");
    CI.getDiagnostics().Report(Loc, DiagID) << Msg;
    return;
  }
  const auto &TargetOpts = CI.getTargetOpts();
  if (M->getTargetTriple() != TargetOpts.Triple) {
    CI.getDiagnostics().Report(SourceLocation(),
      diag::warn_fe_override_module)
      << TargetOpts.Triple;
    M->setTargetTriple(TargetOpts.Triple);
  }
  Timer LLVMIRAnalysis(
#if LLVM_VERSION_MAJOR > 3
    "LLVMIRAnalysis",
#endif
    "LLVM IR Analysis Time");
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.startTimer();
  mQueryManager->run(M.get(), nullptr);
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.stopTimer();
}

std::unique_ptr<ASTConsumer>
MainAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return std::unique_ptr<AnalysisConsumer>(
    new AnalysisConsumer(CI, InFile, mTfmCtx.get(), mQueryManager));
}

MainAction::MainAction(ArrayRef<std::string> CL, QueryManager *QM) :
  ActionBase(QM), mTfmCtx(new TransformationContext(CL)) {}
