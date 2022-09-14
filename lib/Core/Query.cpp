//===------ Query.cpp --------- Query Manager ----------------*- C++ -*-===//
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
// This file implements default query managers.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/tsar-config.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/TraitFilter.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Analysis/Memory/AllocasModRef.h"
#ifdef APC_FOUND
# include "tsar/APC/Passes.h"
# include "tsar/APC/Utils.h"
#endif
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/OutputFile.h"
#include "tsar/Support/PassBarrier.h"
#include "tsar/Transform/AST/Passes.h"
#include "tsar/Transform/IR/Passes.h"
#include "tsar/Transform/Mixed/Passes.h"
#include "tsar/Support/Clang/Utils.h"
#include <clang/Frontend/CompilerInstance.h>
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
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Support/Host.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#ifdef lp_solve_FOUND
# include <lp_solve/lp_solve_config.h>
#endif

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace tsar {
void printToolVersion(raw_ostream &OS) {
  OS << "TSAR (" << TSAR_HOMEPAGE_URL << "):\n";
  OS << "  version " << TSAR_VERSION_STRING << "\n";
#ifdef APC_FOUND
  OS << "  with "; printAPCVersion(OS);
#endif
#ifdef lp_solve_FOUND
  OS << "  with lp_solve(" << LP_SOLVE_HOMEPAGE_URL << "):\n";
  OS << "    version " << LP_SOLVE_VERSION_STRING << "\n";
#endif
#ifndef __OPTIMIZE__
  OS << "  DEBUG build";
#else
  OS << "  Optimized build";
#endif
#ifndef NDEBUG
  OS << " with assertions";
#endif
  OS << ".\n";
  OS << "  Built " << __DATE__ << " (" << __TIME__ << ").\n";
  auto CPU = sys::getHostCPUName();
  OS << "  Host CPU: " << ((CPU != "generic") ? CPU : "(unknown)") << "\n";
}


void addImmutableAliasAnalysis(legacy::PassManager &Passes) {
  Passes.add(createCFLSteensAAWrapperPass());
  Passes.add(createCFLAndersAAWrapperPass());
  Passes.add(createTypeBasedAAWrapperPass());
  Passes.add(createScopedNoAliasAAWrapperPass());
  Passes.add(createAllocasAAWrapperPass());
  Passes.add(
      createExternalAAWrapperPass([](Pass &P, Function &F, AAResults &AAR) {
        if (auto AAP = P.getAnalysisIfAvailable<AllocasAAWrapperPass>()) {
          AAP->getResult().analyzeFunction(F);
          AAR.addAAResult(AAP->getResult());
        }
      }));
}

void addInitialTransformations(legacy::PassManager &Passes) {
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
  Passes.add(createFlangDIVariableRetrieverPass());
  Passes.add(createNoMetadataDSEPass());
  Passes.add(createDILoopRetrieverPass());
  Passes.add(createDINodeRetrieverPass());
  Passes.add(createFlangDummyAliasAnalysis());
}

void addBeforeTfmAnalysis(legacy::PassManager &Passes, StringRef AnalysisUse) {
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createCallExtractorPass());
  Passes.add(createGlobalDefinedMemoryPass());
  Passes.add(createGlobalLiveMemoryPass());
  Passes.add(createFunctionMemoryAttrsAnalysis());
  Passes.add(createPassBarrier());
  Passes.add(createDIDependencyAnalysisPass(true));
  Passes.add(createProcessDIMemoryTraitPass(mark<trait::DirectAccess>));
  Passes.add(createAnalysisReader());
}

void addAfterSROAAnalysis(const GlobalOptions &GO, const DataLayout &DL,
                          legacy::PassManager &Passes) {
  // Some function passes (for example DefinedMemoryPass) may use results of
  // module passes and these results becomes invalid after transformations.
  // So, to prevent access to invalid (destroyed) values we finish processing of
  // all functions.
  Passes.add(createPassBarrier());
  Passes.add(createCFGSimplificationPass());
  // Do not add 'instcombine' here, because in this case some metadata may be
  // lost after SROA (for example, if a promoted variable is a structure).
  // Passes.add(createInstructionCombiningPass());
  Passes.add(createSROAPass());
  Passes.add(createProcessDIMemoryTraitPass(
    [&DL](DIMemoryTrait &T) { markIfNotPromoted(DL, T); }));
  if (!GO.UnsafeTfmAnalysis)
    Passes.add(createProcessDIMemoryTraitPass(
      markIf<trait::Lock, trait::NoPromotedScalar>));
  Passes.add(createEarlyCSEPass());
  Passes.add(createCFGSimplificationPass());
  Passes.add(createInstructionCombiningPass());
  Passes.add(createLoopSimplifyPass());
  Passes.add(createSCEVAAWrapperPass());
  Passes.add(createGlobalsAAWrapperPass());
  Passes.add(createRPOFunctionAttrsAnalysis());
  Passes.add(createPOFunctionAttrsAnalysis());
  Passes.add(createPointerScalarizerPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createCallExtractorPass());
  Passes.add(createGlobalDefinedMemoryPass());
  Passes.add(createGlobalLiveMemoryPass());
  Passes.add(createFunctionMemoryAttrsAnalysis());
  Passes.add(createPassBarrier());
  Passes.add(createDIDependencyAnalysisPass());
}

