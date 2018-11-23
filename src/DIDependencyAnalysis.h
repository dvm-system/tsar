//===- DIDependencyAnalysis.h - Dependency Analyzer (Metadata) --*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// This file defines passes which uses source-level debug information
// to summarize low-level results of privatization, reduction and induction
// recognition and flow/anti/output dependencies exploration.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_DEPENDENCY_ANALYSIS_H
#define TSAR_DI_DEPENDENCY_ANALYSIS_H

#include "DIMemoryTrait.h"
#include "tsar_pass.h"
#include "tsar_utility.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>
#include <memory>

namespace llvm {
class DominatorTree;
class ScalarEvolution;
class MDNode;
}

namespace tsar {
/// Source-level representation of data-dependencies (including anti/flow/output
/// dependencies, privatizable variables, reductions and inductions).
using DIDependencyInfo =
  llvm::DenseMap<llvm::MDNode *, DIDependenceSet,
    llvm::DenseMapInfo<llvm::MDNode *>,
    TaggedDenseMapPair<
      bcl::tagged<llvm::MDNode *, llvm::MDNode>,
      bcl::tagged<DIDependenceSet, DIDependenceSet>>>;
}

namespace llvm {
/// This uses source-level debug information to summarize low-level results of
/// privatization, reduction and induction recognition and flow/anti/output
/// dependencies exploration.
class DIDependencyAnalysisPass :
  public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DIDependencyAnalysisPass() : FunctionPass(ID) {
    initializeDIDependencyAnalysisPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about discovered dependencies.
  tsar::DIDependencyInfo & getDependencies() noexcept {return mDeps;}

  /// Returns information about discovered dependencies.
  const tsar::DIDependencyInfo &getDependencies() const noexcept {return mDeps;}

  // Summarizes low-level results of privatization, reduction and induction
  // recognition and flow/anti/output dependencies exploration.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases allocated memory.
  void releaseMemory() override { mDeps.clear(); }

  /// Prints out the internal state of the pass. This also used to produce
  /// analysis correctness tests.
  void print(raw_ostream &OS, const Module *M) const override;

private:
  /// Performs analysis of promoted memory locations in a specified loop and
  /// updates description of metadata-level traits in a specified pool if
  /// necessary.
  void analyzePromoted(Loop *L, Optional<unsigned> DWLang,
    tsar::DIMemoryTraitRegionPool &Pool);

  tsar::DIDependencyInfo mDeps;
  DominatorTree *mDT;
  ScalarEvolution *mSE;
};
}
#endif//TSAR_DI_DEPENDENCY_ANALYSIS_H
