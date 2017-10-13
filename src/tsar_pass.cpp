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
#include <llvm/Config/llvm-config.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/LegacyPassManager.h>

using namespace llvm;

void llvm::initializeTSAR(PassRegistry &Registry) {
  initializeDFRegionInfoPassPass(Registry);
  initializeDefinedMemoryPassPass(Registry);
  initializeLiveMemoryPassPass(Registry);
  initializeEstimateMemoryPassPass(Registry);
  initializeAliasTreeViewerPass(Registry);
  initializeAliasTreeOnlyViewerPass(Registry);
  initializeAliasTreePrinterPass(Registry);
  initializeAliasTreeOnlyPrinterPass(Registry);
  initializePrivateRecognitionPassPass(Registry);
  initializeTransformationEnginePassPass(Registry);
  initializePrivateCClassifierPassPass(Registry);
  initializeInstrumentationPassPass(Registry);
  initializeLoopMatcherPassPass(Registry);
  initializeTestPrinterPassPass(Registry);
  initializeClangPerfectLoopPassPass(Registry);
  // Initialize necessary LLVM passes.
  initializeUnreachableBlockElimLegacyPassPass(Registry);
  initializeCanonicalLoopPassPass(Registry);
}

void initializeTSAR(LLVMPassRegistryRef R) {
  initializeTSAR(*unwrap(R));
}
