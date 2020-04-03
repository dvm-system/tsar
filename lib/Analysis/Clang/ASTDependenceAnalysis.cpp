//===- ASTDependenceAnalysis.cpp - Dependence Analyzer - (Clang) -*- C++ -*===//
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

#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include <llvm/ADT/DenseSet.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

bool ClangDependenceAnalyzer::evaluateDependency() {
  DenseSet<const DIAliasNode *> Coverage, RedundantCoverage;
  DenseSet<const DIAliasNode *> *RedundantCoverageRef = &Coverage;
  accessCoverage<bcl::SimpleInserter>(mDIDepSet, mDIAT, Coverage,
                                      mGlobalOpts.IgnoreRedundantMemory);
  if (mGlobalOpts.IgnoreRedundantMemory) {
    accessCoverage<bcl::SimpleInserter>(mDIDepSet, mDIAT,
      RedundantCoverage, false);
    RedundantCoverageRef = &RedundantCoverage;
  }
  mASTVars.TraverseStmt(mRegion);
  DenseSet<DIAliasNode *> DirectSideEffect;
  for (auto &TS : mDIDepSet) {
    if (TS.is<trait::DirectAccess>())
      for (auto &T : TS) {
        if (!T->is<trait::DirectAccess>())
          continue;
        if (auto *DIUM = dyn_cast<DIUnknownMemory>(T->getMemory()))
          if (DIUM->isExec())
            DirectSideEffect.insert(
                const_cast<DIAliasMemoryNode *>(DIUM->getAliasNode()));
      }
    MemoryDescriptor Dptr = TS;
    // This should be set to true if current node is redundant and could be
    // ignored.
    bool IgnoreRedundant = false;
    if (!Coverage.count(TS.getNode())) {
      if (mGlobalOpts.IgnoreRedundantMemory && TS.size() == 1 &&
        RedundantCoverageRef->count(TS.getNode())) {
        IgnoreRedundant = true;
        // If -fignore-redundant-memory option is set then traits for redundant
        // alias node is set to 'no access'. However, we want to consider traits
        // of redundant memory if these traits do not prevent parallelization.
        // So, if redundant node contains only single redundant memory location
        // we use traits of this location as traits of the whole node.
        Dptr = **TS.begin();
      } else {
        continue;
      }
    }
    if (Dptr.is_any<trait::Shared, trait::Readonly>()) {
      /// Remember memory locations for variables for further analysis.
      for (auto &T : TS)
        auto S =
            mASTVars.findDecl(*T->getMemory(), mASTToClient, mDIMemoryMatcher);
      if (Dptr.is_any<trait::Readonly>())
        mInToLocalize.push_back(&TS);
      else
        mOutToLocalize.push_back(&TS);
    } else if (Dptr.is<trait::Induction>()) {
      if (TS.size() > 1) {
        if (!IgnoreRedundant) {
          toDiag(mDiags, mRegion->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(mDiags, mRegion->getLocStart(),
                 clang::diag::note_parallel_multiple_induction);
          return false;
        }
      } else {
        auto Search = mASTVars.findDecl(*(*TS.begin())->getMemory(),
          mASTToClient, mDIMemoryMatcher);
        if (Search.second != VariableCollector::CoincideLocal ||
            Search.first != mASTVars.Induction) {
          if (!IgnoreRedundant) {
            toDiag(mDiags, mRegion->getLocStart(),
                   clang::diag::warn_parallel_loop);
            if (mASTVars.Induction && Search.first &&
                mASTVars.Induction != Search.first) {
              toDiag(mDiags, mASTVars.Induction->getLocation(),
                     clang::diag::note_parallel_multiple_induction);
              toDiag(mDiags, Search.first->getLocation(),
                     clang::diag::note_parallel_multiple_induction);
            } else {
              toDiag(mDiags, mRegion->getLocStart(),
                     clang::diag::note_parallel_multiple_induction);
            }
            return false;
          }
        }
        mInToLocalize.push_back(&TS);
        mOutToLocalize.push_back(&TS);
      }
    } else if (Dptr.is<trait::Private>()) {
      clang::VarDecl *Status = nullptr;
      if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
                             mDependenceInfo.get<trait::Private>(), &Status) &&
          !IgnoreRedundant) {
        toDiag(mDiags, mRegion->getLocStart(), clang::diag::warn_parallel_loop);
        toDiag(mDiags, Status ? Status->getLocation() : mRegion->getLocStart(),
          clang::diag::note_parallel_localize_private_unable);
        return false;
      }
    } else if (Dptr.is<trait::Reduction>()) {
      auto I = TS.begin(), EI = TS.end();
      auto *Red = (**I).get<trait::Reduction>();
      if (!Red || Red->getKind() == trait::DIReduction::RK_NoReduction) {
        if (!IgnoreRedundant) {
          toDiag(mDiags, mRegion->getLocStart(),
                 clang::diag::warn_parallel_loop);
          auto Search = mASTVars.findDecl(*(*I)->getMemory(), mASTToClient,
                                         mDIMemoryMatcher);
          toDiag(mDiags,
                 Search.first ? Search.first->getLocation()
                              : mRegion->getLocStart(),
                 clang::diag::note_parallel_reduction_unknown);
          return false;
        }
      } else {
        auto CurrentKind = Red->getKind();
        auto &ReductionList =
            mDependenceInfo.get<trait::Reduction>()[CurrentKind];
        clang::VarDecl *Status = nullptr;
        if (!mASTVars.localize(**I, *TS.getNode(), mASTToClient,
              mDIMemoryMatcher, ReductionList, &Status) &&
            !IgnoreRedundant) {
          toDiag(mDiags, mRegion->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(mDiags, Status ? Status->getLocation() : mRegion->getLocStart(),
                 clang::diag::note_parallel_localize_reduction_unable);
          return false;
        }
        for (++I; I != EI; ++I) {
          auto *Red = (**I).get<trait::Reduction>();
          if (!Red || Red->getKind() != CurrentKind) {
            if (!IgnoreRedundant) {
              toDiag(mDiags, mRegion->getLocStart(),
                clang::diag::warn_parallel_loop);
              auto Search = mASTVars.findDecl(*(*I)->getMemory(), mASTToClient,
                mDIMemoryMatcher);
              toDiag(mDiags,
                Search.first ? Search.first->getLocation()
                : mRegion->getLocStart(),
                clang::diag::note_parallel_reduction_unknown);
              return false;
            }
          } else {
            clang::VarDecl *Status = nullptr;
            if (!mASTVars.localize(**I, *TS.getNode(),
                  mASTToClient, mDIMemoryMatcher, ReductionList, &Status) &&
                !IgnoreRedundant) {
              toDiag(mDiags, mRegion->getLocStart(),
                     clang::diag::warn_parallel_loop);
              toDiag(mDiags,
                     Status ? Status->getLocation() : mRegion->getLocStart(),
                     clang::diag::note_parallel_localize_reduction_unable);
              return false;
            }
          }
        }
      }
    } else {
      if (Dptr.is<trait::SecondToLastPrivate>()) {
        clang::VarDecl *Status = nullptr;
        if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
              mDependenceInfo.get<trait::LastPrivate>(), &Status) &&
            !IgnoreRedundant) {
          toDiag(mDiags, mRegion->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(mDiags, Status ? Status->getLocation() : mRegion->getLocStart(),
                 clang::diag::note_parallel_localize_private_unable);
          return false;
        }
      }
      if (Dptr.is<trait::FirstPrivate>()) {
        clang::VarDecl *Status = nullptr;
        if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
              mDependenceInfo.get<trait::FirstPrivate>(), &Status) &&
            !IgnoreRedundant) {
          toDiag(mDiags, mRegion->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(mDiags, Status ? Status->getLocation() : mRegion->getLocStart(),
                 clang::diag::note_parallel_localize_private_unable);
          return false;
        }
      }
    }
  }
  // Check that localization of global variables (due to private or reduction
  // clauses) does not break relation with original global variables used
  // in calls.
  SpanningTreeRelation<DIAliasTree *> STR(&mDIAT);
  for (auto &NodeWithGlobal : mASTVars.GlobalRefs)
    for (auto *NodeWithSideEffect : DirectSideEffect)
      if (!STR.isUnreachable(NodeWithGlobal.first, NodeWithSideEffect)) {
        toDiag(mDiags, mRegion->getLocStart(), clang::diag::warn_parallel_loop);
        toDiag(mDiags,
               NodeWithGlobal.second ? NodeWithGlobal.second->getLocation()
                                     : mRegion->getLocStart(),
               clang::diag::note_parallel_localize_global_unable);
        return false;
      }
  // Check that traits for all variables referenced in the loop are properly
  // specified.
  for (auto &VarRef : mASTVars.CanonicalRefs)
    if (!mASTVars.CanonicalLocals.count(VarRef.first) &&
        llvm::count(VarRef.second, nullptr)) {
      toDiag(mDiags, mRegion->getLocStart(), clang::diag::warn_parallel_loop);
      toDiag(mDiags, VarRef.first->getLocation(),
             clang::diag::note_parallel_variable_not_analyzed)
          << VarRef.first->getName();
      return false;
    }
  return true;
}

