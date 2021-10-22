//=== CountedRegionsExtractor.h - Extracting Of CountedRegions From Code Coverage (Clang) --*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===---------------------------------------------------------------------===//
//
// This file declares a pass to extract & store 
// all the CountedRegions along with their ExecutionCounts
// from a given code coverage file.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_COUNTED_REGIONS_EXTRACTOR_H
#define TSAR_CLANG_COUNTED_REGIONS_EXTRACTOR_H

#include <bcl/utility.h>
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/Pass.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <vector>

namespace llvm {
/// This immutable pass extracts & stores CountedRegions
/// from a given code coverage file
class ClangCountedRegionsExtractor : public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangCountedRegionsExtractor();

  void initializePass() override;
  const std::vector<llvm::coverage::CountedRegion> &getCountedRegions() const noexcept;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::vector<llvm::coverage::CountedRegion> CountedRegions{};
};
}
#endif//TSAR_CLANG_COUNTED_REGIONS_EXTRACTOR_H
