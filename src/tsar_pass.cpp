#include <llvm/InitializePasses.h>
#include "llvm/PassManager.h"

using namespace llvm;

void llvm::initializeTSAR(PassRegistry &Registry) {
    initializePrivateRecognitionPassPass(Registry);
}


void initializeTSAR(LLVMPassRegistryRef R) {
    initializeTSAR(*unwrap(R));
}
