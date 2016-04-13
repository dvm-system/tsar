//===--- tsar_action.cpp ---- TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements front-end action which is necessary to analyze and
// transfomr sources. It also implements LLVM passess which initializes rewriter
// to transform sources in subsequent passes.
//
//===----------------------------------------------------------------------===//

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
#include "tsar_pass.h"
#include "tsar_action.h"
#include "tsar_rewriter_init.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

namespace tsar {
/// This class represents general information that is necessary to configure
/// rewriter of sources. A single instance of this class associated with a
/// single source file (with additional include files) and command line options
///  wich is necessary to parse this file before it would be rewritten.
/// To configure rewriter additional information is needed, such information
/// could be reseted multiple times.
class RewriterContext {
public:
  /// Constructor wich specifies general and additional information to configure
  /// rewriter.
  ///
  /// \attention A code generator is going to be taken under control of this
  /// class, so do not free it separately.
  RewriterContext(SourceManager &SM, CompilerInvocation &CI, CodeGenerator *Gen,
    ArrayRef<std::string> &CL) :
    mSourceMgr(&SM), mInvocation(&CI), mGen(Gen),
    mCommandLine(CL), mIsReset(false) {
    assert(Gen &&
      "Code generator must not be null if other additional data is specified!");
    FileID FID = mSourceMgr->getMainFileID();
    const FileEntry *File = mSourceMgr->getFileEntryForID(FID);
    mPath = File->getName();
  }

  /// Constructor wich specifies only general information.
  /// \param [in] Path A source file that would be rewritten.
  /// \param [in] CL Command line arguments to parse the source file.
  RewriterContext(StringRef Path, ArrayRef<std::string> &CL) :
    mPath(Path), mCommandLine(CL), mIsReset(false) {}

  /// Returns a source file.
  ArrayRef<std::string> getSource() const { return makeArrayRef(mPath); }

  /// Returns command line to parse a source file.
  ArrayRef<std::string> getCommandLine() const { return mCommandLine; }

  /// Returns functional to obtain initial names for mangled names.
  Demangler getDemangler() {
    assert(mGen && "Rewriter context has no code generator!");
    return [this](StringRef Name) {
      return const_cast<Decl *>(mGen->GetDeclForMangledName(Name));
    };
  }

  /// Returns source manager.
  SourceManager & getSourceManager() {
    assert(mSourceMgr && "Rewriter context has no source manager!");
    return *mSourceMgr;
  }

  /// Returns language options.
  const LangOptions & getLangOpts() {
    assert(mInvocation && mInvocation->getLangOpts() &&
      "Rewriter context has no language options!");
    return *mInvocation->getLangOpts();
  }

  /// Checks whether the additional information is specified.
  bool hasInstanceData() { return mSourceMgr != nullptr; }

  /// Returns true if instance data have been reseted at least one time.
  bool isInstanceDataReset() { return mIsReset; }

  /// Resets additional information wich is necessary to configure rewriter.
  ///
  /// \attention A code generator is going to be taken under control of this
  /// class, so do not free it separately.
  void resetInstanceData(SourceManager &SM, CompilerInvocation &CI,
    CodeGenerator *Gen) {
    assert(Gen &&
      "Code generator must not be null if other additional data is specified!");
    mSourceMgr = &SM;
    mInvocation = &CI;
    mGen.reset(Gen);
    mIsReset = true;
  }

  /// Resets additional information wich is necessary to configure rewriter.
  ///
  /// \attention The managed code generator will be deleted. It is possible to
  /// use the releaseGenerator() method to releases its the ownership.
  void resetInstanceData() {
    mSourceMgr = nullptr;
    mInvocation = nullptr;
    mGen.reset(nullptr);
    mIsReset = true;
  }

  /// Releases the ownership of the managed code generator.
  CodeGenerator * releaseGenerator() noexcept { mGen.release(); }

private:
  IntrusiveRefCntPtr<SourceManager> mSourceMgr;
  IntrusiveRefCntPtr<CompilerInvocation> mInvocation;
  std::unique_ptr<CodeGenerator> mGen;
  std::vector<std::string> mCommandLine;
  std::string mPath;
  bool mIsReset;
};
}

