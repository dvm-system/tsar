//===--- tsar_action.cpp ---- TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements front-end action which is necessary to analyze sources.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Timer.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/CodeGen/Passes.h>
#include <memory>
#include "tsar_action.h"
#include "tsar_pass.h"

using namespace clang;
using namespace llvm;

namespace clang {
static void AnalyseModule(llvm::Module *M) {
  assert(M && "Module must not be null!");
  LLVMContext &Context = getGlobalContext();
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTSAR(Registry);
  legacy::PassManager Passes;
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
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createBasicAliasAnalysisPass());
  Passes.add(createPrivateRecognitionPass());
  Passes.add(createVerifierPass());
  cl::PrintOptionValues();
  Passes.run(*M);
}

class AnalysisConsumer : public ASTConsumer {
public:
  AnalysisConsumer( AnalysisAction::Kind AK,
      raw_pwrite_stream *OS,
      DiagnosticsEngine &Diags,
      const HeaderSearchOptions &HeaderSearchOpts,
      const PreprocessorOptions &PPOpts,
      const CodeGenOptions &CodeGenOpts,
      const TargetOptions &TargetOpts,
      const LangOptions &LangOpts, bool TimePasses,
      const std::string &InFile)
    : mAction(AK), mOS(OS), mDiags(Diags), mCodeGenOpts(CodeGenOpts),
    mTargetOpts(TargetOpts), mLangOpts(LangOpts),
    mLLVMIRGeneration("LLVM IR Generation Time"),
    mASTContext(nullptr), mLLVMContext(new LLVMContext),
    mGen(CreateLLVMCodeGen(Diags, InFile, HeaderSearchOpts, PPOpts,
      CodeGenOpts, *mLLVMContext)) {
    assert(mAction != AnalysisAction::KIND_EMIT_LLVM || !mOS ||
      "Output stream must not be null if emit action is selected!");
    llvm::TimePassesIsEnabled = TimePasses;
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

  void HandleInlineMethodDefinition(CXXMethodDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of inline method");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleInlineMethodDefinition(D);
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
    switch (mAction) {
      case AnalysisAction::KIND_ANALYSIS: AnalyseModule(M); break;
      case AnalysisAction::KIND_EMIT_LLVM: EmitLLVM(); break;
      default: assert("Unknown kind of action, so do nothing!"); break;
    }
  }

  void EmitLLVM() {
    legacy::PassManager Passes;
    Passes.add(createPrintModulePass(*mOS, "", mCodeGenOpts.EmitLLVMUseLists));
    cl::PrintOptionValues();
    Passes.run(*mModule.get());
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

  void HandleVTable(CXXRecordDecl *RD) override {
    mGen->HandleVTable(RD);
  }

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
private:
  DiagnosticsEngine &mDiags;
  const CodeGenOptions &mCodeGenOpts;
  const TargetOptions &mTargetOpts;
  const LangOptions &mLangOpts;
  ASTContext *mASTContext;
  Timer mLLVMIRGeneration;
  std::unique_ptr<llvm::LLVMContext> mLLVMContext;
  std::unique_ptr<llvm::Module> mModule;
  std::unique_ptr<CodeGenerator> mGen;
  AnalysisAction::Kind mAction;
  raw_pwrite_stream *mOS;
};
}

AnalysisAction::AnalysisAction(Kind AK) : mKind(AK) {}

std::unique_ptr<ASTConsumer>
AnalysisAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  raw_pwrite_stream *OS = nullptr;
  switch (mKind) {
  case KIND_EMIT_LLVM:
    OS = CI.createDefaultOutputFile(false, InFile, "ll"); break;
  }
  return std::make_unique<AnalysisConsumer>(
    mKind, OS, CI.getDiagnostics(), CI.getHeaderSearchOpts(),
    CI.getPreprocessorOpts(), CI.getCodeGenOpts(), CI.getTargetOpts(),
    CI.getLangOpts(), CI.getFrontendOpts().ShowTimers ? true : false, InFile);
}

void AnalysisAction::ExecuteAction() {
  // If this is an IR file, we have to treat it specially.
  if (getCurrentFileKind() != IK_LLVM_IR) {
    ASTFrontendAction::ExecuteAction();
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
  const TargetOptions &TargetOpts = CI.getTargetOpts();
  if (M->getTargetTriple() != TargetOpts.Triple) {
    CI.getDiagnostics().Report(SourceLocation(),
      diag::warn_fe_override_module)
      << TargetOpts.Triple;
    M->setTargetTriple(TargetOpts.Triple);
  }
  AnalyseModule(M.get());
  return;
}

bool AnalysisAction::hasIRSupport() const { return true; }

EmitLLVMAnalysisAction::EmitLLVMAnalysisAction() :
  AnalysisAction(AnalysisAction::KIND_EMIT_LLVM) {}
