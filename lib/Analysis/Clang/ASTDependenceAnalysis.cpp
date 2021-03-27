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
  using ErrorListT =
      SmallVector<std::tuple<SortedVarListT, bool,
                             decltype(diag::PADDING_BUILTIN_TSAR_DIAGNOSTIC)>,
                  8>;
  ErrorListT Errors;
  auto getErrorInfo = [&Errors](auto Diag) -> ErrorListT::reference {
    auto I{find_if(Errors, [Diag](auto &E) { return std::get<2>(E) == Diag; })};
    if (I == Errors.end()) {
      Errors.emplace_back();
      I = std::prev(Errors.end());
      std::get<bool>(*I) = false;
      std::get<2>(*I) = Diag;
    }
    return *I;
  };
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
      auto &MultipleInduction{
          getErrorInfo(tsar::diag::note_parallel_multiple_induction)};
      auto &NotLocalized{
          getErrorInfo(tsar::diag::note_parallel_localize_induction_unable)};
      for (auto &I : TS) {
        auto Localized{mASTVars.localize(*I, *TS.getNode(), mASTToClient,
                                         mDIMemoryMatcher)};
        if (!std::get<VarDecl *>(Localized) || !std::get<2>(Localized) ||
            Induction &&
                &*Induction !=
                    std::get<VarDecl *>(Localized)->getCanonicalDecl()) {
          if (!IgnoreRedundant) {
            if (!std::get<2>(Localized)) {
              std::get<bool>(NotLocalized) = true;
              if (std::get<VarDecl *>(Localized))
                std::get<SortedVarListT>(NotLocalized)
                    .emplace(std::get<VarDecl *>(Localized),
                             std::get<DIMemory *>(Localized));
            } else {
              std::get<bool>(MultipleInduction) = true;
              if (Induction)
                std::get<SortedVarListT>(MultipleInduction)
                    .emplace(Induction, nullptr);
              if (std::get<VarDecl *>(Localized))
                std::get<SortedVarListT>(MultipleInduction)
                    .emplace(std::get<VarDecl *>(Localized),
                             std::get<DIMemory *>(Localized));
            }
          }
        } else {
          mDependenceInfo.get<trait::Induction>() = {
              std::get<VarDecl *>(Localized), std::get<DIMemory *>(Localized)};
          if (!std::get<3>(Localized)) {
            mInToLocalize.push_back(&TS);
            mOutToLocalize.push_back(&TS);
          }
        }
      }
    } else if (Dptr.is<trait::Private>()) {
      auto &Status{
          getErrorInfo(tsar::diag::note_parallel_localize_private_unable)};
      if (!mASTVars.localize(
              TS, mASTToClient, mDIMemoryMatcher,
              mDependenceInfo.get<trait::Private>(),
              !IgnoreRedundant ? &std::get<SortedVarListT>(Status) : nullptr) &&
          !IgnoreRedundant)
        std::get<bool>(Status) = true;
    } else if (Dptr.is<trait::Reduction>()) {
      auto I{TS.begin()}, EI{TS.end()};
      auto *Red{(**I).get<trait::Reduction>()};
      auto &UnknownReduction{
          getErrorInfo(tsar::diag::note_parallel_reduction_unknown)};
      auto CurrentKind{trait::DIReduction::RK_NoReduction};
      if (!Red || Red->getKind() == trait::DIReduction::RK_NoReduction) {
        if (!IgnoreRedundant) {
          auto Search{mASTVars.findDecl(*(*I)->getMemory(), mASTToClient,
                                        mDIMemoryMatcher)};
          std::get<bool>(UnknownReduction) = true;
          std::get<SortedVarListT>(UnknownReduction)
              .emplace(std::get<VarDecl *>(Search),
                       std::get<DIMemory *>(Search));
        }
      } else {
        CurrentKind = Red->getKind();
      }
      SortedVarListT Stub;
      auto &ReductionList{
          CurrentKind != trait::DIReduction::RK_NoReduction
              ? mDependenceInfo.get<trait::Reduction>()[CurrentKind]
              : Stub};
      auto &NotLocalized{
          getErrorInfo(tsar::diag::note_parallel_localize_reduction_unable)};
      if (!mASTVars.localize(
              **I, *TS.getNode(), mASTToClient, mDIMemoryMatcher, ReductionList,
              !IgnoreRedundant ? &std::get<SortedVarListT>(NotLocalized)
                               : nullptr) &&
          !IgnoreRedundant)
        std::get<bool>(NotLocalized) = true;
      for (++I; I != EI; ++I) {
        auto *Red{(**I).get<trait::Reduction>()};
        if (!Red || Red->getKind() != CurrentKind) {
          if (!IgnoreRedundant) {
            auto Search{mASTVars.findDecl(*(*I)->getMemory(), mASTToClient,
                                          mDIMemoryMatcher)};
            std::get<bool>(UnknownReduction) = true;
            std::get<SortedVarListT>(UnknownReduction)
                .emplace(std::get<VarDecl *>(Search),
                         std::get<DIMemory *>(Search));
          }
        } else if (!mASTVars.localize(
                       **I, *TS.getNode(), mASTToClient, mDIMemoryMatcher,
                       ReductionList,
                       !IgnoreRedundant
                           ? &std::get<SortedVarListT>(NotLocalized)
                           : nullptr) &&
                   !IgnoreRedundant) {
          std::get<bool>(NotLocalized) = true;
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
          auto Search{mASTVars.findDecl(*(*TS.begin())->getMemory(),
                                        mASTToClient, mDIMemoryMatcher)};
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
            auto &Status{
                getErrorInfo(tsar::diag::note_parallel_variable_not_analyzed)};
            std::get<bool>(Status) = true;
            if (std::get<VarDecl *>(Search))
              std::get<SortedVarListT>(Status).emplace(
                  std::get<VarDecl *>(Search), std::get<DIMemory *>(Search));
          }
        }
      }
    } else {
      if (Dptr.is<trait::SecondToLastPrivate>()) {
        auto &Status{
            getErrorInfo(tsar::diag::note_parallel_localize_private_unable)};
        if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
                               mDependenceInfo.get<trait::LastPrivate>(),
                               !IgnoreRedundant
                                   ? &std::get<SortedVarListT>(Status)
                                   : nullptr) &&
            !IgnoreRedundant)
          std::get<bool>(Status) = true;
      }
      if (Dptr.is<trait::FirstPrivate>()) {
        auto &Status{
            getErrorInfo(tsar::diag::note_parallel_localize_private_unable)};
        if (!mASTVars.localize(TS, mASTToClient, mDIMemoryMatcher,
                               mDependenceInfo.get<trait::FirstPrivate>(),
                               !IgnoreRedundant
                                   ? &std::get<SortedVarListT>(Status)
                                   : nullptr) &&
            !IgnoreRedundant)
          std::get<bool>(Status) = true;
      }
    }
  }
  // Check that localization of global variables (due to private or reduction
  // clauses) does not break relation with original global variables used
  // in calls.
  SpanningTreeRelation<DIAliasTree *> STR(&mDIAT);
  auto &NotLocalizedGlobal{
      getErrorInfo(tsar::diag::note_parallel_localize_global_unable)};
  for (auto &NodeWithGlobal : mASTVars.GlobalRefs)
    for (auto *NodeWithSideEffect : DirectSideEffect)
      if (!STR.isUnreachable(NodeWithGlobal.first, NodeWithSideEffect)) {
        std::get<bool>(NotLocalizedGlobal) = true;
        std::get<SortedVarListT>(NotLocalizedGlobal)
            .emplace(NodeWithGlobal.second, nullptr);
      }
  // Check that traits for all variables referenced in the loop are properly
  // specified.
  auto &NotAnalyzed{
      getErrorInfo(tsar::diag::note_parallel_variable_not_analyzed)};
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
      std::get<bool>(NotAnalyzed) = true;
      std::get<SortedVarListT>(NotAnalyzed).emplace(VarRef.first, nullptr);
    }
  if (std::any_of(Errors.begin(), Errors.end(),
                  [](auto &E) { return std::get<bool>(E); })) {
    toDiag(mDiags, mRegion->getBeginLoc(), tsar::diag::warn_parallel_loop);
    for (auto &&[Vars, Presence, Diag] : Errors) {
      if (Presence)
        if (Vars.empty())
          toDiag(mDiags, mRegion->getBeginLoc(), Diag);
        else
          for (auto &V : Vars)
            toDiag(mDiags, V.get<AST>()->getLocation(), Diag);
    }
    return false;
  }
  return true;
}

bool ClangDependenceAnalyzer::evaluateDefUse() {
  bool IsOk{true};
  SortedVarListT REs, WEs;
  for (auto *TS : mInToLocalize)
    IsOk &=
        mASTVars.localize(*TS, mASTToClient, mDIMemoryMatcher,
                          mDependenceInfo.get<trait::ReadOccurred>(), &REs);
  for (auto *TS : mOutToLocalize)
    IsOk &=
        mASTVars.localize(*TS, mASTToClient, mDIMemoryMatcher,
                          mDependenceInfo.get<trait::WriteOccurred>(), &WEs);
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
    if (REs.empty() && WEs.empty()) {
      toDiag(mDiags, mRegion->getBeginLoc(),
             tsar::diag::note_parallel_localize_inout_unable);
    } else {
      // Copy all variables in a single list to avoid duplicates in diagnostic
      // messages.
      REs.insert(WEs.begin(), WEs.end());
      for (auto &Var : REs)
        toDiag(mDiags, Var.get<AST>()->getLocation(),
               tsar::diag::note_parallel_localize_inout_unable);
    }
  }
  return IsOk;
}
