//===-- ASTDependenceAnalysis.h - Dependence Analyzer -- (Clang) -*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
//===----------------------------------------------------------------------===//
//
// This file implements classes pass to perform source-level dependence
// analysis.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_DEPENDENCE_ANALYZER_H
#define TSAR_CLANG_DEPENDENCE_ANALYZER_H

#include "tsar/Analysis/Clang/VariableCollector.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Support/GlobalOptions.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <array>
#include <set>

namespace tsar {
class ClangDependenceAnalyzer {
public:
  using ClangDIMemoryMatcher = llvm::ClangDIMemoryMatcherPass::DIMemoryMatcher;

  /// Sorted list of variables (to print their in algoristic order).
  using SortedVarListT = std::set<std::string, std::less<std::string>>;

  /// Lists of reduction variables.
  using ReductionVarListT =
      std::array<SortedVarListT, trait::DIReduction::RK_NumberOf>;

  /// List of traits.
  using ASTRegionTraitInfo =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<SortedVarListT, trait::FirstPrivate>,
                        bcl::tagged<SortedVarListT, trait::LastPrivate>,
                        bcl::tagged<SortedVarListT, trait::ReadOccurred>,
                        bcl::tagged<SortedVarListT, trait::WriteOccurred>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>>;

  ClangDependenceAnalyzer(clang::Stmt *Region, const GlobalOptions &GO,
      clang::DiagnosticsEngine &Diags, DIAliasTree &DIAT,
      DIDependenceSet &DIDepSet, ClonedDIMemoryMatcher &DIMemoryMatcher,
      const ClangDIMemoryMatcher &ASTToClient)
    : mRegion(Region), mGlobalOpts(GO), mDiags(Diags), mDIAT(DIAT),
      mDIDepSet(DIDepSet), mDIMemoryMatcher(DIMemoryMatcher),
      mASTToClient(ASTToClient) {
    assert(Region && "Source-level region must not be null!");
  }

  bool evaluateDependency();
  bool evaluateDefUse();

  const ASTRegionTraitInfo & getDependenceInfo() const noexcept {
    return mDependenceInfo;
  }

private:
  clang::Stmt *mRegion;
  const GlobalOptions &mGlobalOpts;
  clang::DiagnosticsEngine &mDiags;
  DIAliasTree &mDIAT;
  DIDependenceSet &mDIDepSet;
  ClonedDIMemoryMatcher &mDIMemoryMatcher;
  const ClangDIMemoryMatcher &mASTToClient;
  ASTRegionTraitInfo mDependenceInfo;

  VariableCollector mASTVars;
  llvm::SmallVector<DIAliasTrait *, 32> mInToLocalize;
  llvm::SmallVector<DIAliasTrait *, 32> mOutToLocalize;
};
}
#endif//TSAR_CLANG_DEPENDENCE_ANALYZER_H