bool ClangDependenceAnalyzer::evaluateDefUse() {
  for (auto *TS : mInToLocalize) {
    clang::VarDecl *Status = nullptr;
    if (!mASTVars.localize(*TS, mASTToClient, mDIMemoryMatcher,
          mDependenceInfo.get<trait::ReadOccurred>(), &Status)) {
      toDiag(mDiags, mRegion->getLocStart(),
             clang::diag::warn_parallel_loop);
      toDiag(mDiags, Status ? Status->getLocation() : mRegion->getLocStart(),
             clang::diag::note_parallel_localize_inout_unable);
      return false;
    }
  }
  for (auto *TS : mOutToLocalize) {
    clang::VarDecl *Status = nullptr;
    if (!mASTVars.localize(*TS, mASTToClient, mDIMemoryMatcher,
          mDependenceInfo.get<trait::WriteOccurred>(), &Status)) {
      toDiag(mDiags, mRegion->getLocStart(),
             clang::diag::warn_parallel_loop);
      toDiag(mDiags, Status ? Status->getLocation() : mRegion->getLocStart(),
             clang::diag::note_parallel_localize_inout_unable);
      return false;
    }
  }
  mDependenceInfo.get<trait::ReadOccurred>().insert(
      mDependenceInfo.get<trait::FirstPrivate>().begin(),
      mDependenceInfo.get<trait::FirstPrivate>().end());
  mDependenceInfo.get<trait::WriteOccurred>().insert(
      mDependenceInfo.get<trait::LastPrivate>().begin(),
      mDependenceInfo.get<trait::LastPrivate>().end());
  for (auto &Red : mDependenceInfo.get<trait::Reduction>()) {
    mDependenceInfo.get<trait::WriteOccurred>().insert(Red.begin(), Red.end());
    mDependenceInfo.get<trait::ReadOccurred>().insert(Red.begin(), Red.end());
  }
  return true;
}
