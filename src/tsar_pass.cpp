//===------ tsar_pass.cpp - Initialize TSAR Passes --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This contains functions to initialize passes that are implemented in TSAR.
//
//===----------------------------------------------------------------------===//

#include <llvm/InitializePasses.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include "tsar_pass.h"

using namespace llvm;

void llvm::initializeTSAR(PassRegistry &Registry) {
  initializePrivateRecognitionPassPass(Registry);
  initializeTransformationEnginePassPass(Registry);
  initializePrivateCClassifierPassPass(Registry);
  initializeInstrumentationPassPass(Registry);
}

void initializeTSAR(LLVMPassRegistryRef R) {
  initializeTSAR(*unwrap(R));
}
