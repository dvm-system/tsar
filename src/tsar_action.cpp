//===--- tsar_action.cpp ---- TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements front-end action which is necessary to analyze and
// transform sources. It also implements LLVM passes which initializes rewriter
// to transform sources in subsequent passes.
//
//===----------------------------------------------------------------------===//

#include "tsar_action.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "Instrumentation.h"
#include "tsar_query.h"
#include "tsar_pass.h"
#include "tsar_transformation.h"
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Sema/Sema.h>
#include <clang/Serialization/ASTReader.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/CFLAndersAliasAnalysis.h>
#include <llvm/Analysis/CFLSteensAliasAnalysis.h>
#include <llvm/Analysis/GlobalsModRef.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/ScalarEvolutionAliasAnalysis.h>
#include <llvm/Analysis/ScopedNoAliasAA.h>
#include <llvm/Analysis/TypeBasedAliasAnalysis.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Pass.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Timer.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <memory>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

void DefaultQueryManager::addWithPrint(llvm::Pass *P, bool PrintResult,
    llvm::legacy::PassManager &Passes) {
  assert(P->getPotentialPassManagerType() == PMT_FunctionPassManager &&
    "Results of function passes can be printed at this moment only!");
  // PassInfo should be obtained before a pass is added into a pass manager
  // because in some cases pass manager delete this pass. After that pointer
  // becomes invalid. For example, the reason is existence of the same pass in
  // a pass sequence.
  if (PrintResult) {
    auto PI = PassRegistry::getPassRegistry()->getPassInfo(P->getPassID());
    Passes.add(P);
    Passes.add(createFunctionPassPrinter(PI, errs()));
    return;
  }
  Passes.add(P);
};

void DefaultQueryManager::run(llvm::Module *M, TransformationContext *Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  Passes.add(createGlobalOptionsImmutableWrapper(mGlobalOptions));
  auto addInitialAliasAanalysis = [&Passes]() {
    Passes.add(createCFLSteensAAWrapperPass());
    Passes.add(createCFLAndersAAWrapperPass());
    Passes.add(createTypeBasedAAWrapperPass());
    Passes.add(createScopedNoAliasAAWrapperPass());
  };
  auto addPrint = [&Passes, this](ProcessingStep CurrentStep) {
    if (CurrentStep & mPrintSteps)
      for (auto PI : mPrintPasses)
        Passes.add(createFunctionPassPrinter(PI, errs()));
  };
  auto addOutput = [&Passes, this]() {
    for (auto PI : mOutputPasses) {
      if (!PI->getNormalCtor()) {
        /// TODO (kainadnr@gmail.com): add a name of executable before
        /// diagnostic.
        errs() << "warning: cannot create pass: " << PI->getPassName() << "\n";
        continue;
      }
      if (auto *GI = OutputPassGroup::getPassRegistry().groupInfo(*PI))
        GI->addBeforePass(Passes);
      Passes.add(PI->getNormalCtor()());
    }
  };
  addInitialAliasAanalysis();
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
  Passes.add(createInferFunctionAttrsLegacyPass());
  Passes.add(createPostOrderFunctionAttrsLegacyPass());
  Passes.add(createReversePostOrderFunctionAttrsPass());
  Passes.add(createRPOFunctionAttrsAnalysis());
  Passes.add(createPOFunctionAttrsAnalysis());
  Passes.add(createStripDeadPrototypesPass());
  Passes.add(createGlobalDCEPass());
  Passes.add(createGlobalsAAWrapperPass());
  Passes.add(createDILoopRetrieverPass());
  Passes.add(createDIGlobalRetrieverPass());
  Passes.add(createMemoryMatcherPass());
  // It is necessary to destroy DIMemoryTraitPool before DIMemoryEnvironment to
  // avoid dangling handles. So, we add pool before environment in the manager.
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  // Preliminary analysis of privatizable variables. This analysis is necessary
  // to prevent lost of result of optimized values. The memory promotion
  // may remove some variables with attached source-level debug information.
  // Consider an example: int *P, X; P = &X; for (...) { *P = 0; X = 1; }
  // Memory promotion removes P, so X will be recognized as private variable.
  // However in the original program data-dependency exists because different
  // pointers refer the same memory.
  Passes.add(createDIDependencyAnalysisPass());
  if (!mAnalysisUse.empty())
    Passes.add(createAnalysisReader(mAnalysisUse));
  addPrint(BeforeTfmAnalysis);
  addOutput();
  // Perform SROA and repeat variable privatization. After that reduction and
  // induction recognition will be performed. Flow/anti/output dependencies
  // also analyses.
  Passes.add(createCFGSimplificationPass());
  // Do not add 'instcombine' here, because in this case some metadata may be
  // lost after SROA (for example, if a promoted variable is a structure).
  // Passes.add(createInstructionCombiningPass());
  Passes.add(createSROAPass());
  Passes.add(createEarlyCSEPass());
  Passes.add(createCFGSimplificationPass());
  // This is necessary to combine multiple GEPs into a single GEP. This allows
  // dependency analysis works without delinearization.
  Passes.add(createInstructionCombiningPass());
  Passes.add(createLoopSimplifyPass());
  Passes.add(createSCEVAAWrapperPass());
  Passes.add(createGlobalsAAWrapperPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createDIDependencyAnalysisPass());
  addPrint(AfterSroaAnalysis);
  addOutput();
  // Perform loop rotation to enable reduction recognition if for-loops.
  Passes.add(createLoopRotatePass());
  Passes.add(createCFGSimplificationPass());
  Passes.add(createInstructionCombiningPass());
  Passes.add(createLoopSimplifyPass());
  Passes.add(createLCSSAPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createDIDependencyAnalysisPass());
  addPrint(AfterLoopRotateAnalysis);
  addOutput();
  Passes.add(createVerifierPass());
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
  Passes.add(createPrintModulePass(*mOS, "", mCodeGenOpts->EmitLLVMUseLists));
  Passes.run(*M);
}

