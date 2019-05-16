//===------ tsar_pass.cpp - Initialize TSAR Passes --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This contains functions to initialize passes that are implemented in TSAR.
//
//===----------------------------------------------------------------------===//

#include "tsar_pass.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/LegacyPassManager.h>

using namespace llvm;

void llvm::initializeTSAR(PassRegistry &Registry) {
  initializeGlobalOptionsImmutableWrapperPass(Registry);
  initializeDFRegionInfoPassPass(Registry);
  initializeDefinedMemoryPassPass(Registry);
  initializeLiveMemoryPassPass(Registry);
  initializeEstimateMemoryPassPass(Registry);
  initializeDIEstimateMemoryPassPass(Registry);
  initializeAliasTreeViewerPass(Registry);
  initializeAliasTreeOnlyViewerPass(Registry);
  initializeDIAliasTreeViewerPass(Registry);
  initializeAliasTreePrinterPass(Registry);
  initializeAliasTreeOnlyPrinterPass(Registry);
  initializeDIAliasTreePrinterPass(Registry);
  initializePrivateRecognitionPassPass(Registry);
  initializeDIDependencyAnalysisPassPass(Registry);
  initializeTransformationEnginePassPass(Registry);
  initializeInstrumentationPassPass(Registry);
  initializeLoopMatcherPassPass(Registry);
  initializeClangExprMatcherPassPass(Registry);
  initializeClangCFTraitsPassPass(Registry);
  initializeTestPrinterPassPass(Registry);
  initializeLoopAttributesDeductionPassPass(Registry);
  initializeDelinearizationPassPass(Registry);
  initializeAnalysisReader(Registry);
  // Initialize LLVM-level transformation passes.
  initializeDILoopRetrieverPassPass(Registry);
  initializeDIGlobalRetrieverPassPass(Registry);
  initializePOFunctionAttrsAnalysisPass(Registry);
  initializeRPOFunctionAttrsAnalysisPass(Registry);
  // Initialize source-level analysis passes.
  initializeClangPerfectLoopPassPass(Registry);
  initializeClangGlobalInfoPassPass(Registry);
  initializeClangAnalysis(Registry);
  // Initialize source-level transform passes.
  initializeClangInlinerPassPass(Registry);
  initializeClangFormatPassPass(Registry);
  initializeCopyEliminationPassPass(Registry);
  initializeClangRenameLocalPassPass(Registry);
  initializeClangDeadDeclsEliminationPass(Registry);
  initializeClangTransform(Registry);
  // Initialize checkers.
  initializeClangNoMacroAssertPass(Registry);
  // Initialize necessary LLVM passes.
  initializeUnreachableBlockElimLegacyPassPass(Registry);
  initializeCanonicalLoopPassPass(Registry);
  initializePromoteLegacyPassPass(Registry);
  initializeLoopRotateLegacyPassPass(Registry);
  initializeEarlyCSELegacyPassPass(Registry);
  initializeLoopSimplifyCFGLegacyPassPass(Registry);
  initializeSROALegacyPassPass(Registry);
  initializeInstCombine(Registry);
}

void initializeTSAR(LLVMPassRegistryRef R) {
  initializeTSAR(*unwrap(R));
}
