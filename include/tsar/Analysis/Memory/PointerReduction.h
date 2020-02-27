#include "Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

#ifndef TSAR_POINTER_REDUCTION_H
#define TSAR_POINTER_REDUCTION_H

namespace llvm {
class PointerReductionPass : public FunctionPass, private bcl::Uncopyable {
public :
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  PointerReductionPass() : FunctionPass(ID) {
    initializePointerReductionPassPass(*PassRegistry::getPassRegistry());
  }

  /// Executes reach definition analysis for a specified function.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases memory.
  void releaseMemory() override { }

};

FunctionPass * createPointerReductionPass() {
    return new PointerReductionPass();
}
}
#endif//TSAR_POINTER_REDUCTION_H