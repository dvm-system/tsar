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
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 7)
#include <llvm/PassManager.h>
#else
#include <llvm/IR/LegacyPassManager.h>
#endif

using namespace llvm;

void llvm::initializeTSAR(PassRegistry &Registry) {
  initializePrivateRecognitionPassPass(Registry);
}


void initializeTSAR(LLVMPassRegistryRef R) {
  initializeTSAR(*unwrap(R));
}
