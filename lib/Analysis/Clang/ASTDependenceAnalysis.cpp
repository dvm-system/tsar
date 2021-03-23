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
  CanonicalDeclPtr<VarDecl> Induction;
  if (auto *For{dyn_cast<ForStmt>(mRegion)}) {
    if (auto *DS{dyn_cast_or_null<DeclStmt>(For->getInit())})
      Induction = cast<VarDecl>(DS->getSingleDecl());
    else if (auto *BO{dyn_cast_or_null<clang::BinaryOperator>(For->getInit())};
             BO && BO->isAssignmentOp())
      if (auto *DRE{dyn_cast<DeclRefExpr>(BO->getLHS())})
        Induction = dyn_cast<VarDecl>(DRE->getFoundDecl());
  }
  if (!Induction) {
    toDiag(mDiags, mRegion->getBeginLoc(),
           tsar::diag::warn_parallel_no_induction);
    return false;
  }
  if (auto *For{dyn_cast<ForStmt>(mRegion)})
    mASTVars.TraverseLoopIteration(For);
  else
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
        mASTVars.findDecl(*T->getMemory(), mASTToClient, mDIMemoryMatcher);
      mInToLocalize.push_back(&TS);
      if (Dptr.is<trait::Shared>())
        mOutToLocalize.push_back(&TS);
    } else if (Dptr.is<trait::Induction>()) {
      if (Induction && TS.size() > 1) {
        if (!IgnoreRedundant) {
          toDiag(mDiags, mRegion->getBeginLoc(),
                 tsar::diag::warn_parallel_loop);
          toDiag(mDiags, mRegion->getBeginLoc(),
                 tsar::diag::note_parallel_multiple_induction);
          return false;
        }
      } else {
        auto Localized = mASTVars.localize(**TS.begin(), *TS.getNode(),
                                           mASTToClient, mDIMemoryMatcher);
        if (!std::get<VarDecl *>(Localized) || !std::get<2>(Localized) ||
            Induction &&
                &*Induction !=
                    std::get<VarDecl *>(Localized)->getCanonicalDecl()) {
          if (!IgnoreRedundant) {
            toDiag(mDiags, mRegion->getBeginLoc(),
                   tsar::diag::warn_parallel_loop);
            if (!std::get<2>(Localized)) {
              toDiag(mDiags,
                     std::get<VarDecl *>(Localized)
                         ? std::get<0>(Localized)->getLocation()
                         : mRegion->getBeginLoc(),
                     tsar::diag::note_parallel_localize_induction_unable);
            } else {
              if (Induction)
                toDiag(mDiags, Induction->getLocation(),
                      tsar::diag::note_parallel_multiple_induction);
              if (std::get<VarDecl *>(Localized))
                toDiag(mDiags, std::get<0>(Localized)->getLocation(),
                       tsar::diag::note_parallel_multiple_induction);
            }
            return false;
          }
        }
        mDependenceInfo.get<trait::Induction>() = {
            std::get<VarDecl *>(Localized), std::get<DIMemory *>(Localized)};
        if (!std::get<3>(Localized)) {
          mInToLocalize.push_back(&TS);
          mOutToLocalize.push_back(&TS);
        }
      }
    } else if (Dptr.is<trait::Private>()) {
      clang::VarDecl *Status = nullptr;
      if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
                             mDependenceInfo.get<trait::Private>(), &Status) &&
          !IgnoreRedundant) {
        toDiag(mDiags, mRegion->getBeginLoc(), tsar::diag::warn_parallel_loop);
        toDiag(mDiags, Status ? Status->getLocation() : mRegion->getBeginLoc(),
          tsar::diag::note_parallel_localize_private_unable);
        return false;
      }
    } else if (Dptr.is<trait::Reduction>()) {
      auto I = TS.begin(), EI = TS.end();
      auto *Red = (**I).get<trait::Reduction>();
      if (!Red || Red->getKind() == trait::DIReduction::RK_NoReduction) {
        if (!IgnoreRedundant) {
          toDiag(mDiags, mRegion->getBeginLoc(),
                 tsar::diag::warn_parallel_loop);
          auto Search = mASTVars.findDecl(*(*I)->getMemory(), mASTToClient,
                                         mDIMemoryMatcher);
          toDiag(mDiags,
                 std::get<VarDecl *>(Search)
                     ? std::get<VarDecl *>(Search)->getLocation()
                     : mRegion->getBeginLoc(),
                 tsar::diag::note_parallel_reduction_unknown);
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
          toDiag(mDiags, mRegion->getBeginLoc(),
                 tsar::diag::warn_parallel_loop);
          toDiag(mDiags, Status ? Status->getLocation() : mRegion->getBeginLoc(),
                 tsar::diag::note_parallel_localize_reduction_unable);
          return false;
        }
        for (++I; I != EI; ++I) {
          auto *Red = (**I).get<trait::Reduction>();
          if (!Red || Red->getKind() != CurrentKind) {
            if (!IgnoreRedundant) {
              toDiag(mDiags, mRegion->getBeginLoc(),
                tsar::diag::warn_parallel_loop);
              auto Search = mASTVars.findDecl(*(*I)->getMemory(), mASTToClient,
                mDIMemoryMatcher);
              toDiag(mDiags,
                     std::get<VarDecl *>(Search)
                         ? std::get<VarDecl *>(Search)->getLocation()
                         : mRegion->getBeginLoc(),
                     tsar::diag::note_parallel_reduction_unknown);
              return false;
            }
          } else {
            clang::VarDecl *Status = nullptr;
            if (!mASTVars.localize(**I, *TS.getNode(),
                  mASTToClient, mDIMemoryMatcher, ReductionList, &Status) &&
                !IgnoreRedundant) {
              toDiag(mDiags, mRegion->getBeginLoc(),
                     tsar::diag::warn_parallel_loop);
              toDiag(mDiags,
                     Status ? Status->getLocation() : mRegion->getBeginLoc(),
                     tsar::diag::note_parallel_localize_reduction_unable);
              return false;
            }
          }
        }
      }
    } else if (Dptr.is_any<trait::Anti, trait::Flow>() &&
               !Dptr.is<trait::Output>()) {
      for (auto &T : TS) {
        DistanceInfo Info;
        auto setIf = [&Info, &T](auto &&Kind) {
          if (auto Dep = T->get<std::decay_t<decltype(Kind)>>()) {
            if (!Dep->isKnownDistance())
              return false;
            Info.get<std::decay_t<decltype(Kind)>>().resize(
                Dep->getKnownLevel());
            for (unsigned I = 0, EI = Dep->getKnownLevel(); I < EI; ++I)
              Info.get<std::decay_t<decltype(Kind)>>()[I] = Dep->getDistance(I);
          }
          return true;
        };
        if (setIf(trait::Flow{}) && setIf(trait::Anti{})) {
          clang::VarDecl *Status = nullptr;
          auto Search = mASTVars.findDecl(*(*TS.begin())->getMemory(),
                                          mASTToClient, mDIMemoryMatcher);
          if (std::get<VarDecl *>(Search) &&
              (std::get<VariableCollector::DeclSearch>(Search) ==
                   VariableCollector::CoincideLocal ||
              VariableCollector::CoincideGlobal)) {
            mDependenceInfo.get<trait::Dependence>().emplace(
                std::piecewise_construct,
                std::forward_as_tuple(std::get<VarDecl *>(Search),
                                      std::get<DIMemory *>(Search)),
                std::forward_as_tuple(std::move(Info)));
            mInToLocalize.push_back(&TS);
            mOutToLocalize.push_back(&TS);
          } else if (!IgnoreRedundant) {
            toDiag(mDiags, mRegion->getBeginLoc(),
                   tsar::diag::warn_parallel_loop);
            if (std::get<VarDecl *>(Search))
              toDiag(mDiags, std::get<VarDecl *>(Search)->getLocation(),
                     tsar::diag::note_parallel_variable_not_analyzed);
            return false;
          }
        }
      }
    } else {
      if (Dptr.is<trait::SecondToLastPrivate>()) {
        clang::VarDecl *Status = nullptr;
        if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
              mDependenceInfo.get<trait::LastPrivate>(), &Status) &&
            !IgnoreRedundant) {
          toDiag(mDiags, mRegion->getBeginLoc(),
                 tsar::diag::warn_parallel_loop);
          toDiag(mDiags, Status ? Status->getLocation() : mRegion->getBeginLoc(),
                 tsar::diag::note_parallel_localize_private_unable);
          return false;
        }
      }
      if (Dptr.is<trait::FirstPrivate>()) {
        clang::VarDecl *Status = nullptr;
        if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
              mDependenceInfo.get<trait::FirstPrivate>(), &Status) &&
            !IgnoreRedundant) {
          toDiag(mDiags, mRegion->getBeginLoc(),
                 tsar::diag::warn_parallel_loop);
          toDiag(mDiags, Status ? Status->getLocation() : mRegion->getBeginLoc(),
                 tsar::diag::note_parallel_localize_private_unable);
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
        toDiag(mDiags, mRegion->getBeginLoc(), tsar::diag::warn_parallel_loop);
        toDiag(mDiags,
               NodeWithGlobal.second ? NodeWithGlobal.second->getLocation()
                                     : mRegion->getBeginLoc(),
               tsar::diag::note_parallel_localize_global_unable);
        return false;
      }
  // Check that traits for all variables referenced in the loop are properly
  // specified.
  for (auto &VarRef : mASTVars.CanonicalRefs)
    if (!mASTVars.CanonicalLocals.count(VarRef.first) &&
        !llvm::all_of(VarRef.second, [this](const auto &Derived) {
          return Derived &&
                     (Derived.Kind == VariableCollector::DK_Strong ||
                      mGlobalOpts.IgnoreRedundantMemory ==
                              GlobalOptions::IRMK_Bounded &&
                          Derived.Kind == VariableCollector::DK_Bounded ||
                      mGlobalOpts.IgnoreRedundantMemory ==
                              GlobalOptions::IRMK_Partial &&
                          Derived.Kind == VariableCollector::DK_Partial) ||
                 !Derived && mGlobalOpts.IgnoreRedundantMemory ==
                                 GlobalOptions::IRMK_Weak;
        })) {
      toDiag(mDiags, mRegion->getBeginLoc(), tsar::diag::warn_parallel_loop);
      toDiag(mDiags, VarRef.first->getLocation(),
             tsar::diag::note_parallel_variable_not_analyzed)
          << VarRef.first->getName();
      return false;
    }
  return true;
}

bool ClangDependenceAnalyzer::evaluateDefUse() {
  bool IsOk{true};
  clang::VarDecl *Status{nullptr};
  for (auto *TS : mInToLocalize)
    IsOk &=
        mASTVars.localize(*TS, mASTToClient, mDIMemoryMatcher,
                          mDependenceInfo.get<trait::ReadOccurred>(), &Status);
  for (auto *TS : mOutToLocalize)
    IsOk &=
        mASTVars.localize(*TS, mASTToClient, mDIMemoryMatcher,
                          mDependenceInfo.get<trait::WriteOccurred>(), &Status);
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
  if (!IsOk) {
    toDiag(mDiags, mRegion->getBeginLoc(), tsar::diag::warn_parallel_loop);
    toDiag(mDiags, Status ? Status->getLocation() : mRegion->getBeginLoc(),
           tsar::diag::note_parallel_localize_inout_unable);
  }
  return IsOk;
}
