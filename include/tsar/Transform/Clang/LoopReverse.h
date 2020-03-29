#ifndef TSAR_CLANG_LOOP_REVERSE_H
#define TSAR_CLANG_LOOP_REVERSE_H

#include "tsar/Analysis/AnalysisSocket.h"
#include <llvm/Pass.h>

namespace llvm {
class ClangLoopReverse : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  ClangLoopReverse();
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace llvm

#endif
