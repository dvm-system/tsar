#ifndef TSAR_CLANG_LOOP_SWAP_H
#define TSAR_CLANG_LOOP_SWAP_H

#include <bcl/utility.h>
#include <llvm/Pass.h>
namespace llvm {
class ClangLoopSwap : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  ClangLoopSwap();
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace llvm

#endif
