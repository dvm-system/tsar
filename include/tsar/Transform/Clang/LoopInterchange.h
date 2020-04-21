#ifndef TSAR_CLANG_LOOP_INTERCHANGE_H
#define TSAR_CLANG_LOOP_INTERCHANGE_H

#include <bcl/utility.h>
#include <llvm/Pass.h>
namespace llvm {
class ClangLoopInterchange : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  ClangLoopInterchange();
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace llvm

#endif
