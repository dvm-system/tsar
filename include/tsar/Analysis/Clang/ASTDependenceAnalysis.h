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
#include "tsar/Support/Diagnostic.h"
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

  using VariableT = VariableCollector::VariableT;
  using VariableLess = VariableCollector::VariableLess;

  using SortedVarListT = VariableCollector::SortedVarListT;
  using SortedVarMultiListT = VariableCollector::SortedVarMultiListT;

private:
  using DiagnosticId = decltype(tsar::diag::PADDING_BUILTIN_TSAR_DIAGNOSTIC);
  struct DiagnosticT {
    DiagnosticT(DiagnosticId Id) : Id(Id) {}
    SortedVarMultiListT Source;
    DiagnosticId Id;
    bool IsPresent = false;
  };
  using DiagnosticListT = llvm::SmallVector<DiagnosticT, 8>;
public:

  /// List of dependence distances.
  using DistanceInfo = bcl::tagged_tuple<
      bcl::tagged<trait::DIDependence::DistanceVector, trait::Flow>,
      bcl::tagged<trait::DIDependence::DistanceVector, trait::Anti>>;

  /// Sorted list of variables with dependence distances (flow and anti)
  /// attached to them.
  using SortedVarDistanceT =
      std::map<VariableT, DistanceInfo, VariableLess>;

  /// Lists of reduction variables.
  using ReductionVarListT =
      std::array<SortedVarListT, trait::DIReduction::RK_NumberOf>;

  /// Description of an induction variable and its bounds.
  using InductionInfo =
      bcl::tagged_tuple<bcl::tagged<trait::DIInduction::Constant, Begin>,
                        bcl::tagged<trait::DIInduction::Constant, End>,
                        bcl::tagged<trait::DIInduction::Constant, Step>>;

  /// List of induction variables.
  using InductionVarListT = std::map<VariableT, InductionInfo, VariableLess>;

  /// List of traits.
  using ASTRegionTraitInfo =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Local>,
                        bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<SortedVarListT, trait::FirstPrivate>,
                        bcl::tagged<SortedVarListT, trait::LastPrivate>,
                        bcl::tagged<SortedVarListT, trait::ReadOccurred>,
                        bcl::tagged<SortedVarListT, trait::WriteOccurred>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>,
                        bcl::tagged<SortedVarDistanceT, trait::Dependence>,
                        bcl::tagged<InductionVarListT, trait::Induction>>;

  ClangDependenceAnalyzer(clang::Stmt *Region, const GlobalOptions &GO,
      clang::DiagnosticsEngine &Diags, DIAliasTree &DIAT,
      DIDependenceSet &DIDepSet, ClonedDIMemoryMatcher &DIMemoryMatcher,
      const ClangDIMemoryMatcher &ASTToClient)
    : mRegion(Region), mGlobalOpts(GO), mDiags(Diags), mDIAT(DIAT),
      mDIDepSet(DIDepSet), mDIMemoryMatcher(DIMemoryMatcher),
      mASTToClient(ASTToClient) {
    assert(Region && "Source-level region must not be null!");
  }

  bool evaluateDependency(bool EmitDiags = true);
  bool evaluateDefUse(bool EmitDiags = true);

  const ASTRegionTraitInfo & getDependenceInfo() const noexcept {
    return mDependenceInfo;
  }

  const SortedVarMultiListT * hasDiagnostic(DiagnosticId Id) const {
    auto I{llvm::find_if(mDiagList,
                         [Id](auto &Diag) { return Diag.Id == Id; })};
    if (I == mDiagList.end() || !I->IsPresent)
      return nullptr;
    return &I->Source;
  }

  clang::DiagnosticsEngine &getDiagnostics() const noexcept {
    return mDiags;
  }

  clang::Stmt *getRegion() noexcept { return mRegion; }
  const clang::Stmt *getRegion() const noexcept { return mRegion; }

private:
  unsigned findOrCreateDiagnosticInfo(DiagnosticId Id) {
    auto I{llvm::find_if(mDiagList,
                         [Id](auto &Diag) { return Diag.Id == Id; })};
    if (I != mDiagList.end())
      return std::distance(mDiagList.begin(), I);
    mDiagList.emplace_back(Id);
    return mDiagList.size() - 1;
  }
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
  DiagnosticListT mDiagList;
};
}
#endif//TSAR_CLANG_DEPENDENCE_ANALYZER_H