namespace clang {
/// Analysis the specified module and transformes source file associated with it
/// if rewriter context is specified.
///
/// \attention The rewriter context is going to be taken under control, so do
/// not free it separatly.
static void AnalyzeAndTransform(llvm::Module *M,
    RewriterContext *Ctx = nullptr) {
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
  if (Ctx) {
    auto RewriterInit = static_cast<RewriterInitializationPass *>(
      createRewriterInitializationPass());
    RewriterInit->setRewriterContext(*M, Ctx);
    Passes.add(RewriterInit);
  }
  Passes.add(createBasicAliasAnalysisPass());
  Passes.add(createPrivateRecognitionPass());
  if (Ctx)
    Passes.add(createPrivateCClassifierPass());
  Passes.add(createVerifierPass());
  cl::PrintOptionValues();
  Passes.run(*M);
}

/// This consumer builds LLVM IR for the specified file and launch analysis of
/// the LLVM IR.
class AnalysisConsumer : public ASTConsumer {
public:
  /// Constructor.
  AnalysisConsumer(AnalysisAction::Kind AK, raw_pwrite_stream *OS,
      CompilerInstance &CI, StringRef InFile,
      RewriterContext *RewriterCtx = nullptr,
      ArrayRef<std::string> CL = makeArrayRef<std::string>(""))
    : mAction(AK), mOS(OS),  mLLVMIRGeneration("LLVM IR Generation Time"),
    mLLVMIRAnalysis("LLVM IR Analysis Time"),
    mASTContext(nullptr), mLLVMContext(new LLVMContext),
    mGen(CreateLLVMCodeGen(CI.getDiagnostics(), InFile,
      CI.getHeaderSearchOpts(), CI.getPreprocessorOpts(),
      CI.getCodeGenOpts(), *mLLVMContext)),
    mRewriterContext(RewriterCtx), mCodeGenOpts(CI.getCodeGenOpts()) {
    assert(AnalysisAction::FIRST_KIND <= AK && AK <= AnalysisAction::LAST_KIND
      && "Unknown kind of action!");
    assert((AK != AnalysisAction::KIND_CONFIGURE_REWRITER || mRewriterContext)
      && "For a configure rewriter action context must not be null!");
    assert(mAction != AnalysisAction::KIND_EMIT_LLVM || !mOS ||
      "Output stream must not be null if emit action is selected!");
    TimePassesIsEnabled = CI.getFrontendOpts().ShowTimers;
    switch (mAction) {
    default:
      mRewriterContext.reset(new RewriterContext(CI.getSourceManager(),
        CI.getInvocation(), mGen, CL));
      break;
    case AnalysisAction::KIND_CONFIGURE_REWRITER:
      mRewriterContext->resetInstanceData(CI.getSourceManager(),
        CI.getInvocation(), mGen);
      break;
    }
  }

  ~AnalysisConsumer() {
    // Be careful if kind of action KIND_CONFIGURE_REWRITER the rewriter
    // context should not be deleted whis this consumer.
    if (mAction == AnalysisAction::KIND_CONFIGURE_REWRITER)
      mRewriterContext.release();
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
      case AnalysisAction::KIND_ANALYSIS:
        if (llvm::TimePassesIsEnabled)
          mLLVMIRAnalysis.startTimer();
        AnalyzeAndTransform(M, mRewriterContext.release());
        if (llvm::TimePassesIsEnabled)
          mLLVMIRAnalysis.stopTimer();
        break;
      case AnalysisAction::KIND_EMIT_LLVM: EmitLLVM(); break;
      case AnalysisAction::KIND_CONFIGURE_REWRITER:
        // Do nothing because mRewriterContext has been updated in constructor.
        break;
      default: assert("Unknown kind of action, so do nothing!"); break;
    }
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

