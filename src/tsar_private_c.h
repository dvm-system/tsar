#ifndef TSAR_PRIVATE_C_H
#define TSAR_PRIVATE_C_H

#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_pass.h"

namespace llvm {
class PrivateCClassifierPass :
  public FunctionPass, private Utility::Uncopyable {
  public:
    /// Pass identification, replacement for typeid.
    static char ID;

    /// Default constructor.
    PrivateCClassifierPass() : FunctionPass(ID) {
      initializePrivateCClassifierPassPass(*PassRegistry::getPassRegistry());
    }

    bool runOnFunction(Function &F) override;

    /// Specifies a list of analyzes  that are necessary for this pass.
    void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_PRIVATE_C_H