void addAfterFunctionInlineAnalysis(
    const GlobalOptions &GO, const DataLayout &DL,
    const std::function<void(tsar::DIMemoryTrait &)> &Unlock,
    legacy::PassManager &Passes) {
  if (GO.NoInline)
    return;
  Passes.add(createPassBarrier());
  Passes.add(createDependenceInlinerAttributer());
  Passes.add(createProcessDIMemoryTraitPass(Unlock));
  Passes.add(createDependenceInlinerPass());
  addAfterSROAAnalysis(GO, DL, Passes);
}

void addAfterLoopRotateAnalysis(legacy::PassManager &Passes) {
  Passes.add(createPassBarrier());
  Passes.add(
      createProcessDIMemoryTraitPass(markIf<trait::Lock, trait::HeaderAccess>));
  Passes.add(createLoopRotatePass());
  Passes.add(createCFGSimplificationPass());
  Passes.add(createInstructionCombiningPass());
  Passes.add(createLoopSimplifyPass());
  Passes.add(createLCSSAPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createCallExtractorPass());
  Passes.add(createGlobalDefinedMemoryPass());
  Passes.add(createGlobalLiveMemoryPass());
  Passes.add(createFunctionMemoryAttrsAnalysis());
  Passes.add(createPassBarrier());
  Passes.add(createDIDependencyAnalysisPass());
}
} // namespace tsar

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

void DefaultQueryManager::run(llvm::Module *M, TransformationInfo *TfmInfo) {
  assert(M && "Module must not be null!");
  auto &PrintOS{errs()};
  if (!mPrintPasses.empty() && mGlobalOptions->PrintToolVersion)
    printToolVersion(PrintOS);
  legacy::PassManager Passes;
  Passes.add(createGlobalOptionsImmutableWrapper(mGlobalOptions));
  if (TfmInfo) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->set(*TfmInfo);
    Passes.add(TEP);
    Passes.add(createImmutableASTImportInfoPass(mImportInfo));
  }
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  auto addPrint = [&Passes, this, &PrintOS](ProcessingStep CurrentStep) {
    if (!(CurrentStep & mPrintSteps))
      return;
    for (auto PI : mPrintPasses) {
      if (!PI->getNormalCtor()) {
        errs() << "warning: cannot create pass: " << PI->getPassName() << "\n";
        continue;
      }
      if (auto *GI = PrintPassGroup::getPassRegistry().groupInfo(*PI))
        GI->addBeforePass(Passes);
      auto *P = PI->getNormalCtor()();
      auto Kind = P->getPassKind();
      Passes.add(P);
      switch (Kind) {
      default:
        llvm_unreachable("Printers does not support this kind of passes yet!");
        break;
      case PT_Function:
        Passes.add(createFunctionPassPrinter(PI, PrintOS));
        break;
      case PT_Module:
        Passes.add(createModulePassPrinter(PI, PrintOS));
        break;
      }
    }
  };
  auto addOutput = [&Passes, this](ProcessingStep CurrentStep) {
    if (!(CurrentStep & mPrintSteps))
      return;
    for (auto PI : mOutputPasses) {
      if (!PI->getNormalCtor()) {
        errs() << "warning: cannot create pass: " << PI->getPassName() << "\n";
        continue;
      }
      if (auto *GI = OutputPassGroup::getPassRegistry().groupInfo(*PI))
        GI->addBeforePass(Passes);
      Passes.add(PI->getNormalCtor()());
    }
  };
  // Add pass to a manager if it is necessary for some of pases in a list.
  // Properties of this passes will be looked up in a specified group of passes.
  auto addIfNecessary =
    [](Pass *P, ArrayRef<const PassInfo *> PL, PassGroupRegistry &GR,
      legacy::PassManager &Passes) {
    for (auto *PI : PL) {
      if (auto *GI = GR.groupInfo(*PI))
        if (GI->isNecessaryPass(P->getPassID())) {
          Passes.add(P);
          return;
        }
    }
    delete P;
  };
  Passes.add(createGlobalsAccessStorage());
  if (mUseServer) {
    Passes.add(createGlobalsAccessCollector());
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(createDIMemoryAnalysisServer());
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createMemoryMatcherPass());
    Passes.add(createAnalysisWaitServerPass());
    addPrint(BeforeTfmAnalysis);
    addOutput(BeforeTfmAnalysis);
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
    Passes.add(createVerifierPass());
    Passes.run(*M);
    return;
  }
  Passes.add(createMemoryMatcherPass());
  Passes.add(createGlobalDefinedMemoryStorage());
  Passes.add(createGlobalLiveMemoryStorage());
  // It is necessary to destroy DIMemoryTraitPool before DIMemoryEnvironment to
  // avoid dangling handles. So, we add pool before environment in the manager.
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  addBeforeTfmAnalysis(Passes);
  addPrint(BeforeTfmAnalysis);
  addOutput(BeforeTfmAnalysis);
  addAfterSROAAnalysis(*mGlobalOptions, M->getDataLayout(), Passes);
  addPrint(AfterSroaAnalysis);
  addOutput(AfterSroaAnalysis);
  addAfterFunctionInlineAnalysis(
      *mGlobalOptions, M->getDataLayout(),
      [](auto &T) {
        if (T.getMemory()->emptyBinding() || T.getMemory()->isOriginal())
          return;
        unmarkIf<trait::Lock, trait::NoPromotedScalar>(T);
        unmark<trait::NoPromotedScalar>(T);
      },
      Passes);
  addPrint(AfterFunctionInlineAnalysis);
  addOutput(AfterFunctionInlineAnalysis);
  addAfterLoopRotateAnalysis(Passes);
  addPrint(AfterLoopRotateAnalysis);
  addOutput(AfterLoopRotateAnalysis);
  Passes.add(createVerifierPass());
  Passes.run(*M);
}

