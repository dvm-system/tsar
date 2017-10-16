//===--- tsar_action.cpp ---- TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements front-end action which is necessary to analyze and
// transform sources. It also implements LLVM passes which initializes rewriter
// to transform sources in subsequent passes.
//
//===----------------------------------------------------------------------===//

#include <llvm/Config/llvm-config.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
#else
#include <llvm/Analysis/BasicAliasAnalysis.h>
#endif
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Sema/Sema.h>
#include <clang/Serialization/ASTReader.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Pass.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Timer.h>
#include <memory>
#include "tsar_action.h"
#include "tsar_instrumentation.h"
#include "tsar_query.h"
#include "tsar_pass.h"
#include "tsar_transformation.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

void QueryManager::run(llvm::Module *M, TransformationContext *Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  // The 'unreachableblockelim' pass is necessary because implementation
  // of data-flow analysis relies on suggestion that control-flow graph does
  // not contain unreachable basic blocks.
  // For the example int main() {exit(1);} 'clang' will generate the LLVM IR:
  // define i32 @main() #0 {
  // entry:
  //  %retval = alloca i32, align 4
  //  store i32 0, i32* %retval
  //   call void @exit(i32 1) #2, !dbg !11
  //  unreachable, !dbg !11
  // return:
  //  %0 = load i32, i32* %retval, !dbg !12
  //  ret i32 %0, !dbg !12
  //}
  // In other cases 'clang' automatically deletes unreachable blocks.
  if (Pass *P = createInitializationPass())
    Passes.add(P);
  Passes.add(createUnreachableBlockEliminationPass());
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  Passes.add(createBasicAliasAnalysisPass());
#else
  Passes.add(createBasicAAWrapperPass());
#endif
  auto PRP = createPrivateRecognitionPass();
  Passes.add(PRP);
  Passes.add(createFunctionPassPrinter(
    PassRegistry::getPassRegistry()->getPassInfo(PRP->getPassID()), errs()));
  if (Ctx)
    Passes.add(createPrivateCClassifierPass());
  Passes.add(createVerifierPass());
  if (Pass *P = createFinalizationPass())
    Passes.add(P);
  cl::PrintOptionValues();
  Passes.run(*M);
}

bool EmitLLVMQueryManager::beginSourceFile(
    CompilerInstance &CI, StringRef InFile) {
  mOS = CI.createDefaultOutputFile(false, InFile, "ll");
  mCodeGenOpts = &CI.getCodeGenOpts();
  return mOS && mCodeGenOpts;
}

void EmitLLVMQueryManager::run(llvm::Module *M, TransformationContext *) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Pass *P = createInitializationPass())
    Passes.add(P);
  Passes.add(createPrintModulePass(*mOS, "", mCodeGenOpts->EmitLLVMUseLists));
  if (Pass *P = createFinalizationPass())
    Passes.add(P);
  cl::PrintOptionValues();
  Passes.run(*M);
}

Pass *InstrLLVMQueryManager::createInitializationPass() {
  return createInstrumentationPass();
}

namespace clang {
/// This consumer builds LLVM IR for the specified file and launch analysis of
/// the LLVM IR.
class AnalysisConsumer : public ASTConsumer {
public:
  /// Constructor.
  AnalysisConsumer(CompilerInstance &CI, StringRef InFile,
    TransformationContext *TfmCtx, QueryManager *QM)
    : mLLVMIRGeneration(
#if LLVM_VERSION_MAJOR > 3
      "mLLVMIRGeneration",
#endif
      "LLVM IR Generation Time"
    ),
    mASTContext(nullptr), mLLVMContext(new LLVMContext),
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
    mTransformContext->reset(Ctx, *mGen);
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

#if (LLVM_VERSION_MAJOR < 4)
  void HandleInlineMethodDefinition(CXXMethodDecl *D) override {
#else
  void HandleInlineFunctionDefinition(FunctionDecl *D) override {
#endif
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of inline method");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
#if (LLVM_VERSION_MAJOR < 4)
     mGen->HandleInlineMethodDefinition(D);
#else
    mGen->HandleInlineFunctionDefinition(D);
#endif
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
#if LLVM_VERSION_MAJOR > 3
      "LLVMIRAnalysis",
#endif
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

#if (LLVM_VERSION_MAJOR > 3)
  void AssignInheritanceModel(CXXRecordDecl *RD) override {
    mGen->AssignInheritanceModel(RD);
  }
#endif

  void HandleVTable(CXXRecordDecl *RD) override {
    mGen->HandleVTable(RD);
  }

#if (LLVM_VERSION_MAJOR < 4)
  void HandleLinkerOptionPragma(llvm::StringRef Opts) override {
    mGen->HandleLinkerOptionPragma(Opts);
  }

  void HandleDetectMismatch(
      llvm::StringRef Name, llvm::StringRef Value) override {
    mGen->HandleDetectMismatch(Name, Value);
  }

  void HandleDependentLibrary(llvm::StringRef Opts) override {
    mGen->HandleDependentLibrary(Opts);
  }
#endif

private:
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

bool ActionBase::BeginSourceFileAction(
    CompilerInstance &CI, StringRef InFile) {
  TimePassesIsEnabled = CI.getFrontendOpts().ShowTimers;
  return mQueryManager->beginSourceFile(CI, InFile);
}

void ActionBase::EndSourceFileAction() {
  mQueryManager->endSourceFile();
}

void ActionBase::ExecuteAction() {
  // If this is an IR file, we have to treat it specially.
  if (getCurrentFileKind() != IK_LLVM_IR) {
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
