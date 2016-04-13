#include <clang/AST/Decl.h>
#include <llvm/Analysis/LoopInfo.h>
#include "tsar_pass.h"
#include "tsar_private.h"
#include "tsar_private_c.h"
#include "tsar_rewriter_init.h"

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private-c"

char PrivateCClassifierPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateCClassifierPass, "private-c",
  "Private Variable Analysis (C sources)", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(RewriterInitializationPass)
INITIALIZE_PASS_END(PrivateCClassifierPass, "private-c",
  "Private Variable Analysis (C sources)", true, true)

 bool PrivateCClassifierPass::runOnFunction(Function &F) {
  return false;
}

void PrivateCClassifierPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<PrivateRecognitionPass>();
  AU.addRequired<RewriterInitializationPass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createPrivateCClassifierPass() {
  return new PrivateCClassifierPass();
}