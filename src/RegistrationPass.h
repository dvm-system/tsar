#ifndef REGISTRATION_PASS_H
#define REGISTRATION_PASS_H

#include <llvm/Pass.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include "Registrator.h"
#include "tsar_pass.h"

//I hope I did it as it expected
//module pass to have only one Registrator class example in each module
class RegistrationPass :public llvm::ImmutablePass {
private:
  Registrator mRegistrator;
public:
  static char ID;
  RegistrationPass() : llvm::ImmutablePass(ID) {
    initializeMemoryMatcherPassPass(*llvm::PassRegistry::getPassRegistry());
  }    
  bool runOnModule(llvm::Module& M) override { 
    return 1;
  }
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  Registrator& getRegistrator() { return mRegistrator; }  
};

#endif //REGISTRATION_PASS_H


