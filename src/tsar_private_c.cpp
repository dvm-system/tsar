#include <clang/AST/Decl.h>
#include <llvm/Analysis/LoopInfo.h>
#include "tsar_dbg_output.h"
#include "tsar_private.h"
#include "tsar_private_c.h"
#include "tsar_transformation.h"

using namespace llvm;
using namespace tsar;
using namespace clang;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private-c"

char PrivateCClassifierPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateCClassifierPass, "private-c",
  "Private Variable Analysis (C sources)", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(PrivateCClassifierPass, "private-c",
  "Private Variable Analysis (C sources)", true, true)

 bool PrivateCClassifierPass::runOnFunction(Function &F) {
  auto &TEP = getAnalysis<TransformationEnginePass>();
  auto TC = TEP.getContext(*F.getParent());
  if (!TC || !TC->hasInstance())
    return false;
  return false;
}

void PrivateCClassifierPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<PrivateRecognitionPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createPrivateCClassifierPass() {
  return new PrivateCClassifierPass();
}