  /// Prints LLVM IR to the output stream which has been specified in
  /// a constructor of this action.
  void EmitLLVM() {
    legacy::PassManager Passes;
    Passes.add(createPrintModulePass(*mOS, "", mCodeGenOpts.EmitLLVMUseLists));
    cl::PrintOptionValues();
    Passes.run(*mModule.get());
  }

private:
  AnalysisAction::Kind mAction;
  raw_pwrite_stream *mOS;
  Timer mLLVMIRGeneration;
  Timer mLLVMIRAnalysis;
  ASTContext *mASTContext;
  std::unique_ptr<llvm::LLVMContext> mLLVMContext;
  // Do not remove mGen explicitly, it will be removed with mRewriterContext if
  // it is necessary.
  CodeGenerator *mGen;
  std::unique_ptr<RewriterContext> mRewriterContext;
  const CodeGenOptions mCodeGenOpts;
  std::unique_ptr<llvm::Module> mModule;
};
}

AnalysisActionBase::AnalysisActionBase(Kind AK, RewriterContext *Ctx,
    ArrayRef<std::string> CL) :
  mKind(AK), mRewriterContext(Ctx), mCommandLine(CL) {
  assert(FIRST_KIND <= AK && AK <= LAST_KIND && "Unknown kind of action!");
  assert((AK != KIND_CONFIGURE_REWRITER || Ctx) &&
    "For a configure rewriter action context must not be null!");
}

std::unique_ptr<ASTConsumer>
AnalysisActionBase::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  raw_pwrite_stream *OS = nullptr;
  switch (mKind) {
  case KIND_EMIT_LLVM:
    OS = CI.createDefaultOutputFile(false, InFile, "ll"); break;
  }
  return std::make_unique<AnalysisConsumer>(mKind, OS, CI, InFile,
    mRewriterContext, mCommandLine);
}

void AnalysisActionBase::ExecuteAction() {
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
  AnalyzeAndTransform(M.get());
  return;
}

bool AnalysisActionBase::hasIRSupport() const { return true; }

AnalysisAction::AnalysisAction(ArrayRef<std::string> CL) :
  AnalysisActionBase(KIND_ANALYSIS, nullptr, CL) {}

EmitLLVMAnalysisAction::EmitLLVMAnalysisAction() :
  AnalysisActionBase(KIND_EMIT_LLVM, nullptr, makeArrayRef<std::string>("")) {}

RewriterInitAction::RewriterInitAction(RewriterContext *Ctx) :
  AnalysisActionBase(KIND_CONFIGURE_REWRITER, Ctx,
    makeArrayRef<std::string>("")) {}

#undef DEBUG_TYPE
#define DEBUG_TYPE "rewriter-init"

char RewriterInitializationPass::ID = 0;
INITIALIZE_PASS(RewriterInitializationPass, "rewriter-init",
  "Source Code Rewriter Initializer", true, true)

bool RewriterInitializationPass::runOnModule(llvm::Module &M) {
  auto CtxItr = mRewriterCtx.find(&M);
  assert(CtxItr != mRewriterCtx.end() && "The module has no rewriter context!");
  tsar::RewriterContext *Ctx = CtxItr->second;
  if (Ctx->isInstanceDataReset() || !Ctx->hasInstanceData()) {
    // If condition is not met then this is the first run of this pass under
    // the control of a pass manager for the specified module so it is
    // not necessary to re-parse source code for the module (if additional data
    // is specified). The subsequent launches of this pass require that source
    // code has been re-parsed because it is possible that is has been
    //transformed.
    std::unique_ptr<CompilationDatabase> Compilations(
      new FixedCompilationDatabase(".", Ctx->getCommandLine()));
    ClangTool Tool(*Compilations, Ctx->getSource());
    Tool.run(newAnalysisActionFactory<RewriterInitAction>(Ctx).get());
  }
  mDemangler = Ctx->getDemangler();
  mRewriter.setSourceMgr(Ctx->getSourceManager(), Ctx->getLangOpts());
  return false;
}

void RewriterInitializationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

void RewriterInitializationPass::releaseMemory() {
  for (auto &Ctx : mRewriterCtx)
    delete Ctx.second;
  mRewriterCtx.clear();
}

ModulePass * llvm::createRewriterInitializationPass() {
  return new RewriterInitializationPass();
}
