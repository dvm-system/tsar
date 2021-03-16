//===---------------------------------------------------------------------===//
//
// This file defines a pass to eliminate unreachable calls in a source code.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_UNREACHABLE_CALLS_H
#define TSAR_CLANG_UNREACHABLE_CALLS__H


#include <bcl/utility.h>
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/Pass.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <vector>


namespace llvm {
/// This per-function pass performs source-level elimination of
/// unreachable calls.
class ClangUnreachableCallsElimination : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangUnreachableCallsElimination();

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::vector<llvm::coverage::CountedRegion> Unreachable{};
};
}
#endif//TSAR_CLANG_UNREACHABLE_CALLS__H