void InstrLLVMQueryManager::run(llvm::Module *M, TransformationContext *Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createDIGlobalRetrieverPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createDILoopRetrieverPass());
  Passes.add(createInstrumentationPass(mInstrEntry, mInstrStart));
  Passes.add(createPrintModulePass(*mOS, "", mCodeGenOpts->EmitLLVMUseLists));
  Passes.run(*M);
}

void TransformationQueryManager::run(llvm::Module *M,
    TransformationContext* Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (!Ctx)
    report_fatal_error("transformation context is not available");
  auto TEP = static_cast<TransformationEnginePass *>(
    createTransformationEnginePass());
  TEP->setContext(*M, Ctx);
  Passes.add(createImmutableASTImportInfoPass(mImportInfo));
  Passes.add(TEP);
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createInferFunctionAttrsLegacyPass());
  Passes.add(createPostOrderFunctionAttrsLegacyPass());
  Passes.add(createReversePostOrderFunctionAttrsPass());
  Passes.add(createRPOFunctionAttrsAnalysis());
  Passes.add(createPOFunctionAttrsAnalysis());
  if (!mTfmPass->getNormalCtor()) {
    M->getContext().emitError("cannot create pass " + mTfmPass->getPassName());
    return;
  }
  if (auto *GI = getPassRegistry().groupInfo(*mTfmPass))
    GI->addBeforePass(Passes);
  Passes.add(mTfmPass->getNormalCtor()());
  Passes.add(createClangFormatPass(mOutputSuffix, mNoFormat));
  Passes.add(createVerifierPass());
  Passes.run(*M);
}

void CheckQueryManager::run(llvm::Module *M, TransformationContext* Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (!Ctx)
    report_fatal_error("transformation context is not available");
  auto TEP = static_cast<TransformationEnginePass *>(
    createTransformationEnginePass());
  TEP->setContext(*M, Ctx);
  Passes.add(TEP);
  Passes.add(createUnreachableBlockEliminationPass());
  for (auto *PI : getPassRegistry()) {
    if (!PI->getNormalCtor()) {
      M->getContext().emitError("cannot create pass " + PI->getPassName());
      continue;
    }
    if (auto *GI = getPassRegistry().groupInfo(*PI))
      GI->addBeforePass(Passes);
    Passes.add(PI->getNormalCtor()());
  }
  Passes.add(createVerifierPass());
  Passes.run(*M);
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