bool EmitLLVMQueryManager::beginSourceFile(clang::DiagnosticsEngine &Diags,
                                           StringRef InputFile,
                                           StringRef OutputFile,
                                           StringRef WorkingDir) {
  mDiags = &Diags;
  mWorkingDir = WorkingDir.str();
  mOutputFile = std::move(createDefaultOutputFile(Diags, OutputFile, false,
                                                  InputFile, "ll", true, true));
  return mOutputFile.hasValue();
}

void EmitLLVMQueryManager::endSourceFile(bool HasErrorOccurred) {
  auto E{mOutputFile->clear(mWorkingDir, HasErrorOccurred)};
  if (E && !HasErrorOccurred && mOutputFile->useTemporary())
    mDiags->Report(diag::err_unable_to_rename_temp)
        << mOutputFile->getTemporary().TmpName << mOutputFile->getFilename()
        << std::move(E);
}

void EmitLLVMQueryManager::run(llvm::Module *M, TransformationInfo *) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  Passes.add(createPrintModulePass(mOutputFile->getStream(), ""));
  Passes.run(*M);
}

void InstrLLVMQueryManager::run(llvm::Module *M,
    TransformationInfo *TfmInfo) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  Passes.add(createGlobalOptionsImmutableWrapper(mGlobalOptions));
  if (TfmInfo) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->set(*TfmInfo);
    Passes.add(TEP);
  }
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createNoMetadataDSEPass());
  Passes.add(createFlangDIVariableRetrieverPass());
  Passes.add(createDINodeRetrieverPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createDILoopRetrieverPass());
  Passes.add(createGlobalsAccessStorage());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createInstrumentationPass(mInstrEntry, mInstrStart));
  Passes.add(createPrintModulePass(mOutputFile->getStream(), ""));
  Passes.run(*M);
}

void TransformationQueryManager::run(llvm::Module *M,
    TransformationInfo *TfmInfo) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  Passes.add(createGlobalOptionsImmutableWrapper(mGlobalOptions));
  if (!TfmInfo)
    report_fatal_error("transformation context is not available");
  auto TEP = static_cast<TransformationEnginePass *>(
    createTransformationEnginePass());
  TEP->set(*TfmInfo);
  Passes.add(TEP);
  Passes.add(createImmutableASTImportInfoPass(mImportInfo));
  addInitialTransformations(Passes);
  if (!mTfmPass->getNormalCtor()) {
    M->getContext().emitError("cannot create pass " + mTfmPass->getPassName());
    return;
  }
  if (auto *GI = getPassRegistry().groupInfo(*mTfmPass))
    GI->addBeforePass(Passes);
  Passes.add(mTfmPass->getNormalCtor()());
  if (auto *GI = getPassRegistry().groupInfo(*mTfmPass))
    GI->addAfterPass(Passes);
  Passes.add(createASTFormatPass());
  Passes.add(createVerifierPass());
  Passes.run(*M);
}

void CheckQueryManager::run(llvm::Module *M, TransformationInfo *TfmInfo) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (!TfmInfo)
    report_fatal_error("transformation context is not available");
  auto TEP = static_cast<TransformationEnginePass *>(
    createTransformationEnginePass());
  TEP->set(*TfmInfo);
  Passes.add(TEP);
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createNoMetadataDSEPass());
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
