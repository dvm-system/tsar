#undef DEBUG_TYPE
#define DEBUG_TYPE "registration"

#include "RegistrationPass.h"

using namespace llvm;

char RegistrationPass::ID = 0;

//put some information about pass(3rd parameter)
INITIALIZE_PASS_BEGIN(RegistrationPass, "registration", "some message", true, true)
INITIALIZE_PASS_END(RegistrationPass, "registration", "some message", true, true)

void RegistrationPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
}  
