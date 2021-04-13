//===---------------------------------------------------------------------===//
//
// This file defines a pass to obtain
// unreachable CountedRegions from the given file.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_UNREACHABLE_COUNTED_REGIONS_H
#define TSAR_CLANG_UNREACHABLE_COUNTED_REGIONS_H


#include <bcl/utility.h>
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/Pass.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <vector>


namespace llvm {
/// This immutable pass obtains unreachable CountedRegions
/// from the given file
class ClangUnreachableCountedRegions : public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangUnreachableCountedRegions();

  void initializePass() override;
  const std::vector<llvm::coverage::CountedRegion> &getUnreachable() const noexcept;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::vector<llvm::coverage::CountedRegion> Unreachable{};
};
}
#endif//TSAR_CLANG_UNREACHABLE_COUNTED_REGIONS_H
