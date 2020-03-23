#ifndef TSAR_CLANG_RENAME_LOCAL_H
#define TSAR_CLANG_RENAME_LOCAL_H

#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class ClangLoopReversePass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangLoopReversePass() : ModulePass(ID) {
    initializeClangLoopReversePassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace llvm
#endif