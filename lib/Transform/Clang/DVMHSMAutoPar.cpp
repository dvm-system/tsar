//===-- DVMHSMAutoPar.cpp - DVMH Based Parallelization (Clang) ---*- C++ -*===//
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
// This file implements a pass to perform DVMH-based auto parallelization for
// shared memory.
//
//===----------------------------------------------------------------------===//

#include "SharedMemoryAutoPar.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/ExpressionMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/Utils.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <clang/AST/ParentMapContext.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>


using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-sm-parallel"

namespace {
using DistanceInfo = ClangDependenceAnalyzer::DistanceInfo;
using VariableT = ClangDependenceAnalyzer::VariableT;
using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
using SortedVarMultiListT = ClangDependenceAnalyzer::SortedVarMultiListT;

class PragmaRegion : public ParallelLevel {
public:
  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<SortedVarListT, trait::ReadOccurred>,
                        bcl::tagged<SortedVarListT, trait::WriteOccurred>>;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmRegion);
  }

  explicit PragmaRegion(bool HostOnly = false)
      : ParallelLevel(static_cast<unsigned>(DirectiveId::DvmRegion), false),
        mHostOnly(HostOnly) {}

  ClauseList &getClauses() noexcept { return mClauses; }
  const ClauseList &getClauses() const noexcept { return mClauses; }

  void setHostOnly(bool HostOnly = true) { mHostOnly = HostOnly; }
  bool isHostOnly() const noexcept { return mHostOnly; }

private:
  ClauseList mClauses;
  bool mHostOnly;
};

class PragmaData : public ParallelLevel {
public:
  enum State : uint8_t {
    Default = 0,
    Required = 1 << 0u,
    Skipped = 1 << 1u,
    Invalid = 1 << 2u,
    LLVM_MARK_AS_BITMASK_ENUM(Invalid)
  };

  static bool classof(const ParallelItem *Item) noexcept {
    switch (static_cast<DirectiveId>(Item->getKind())) {
    case DirectiveId::DvmGetActual:
    case DirectiveId::DvmActual:
    case DirectiveId::DvmRemoteAccess:
      return true;
    }
    return false;
  }

  SortedVarListT &getMemory() noexcept { return mMemory; }
  const SortedVarListT &getMemory() const noexcept { return mMemory; }

  bool isRequired() const noexcept { return mState & Required; }
  bool isSkipped() const noexcept { return mState & Skipped; }
  bool isInvalid() const noexcept { return mState & Invalid; }

  void skip() noexcept { mState |= Skipped; }
  void actualize() noexcept { mState &= ~Skipped; }
  void invalidate() noexcept { mState |= Invalid; }

protected:
  PragmaData(DirectiveId Id, bool IsRequired, bool IsFinal)
      : ParallelLevel(static_cast<unsigned>(Id), IsFinal),
        mState(IsRequired ? Required : Default) {}

  bool isMergeableWith(bool IsRequired, bool IsFinal) const noexcept {
    return IsRequired == isRequired() && IsFinal == isFinal();
  }

private:
  SortedVarListT mMemory;
  State mState;
};

class PragmaActual : public PragmaData {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmActual);
  }

  PragmaActual(bool IsRequired, bool IsFinal = false)
      : PragmaData(DirectiveId::DvmActual, IsRequired, IsFinal) {}

  bool isMergeableWith(bool IsRequired, bool IsFinal = false) const noexcept {
    return PragmaData::isMergeableWith(IsRequired, IsFinal);
  }
};

class PragmaGetActual : public PragmaData {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmGetActual);
  }

  PragmaGetActual(bool IsRequired, bool IsFinal = false)
      : PragmaData(DirectiveId::DvmGetActual, IsRequired, IsFinal) {}

};

class PragmaParallel : public ParallelItem {
public:
  using AcrossVarListT =
      std::map<VariableT, trait::DIDependence::DistanceVector,
               ClangDependenceAnalyzer::VariableLess>;
  using LoopNestT =
      SmallVector<bcl::tagged_pair<bcl::tagged<ObjectID, Loop>,
                                   bcl::tagged<VariableT, VariableT>>,
                  4>;
  using VarMappingT =
      std::multimap<VariableT, SmallVector<std::pair<ObjectID, bool>, 4>,
                    ClangDependenceAnalyzer::VariableLess>;

  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>,
                        bcl::tagged<AcrossVarListT, trait::Dependence>,
                        bcl::tagged<LoopNestT, trait::Induction>,
                        bcl::tagged<VarMappingT, trait::DirectAccess>>;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmParallel);
  }

  PragmaParallel()
      : ParallelItem(static_cast<unsigned>(DirectiveId::DvmParallel), false) {}

  ClauseList &getClauses() noexcept { return mClauses; }
  const ClauseList &getClauses() const noexcept { return mClauses; }

  unsigned getPossibleAcrossDepth() const noexcept {
    return mPossibleAcrossDepth;
  }

  void setPossibleAcrossDepth(unsigned Depth) noexcept {
    mPossibleAcrossDepth = Depth;
  }

private:
  ClauseList mClauses;
  unsigned mPossibleAcrossDepth = 0;
};

/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangDVMHSMParallelization : public ClangSMParallelization {
  /// Stack of lists which contain actual/get_actual directives.
  /// If optimization is successful for a level then the corresponding
  /// directives can be removed.
  using RegionDataReplacement = std::vector<SmallVector<ParallelLevel *, 4>>;

  /// Variables which are presented in some actual or get_actual directive
  /// correspondingly. The key is an alias node which contains a corresponding
  /// memory on the analysis server. Hierarchy field identifies the innermost
  /// level (in the RegionDataReplacement stack) which contains a corresponding
  /// directive.
  using RegionDataCache = DenseMap<
      const DIAliasNode *, std::tuple<SmallVector<VariableT, 1>, unsigned>,
      DenseMapInfo<const DIAliasNode *>,
      TaggedDenseMapTuple<bcl::tagged<const DIAliasNode *, DIAliasNode>,
                          bcl::tagged<SmallVector<VariableT, 1>, VariableT>,
                          bcl::tagged<unsigned, Hierarchy>>>;

public:
  static char ID;
  ClangDVMHSMParallelization() : ClangSMParallelization(ID) {
    initializeClangDVMHSMParallelizationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    ClangSMParallelization::getAnalysisUsage(AU);
    AU.addRequired<TargetLibraryInfoWrapperPass>();
  }

private:
  ParallelItem * exploitParallelism(const DFLoop &IR, const clang::ForStmt &AST,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    ParallelItem *PI) override;

  bool processRegularDependenceis(const DFLoop &DFL,
    const tsar::ClangDependenceAnalyzer &ASRegionAnalysis,
    const FunctionAnalysis &Provider, PragmaParallel &DVMHParallel);

  void optimizeLevel(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  bool optimizeGlobalIn(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  bool optimizeGlobalOut(PointerUnion<Loop *, Function *> Level,
    const FunctionAnalysis &Provider) override;

  Parallelization mParallelizationInfo;
  RegionDataCache mToActual, mToGetActual;
  RegionDataReplacement mReplacementFor;
  SmallPtrSet<const DIAliasNode *, 8> mDistinctMemory;
};

struct Insertion {
  using PragmaString = SmallString<128>;
  using PragmaList = SmallVector<std::tuple<ParallelItem *, PragmaString>, 2>;
  PragmaList Before, After;
};
using LocationToPragmas = DenseMap<const Stmt *, Insertion>;
} // namespace

bool ClangDVMHSMParallelization::processRegularDependenceis(const DFLoop &DFL,
    const tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    const FunctionAnalysis &Provider, PragmaParallel &DVMHParallel) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (ASTDepInfo.get<trait::Dependence>().empty())
    return true;
  assert(DVMHParallel.getPossibleAcrossDepth() == 0 ||
         DVMHParallel.getClauses().get<trait::Induction>().size() <
             DVMHParallel.getPossibleAcrossDepth() &&
         "Maximum depth of a parallel nest has been exceeded!");
  auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
  auto CanonicalItr = CL.find_as(&DFL);
  auto ConstStep = dyn_cast_or_null<SCEVConstant>((*CanonicalItr)->getStep());
  if (!ConstStep) {
    toDiag(ASTRegionAnalysis.getDiagnostics(),
           ASTRegionAnalysis.getRegion()->getBeginLoc(),
           tsar::diag::warn_parallel_loop);
    toDiag(ASTRegionAnalysis.getDiagnostics(),
           ASTRegionAnalysis.getRegion()->getBeginLoc(),
           tsar::diag::note_parallel_across_direction_unknown);
    return false;
  }
  auto LoopID = DFL.getLoop()->getLoopID();
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (!AccessInfo)
    return false;
  unsigned PossibleAcrossDepth =
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Flow>().empty() ?
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Anti>().size() :
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Flow>().size();
  auto updatePAD = [&PossibleAcrossDepth](auto &Dep, auto T) {
    if (!Dep.second.template get<decltype(T)>().empty()) {
      auto MinDepth = *Dep.second.template get<decltype(T)>().front().first;
      unsigned PAD = 1;
      for (auto InnerItr = Dep.second.template get<decltype(T)>().begin() + 1,
                InnerItrE = Dep.second.template get<decltype(T)>().end();
           InnerItr != InnerItrE; ++InnerItr, ++PAD)
        if (InnerItr->first->isNegative()) {
          auto Revert = -(*InnerItr->first);
          Revert.setIsUnsigned(true);
          if (Revert >= MinDepth)
            break;
        }
      PossibleAcrossDepth = std::min(PossibleAcrossDepth, PAD);
    }
  };
  for (auto &Dep : ASTDepInfo.get<trait::Dependence>()) {
    auto AccessItr =
        find_if(AccessInfo->scope_accesses(LoopID), [&Dep](auto &Access) {
            return Access.getArray() == Dep.first.get<MD>();
        });
    if (AccessItr == AccessInfo->scope_end(LoopID))
      return false;
    Optional<unsigned> DependentDim;
    unsigned NumberOfDims = 0;
    for (auto &Access :
         AccessInfo->array_accesses(AccessItr->getArray(), LoopID)) {
      NumberOfDims = std::max(NumberOfDims, Access.size());
      for (auto *Subscript : Access) {
        if (!Subscript || !isa<DIAffineSubscript>(Subscript))
          return false;
        auto Affine = cast<DIAffineSubscript>(Subscript);
        ObjectID AnotherColumn = nullptr;
        for (unsigned Idx = 0, IdxE = Affine->getNumberOfMonoms(); Idx < IdxE;
             ++Idx) {
          if (Affine->getMonom(Idx).Column == LoopID) {
            if (AnotherColumn ||
                DependentDim && *DependentDim != Affine->getDimension())
              return false;
            DependentDim = Affine->getDimension();
          } else {
            if (DependentDim && *DependentDim == Affine->getDimension())
              return false;
            AnotherColumn = Affine->getMonom(Idx).Column;
          }
        }
      }
    }
    if (!DependentDim)
      return false;
    updatePAD(Dep, trait::Flow{});
    updatePAD(Dep, trait::Anti{});
    auto I =
        DVMHParallel.getClauses()
            .get<trait::Dependence>()
            .emplace(std::piecewise_construct, std::forward_as_tuple(Dep.first),
                     std::forward_as_tuple())
            .first;
    I->second.resize(NumberOfDims);
    auto getDistance = [&Dep](auto T) {
      return Dep.second.get<decltype(T)>().empty()
                 ? None
                 : Dep.second.get<decltype(T)>().front().second;
    };
    if (ConstStep->getAPInt().isNegative())
      I->second[*DependentDim] = {getDistance(trait::Anti{}),
                                  getDistance(trait::Flow{})};

    else
      I->second[*DependentDim] = {getDistance(trait::Flow{}),
                                  getDistance(trait::Anti{})};
  }
  PossibleAcrossDepth += DVMHParallel.getClauses().get<trait::Induction>().size();
  if (DVMHParallel.getPossibleAcrossDepth() == 0)
    DVMHParallel.setPossibleAcrossDepth(PossibleAcrossDepth);
  else
    DVMHParallel.setPossibleAcrossDepth(
        std::min(DVMHParallel.getPossibleAcrossDepth(), PossibleAcrossDepth));
  return true;
}

ParallelItem *ClangDVMHSMParallelization::exploitParallelism(
    const DFLoop &IR, const clang::ForStmt &For,
    const FunctionAnalysis &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis, ParallelItem *PI) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
      !ASTDepInfo.get<trait::LastPrivate>().empty() ||
      !ASTDepInfo.get<trait::Induction>().get<AST>()) {
    if (PI)
      PI->finalize();
    return PI;
  }
  if (PI) {
    auto DVMHParallel = cast<PragmaParallel>(PI);
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    DVMHParallel->getClauses().get<trait::Private>().erase(
        ASTDepInfo.get<trait::Induction>());
    if (PL[IR.getLoop()].isHostOnly() ||
        ASTDepInfo.get<trait::Private>() !=
            DVMHParallel->getClauses().get<trait::Private>() ||
        ASTDepInfo.get<trait::Reduction>() !=
            DVMHParallel->getClauses().get<trait::Reduction>()) {
      DVMHParallel->getClauses().get<trait::Private>().insert(
          ASTDepInfo.get<trait::Induction>());
      PI->finalize();
      return PI;
    }
    if (!processRegularDependenceis(IR, ASTRegionAnalysis, Provider,
                                    *DVMHParallel)) {
      PI->finalize();
      return PI;
    }
  } else {
    std::unique_ptr<PragmaActual> DVMHActual;
    std::unique_ptr<PragmaGetActual> DVMHGetActual;
    std::unique_ptr<PragmaRegion> DVMHRegion;
    auto DVMHParallel{std::make_unique<PragmaParallel>()};
    SortedVarMultiListT NotLocalized;
    auto Localized = ASTRegionAnalysis.evaluateDefUse(&NotLocalized);
    if (Localized) {
      assert(NotLocalized.empty() && "All variables must be localized!");
      DVMHRegion = std::make_unique<PragmaRegion>();
      DVMHRegion->finalize();
      DVMHRegion->child_insert(DVMHParallel.get());
      DVMHParallel->parent_insert(DVMHRegion.get());
      for (const auto &V : ASTDepInfo.get<trait::WriteOccurred>())
        DVMHParallel->getClauses().template get<trait::DirectAccess>().emplace(
            std::piecewise_construct, std::forward_as_tuple(V),
            std::forward_as_tuple());
      for (const auto &V : ASTDepInfo.get<trait::ReadOccurred>())
        DVMHParallel->getClauses().template get<trait::DirectAccess>().emplace(
            std::piecewise_construct, std::forward_as_tuple(V),
            std::forward_as_tuple());
    } else {
      for (const auto &V : NotLocalized)
        DVMHParallel->getClauses().template get<trait::DirectAccess>().emplace(
            std::piecewise_construct, std::forward_as_tuple(V),
            std::forward_as_tuple());
    }
    PI = DVMHParallel.get();
    DVMHParallel->getClauses().get<trait::Private>().insert(
        ASTDepInfo.get<trait::Private>().begin(),
        ASTDepInfo.get<trait::Private>().end());
    for (unsigned I = 0, EI = ASTDepInfo.get<trait::Reduction>().size(); I < EI;
         ++I)
      DVMHParallel->getClauses().get<trait::Reduction>()[I].insert(
          ASTDepInfo.get<trait::Reduction>()[I].begin(),
          ASTDepInfo.get<trait::Reduction>()[I].end());
    if (!processRegularDependenceis(IR, ASTRegionAnalysis, Provider,
                                    *DVMHParallel))
      return nullptr;
    auto &PL = Provider.value<ParallelLoopPass *>()->getParallelLoopInfo();
    if (!PL[IR.getLoop()].isHostOnly() && Localized) {
      if (!ASTDepInfo.get<trait::ReadOccurred>().empty()) {
        DVMHActual = std::make_unique<PragmaActual>(false);
        DVMHActual->getMemory()
            .insert(ASTDepInfo.get<trait::ReadOccurred>().begin(),
                    ASTDepInfo.get<trait::ReadOccurred>().end());
        DVMHRegion->getClauses().get<trait::ReadOccurred>().insert(
            ASTDepInfo.get<trait::ReadOccurred>().begin(),
            ASTDepInfo.get<trait::ReadOccurred>().end());
      }
      if (!ASTDepInfo.get<trait::WriteOccurred>().empty()) {
        DVMHGetActual = std::make_unique<PragmaGetActual>(false);
        DVMHGetActual->getMemory().insert(
            ASTDepInfo.get<trait::WriteOccurred>().begin(),
            ASTDepInfo.get<trait::WriteOccurred>().end());
        DVMHRegion->getClauses().get<trait::WriteOccurred>().insert(
            ASTDepInfo.get<trait::WriteOccurred>().begin(),
            ASTDepInfo.get<trait::WriteOccurred>().end());
      }
      DVMHRegion->getClauses().get<trait::Private>().insert(
          ASTDepInfo.get<trait::Private>().begin(),
          ASTDepInfo.get<trait::Private>().end());
    } else if (Localized) {
      DVMHRegion->setHostOnly(true);
      // TODO (kaniandr@gmail.com): try to predict influence of OpenMP collapse
      // directives. Sometimes they may degrade performance, so we do not use
      // them now if there are no regular dependencies.
      if (ASTDepInfo.get<trait::Dependence>().empty())
        DVMHParallel->finalize();
    } else if (ASTDepInfo.get<trait::Dependence>().empty()) {
      // TODO (kaniandr@gmail.com): try to predict influence of OpenMP collapse
      // directives. Sometimes they may degrade performance, so we do not use
      // them now if there are no regular dependencies.
      DVMHParallel->finalize();
    }
    auto EntryInfo =
        mParallelizationInfo.try_emplace(IR.getLoop()->getHeader());
    assert(EntryInfo.second && "Unable to create a parallel block!");
    EntryInfo.first->get<ParallelLocation>().emplace_back();
    EntryInfo.first->get<ParallelLocation>().back().Anchor =
        IR.getLoop()->getLoopID();
    auto ExitingBB = IR.getLoop()->getExitingBlock();
    assert(ExitingBB && "Parallel loop must have a single exit!");
    ParallelLocation *ExitLoc = nullptr;
    if (ExitingBB == IR.getLoop()->getHeader()) {
      ExitLoc = &EntryInfo.first->get<ParallelLocation>().back();
    } else {
      auto ExitInfo = mParallelizationInfo.try_emplace(ExitingBB);
      assert(ExitInfo.second && "Unable to create a parallel block!");
      ExitInfo.first->get<ParallelLocation>().emplace_back();
      ExitLoc = &ExitInfo.first->get<ParallelLocation>().back();
      ExitLoc->Anchor = IR.getLoop()->getLoopID();
    }
    if (DVMHRegion)
      ExitLoc->Exit.push_back(
          std::make_unique<ParallelMarker<PragmaRegion>>(0, DVMHRegion.get()));
    if (DVMHActual)
      EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
          std::move(DVMHActual));
    if (DVMHRegion)
      EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
          std::move(DVMHRegion));
    EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
        std::move(DVMHParallel));
    if (DVMHGetActual)
      ExitLoc->Exit.push_back(std::move(DVMHGetActual));
  }
  cast<PragmaParallel>(PI)->getClauses().get<trait::Induction>().emplace_back(
      IR.getLoop()->getLoopID(), ASTDepInfo.get<trait::Induction>());
   auto &PerfectInfo =
      Provider.value<ClangPerfectLoopPass *>()->getPerfectLoopInfo();
  if (!PI->isFinal() &&
      (!PerfectInfo.count(&IR) || IR.getNumRegions() == 0 ||
       (cast<PragmaParallel>(PI)->getPossibleAcrossDepth() != 0 &&
        cast<PragmaParallel>(PI)->getClauses().get<trait::Induction>().size() ==
            cast<PragmaParallel>(PI)->getPossibleAcrossDepth())))
    PI->finalize();
  return PI;
}

static inline PragmaParallel *isParallel(const Loop *L,
                                         Parallelization &ParallelizationInfo) {
  if (auto ID = L->getLoopID()) {
    auto Ref{ParallelizationInfo.find<PragmaParallel>(L->getHeader(), ID)};
    return cast_or_null<PragmaParallel>(Ref);
  }
  return nullptr;
}

static inline Stmt *getScope(Loop *L,
    const LoopMatcherPass::LoopMatcher &LoopMatcher, ASTContext &ASTCtx) {
  auto &ParentCtx = ASTCtx.getParentMapContext();
  auto Itr = LoopMatcher.find<IR>(L);
  if (Itr == LoopMatcher.end())
    return nullptr;
  return const_cast<Stmt *>(
      ParentCtx.getParents(*Itr->get<AST>()).begin()->get<Stmt>());
}

static void mergeRegions(const SmallVectorImpl<Loop *> &ToMerge,
    Parallelization &ParallelizationInfo) {
  assert(ToMerge.size() > 1 && "At least two regions must be specified!");
  auto MergedRegion = ParallelizationInfo.find<PragmaRegion>(
      ToMerge.front()->getHeader(), ToMerge.front()->getLoopID(), true);
  auto MergedMarker = ParallelizationInfo.find<ParallelMarker<PragmaRegion>>(
      ToMerge.back()->getExitingBlock(), ToMerge.back()->getLoopID(), false);
  auto &MergedActual = *[&PB = MergedRegion.getPL()->Entry]() {
    auto I = find_if(PB, [](auto &PI) { return isa<PragmaActual>(*PI); });
    if (I != PB.end())
      return cast<PragmaActual>(I->get());
    PB.push_back(std::make_unique<PragmaActual>(false));
    return cast<PragmaActual>(PB.back().get());
  }();
  auto &MergedGetActual = *[&PB = MergedMarker.getPL()->Exit]() {
    auto I = find_if(PB, [](auto &PI) { return isa<PragmaGetActual>(*PI); });
    if (I != PB.end())
      return cast<PragmaGetActual>(I->get());
    PB.push_back(std::make_unique<PragmaGetActual>(false));
    return cast<PragmaGetActual>(PB.back().get());
  }();
  cast<ParallelMarker<PragmaRegion>>(MergedMarker)
      ->parent_insert(MergedRegion.getUnchecked());
  auto copyActual = [](auto &PB, auto &To) {
    auto I = find_if(PB, [&To](auto &PI) {
      return isa<std::decay_t<decltype(To)>>(PI.get());
    });
    if (I != PB.end()) {
      auto &M = cast<std::decay_t<decltype(To)>>(**I).getMemory();
      To.getMemory().insert(M.begin(), M.end());
    }
    return I;
  };
  auto copyInOut = [&To = cast<PragmaRegion>(MergedRegion)->getClauses()](
                       auto &FromRegion) {
    auto &From = FromRegion.getClauses();
    To.get<trait::ReadOccurred>().insert(
        From.template get<trait::ReadOccurred>().begin(),
        From.template get<trait::ReadOccurred>().end());
    To.get<trait::WriteOccurred>().insert(
        From.template get<trait::WriteOccurred>().begin(),
        From.template get<trait::WriteOccurred>().end());
    To.get<trait::Private>().insert(From.template get<trait::Private>().begin(),
                                    From.template get<trait::Private>().end());
  };
  // Remove start or and of region and corresponding actualization directive.
  auto remove = [&ParallelizationInfo](BasicBlock *BB, auto RegionItr,
      auto ActualItr,  auto PEdgeItr, ParallelBlock &OppositePB,
      ParallelBlock &FromPB) {
    if (FromPB.size() == 1 || FromPB.size() == 2 && ActualItr != FromPB.end()) {
      if (OppositePB.empty() &&
          PEdgeItr->template get<ParallelLocation>().size() == 1)
        ParallelizationInfo.erase(BB);
      else
        FromPB.clear();
    } else if (ActualItr != FromPB.end()) {
      FromPB.erase(RegionItr);
      FromPB.erase(find_if(FromPB, [&ActualItr](auto &PI) {
        return PI->getKind() == (*ActualItr)->getKind();
      }));
    } else {
      FromPB.erase(RegionItr);
    }
  };
  auto removeEndOfRegion = [&copyActual, &remove, &MergedGetActual,
                            &ParallelizationInfo](Loop *L) {
    auto ExitingBB = L->getExitingBlock();
    auto ID = L->getLoopID();
    auto Marker = ParallelizationInfo.find<ParallelMarker<PragmaRegion>>(
        ExitingBB, ID, false);
    auto &ExitPB = Marker.getPL()->Exit;
    auto GetActualItr = copyActual(ExitPB, MergedGetActual);
    remove(ExitingBB, Marker.getPI(), GetActualItr, Marker.getPE(),
           Marker.getPL()->Entry, ExitPB);
  };
  auto removeStartOfRegion = [&copyActual, &copyInOut, &remove, &MergedActual,
                              &ParallelizationInfo](Loop *L) {
    auto HeaderBB = L->getExitingBlock();
    auto ID = L->getLoopID();
    auto Region = ParallelizationInfo.find<PragmaRegion>(L->getHeader(), ID);
    copyInOut(cast<PragmaRegion>(*Region));
    auto &EntryPB = Region.getPL()->Entry;
    auto ActualItr = copyActual(EntryPB, MergedActual);
    remove(HeaderBB, Region.getPI(), ActualItr, Region.getPE(),
           Region.getPL()->Exit, EntryPB);
  };
  removeEndOfRegion(ToMerge.front());
  for (auto I = ToMerge.begin() + 1, EI = ToMerge.end() - 1; I != EI; ++I) {
    // All markers should be removed before the corresponding region.
    removeEndOfRegion(*I);
    removeStartOfRegion(*I);
  }
  removeStartOfRegion(ToMerge.back());
}

template<typename ItrT>
static void mergeSiblingRegions(ItrT I, ItrT EI,
    const FunctionAnalysis &Provider, ASTContext &ASTCtx,
    Parallelization &ParallelizationInfo) {
  if (I == EI)
    return;
  auto &LoopMatcher = Provider.value<LoopMatcherPass *>()->getMatcher();
  // At least one loop must be parallel, so IR-to-AST match is always known for
  // this loop.
  auto *ParentScope{[&]() -> clang::Stmt * {
    for (auto Itr{I}; Itr != EI; ++Itr)
      if (auto *S{getScope(*Itr, LoopMatcher, ASTCtx)})
        return S;
    return nullptr;
  }()};
  assert(ParentScope && "Unable to find AST scope for a parallel loop!");
  SmallVector<Loop *, 4> ToMerge;
  bool IsHostOnly = false;
  for (auto *Child : ParentScope->children()) {
    if (!Child)
      continue;
    if (auto *For = dyn_cast<ForStmt>(Child)) {
      auto MatchItr = LoopMatcher.find<AST>(For);
      if (MatchItr != LoopMatcher.end())
        if (auto *DVMHParallel{
                isParallel(MatchItr->template get<IR>(), ParallelizationInfo)};
            DVMHParallel && !DVMHParallel->parent_empty()) {
          auto *DVMHRegion{cast<PragmaRegion>(DVMHParallel->parent_front())};
          if (!ToMerge.empty()) {
            if (DVMHRegion->isHostOnly() == IsHostOnly) {
              ToMerge.push_back(MatchItr->template get<IR>());
              continue;
            }
            if (ToMerge.size() > 1)
              mergeRegions(ToMerge, ParallelizationInfo);
          }
          ToMerge.push_back(MatchItr->template get<IR>());
          IsHostOnly = DVMHRegion->isHostOnly();
          continue;
        }
    }
    if (ToMerge.size() > 1)
      mergeRegions(ToMerge, ParallelizationInfo);
    ToMerge.clear();
  }
  if (ToMerge.size() > 1)
    mergeRegions(ToMerge, ParallelizationInfo);
}

template <typename ItrT>
static void optimizeLevelImpl(ItrT I, ItrT EI, const FunctionAnalysis &Provider,
    const DIArrayAccessInfo &AccessInfo, Parallelization &ParallelizationInfo) {
  for (; I != EI; ++I) {
    auto *DVMHParallel = isParallel(*I, ParallelizationInfo);
    if (!DVMHParallel)
      continue;
    auto ID = (*I)->getLoopID();
    assert(ID && "Identifier must be known for a parallel loop!");
    auto &Clauses = DVMHParallel->getClauses();
    for (auto &Access : AccessInfo.scope_accesses(ID)) {
      if (!isa<DIEstimateMemory>(Access.getArray()))
        continue;
      auto MappingItr{find_if(
          Clauses.template get<trait::DirectAccess>(), [&Access](auto V) {
            return V.first.template get<MD>() == Access.getArray();
          })};
      if (MappingItr == Clauses.template get<trait::DirectAccess>().end())
        continue;
      if (is_contained(Clauses.template get<trait::Private>(),
                       MappingItr->first))
        continue;
      MappingItr->second.assign(Access.size(),
                                std::pair<ObjectID, bool>(nullptr, true));
      for (auto *Subscript : Access) {
        if (!Subscript || MappingItr->second[Subscript->getDimension()].first)
          continue;
        if (auto *Affine = dyn_cast<DIAffineSubscript>(Subscript)) {
          for (unsigned I = 0, EI = Affine->getNumberOfMonoms(); I < EI; ++I) {
            auto &Monom{Affine->getMonom(I)};
            if (Monom.Value.Kind == DIAffineSubscript::Symbol::SK_Constant &&
                Monom.Value.Constant.isNullValue())
              continue;
            auto Itr = find_if(
                Clauses.template get<trait::Induction>(),
                [Column = Monom.Column](auto &Level) {
                  return Level.template get<Loop>() == Column;
                });
            if (Itr != Clauses.template get<trait::Induction>().end())
              MappingItr->second[Affine->getDimension()] = {
                  Itr->template get<Loop>(),
                  Monom.Value.Kind == DIAffineSubscript::Symbol::SK_Constant &&
                      !Monom.Value.Constant.isNegative()};
          }
        }
      }
    }
  }
}

void ClangDVMHSMParallelization::optimizeLevel(
    PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (AccessInfo) {
    if (Level.is<Function *>()) {
      auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
      optimizeLevelImpl(LI.begin(), LI.end(), Provider, *AccessInfo,
                        mParallelizationInfo);
    } else {
      optimizeLevelImpl(Level.get<Loop *>()->begin(),
                        Level.get<Loop *>()->end(), Provider, *AccessInfo,
                        mParallelizationInfo);
    }
  }
  auto *M = Level.is<Function *>() ? Level.get<Function *>()->getParent() :
    Level.get<Loop *>()->getHeader()->getModule();
  auto &ASTCtx =
      getAnalysis<TransformationEnginePass>()->getContext(*M)->getContext();
  if (Level.is<Function *>()) {
    auto &LI = Provider.value<LoopInfoWrapperPass *>()->getLoopInfo();
    mergeSiblingRegions(LI.begin(), LI.end(), Provider, ASTCtx,
                      mParallelizationInfo);
  } else {
    mergeSiblingRegions(Level.get<Loop *>()->begin(),
                        Level.get<Loop *>()->end(), Provider, ASTCtx,
                        mParallelizationInfo);
  }
}

bool ClangDVMHSMParallelization::optimizeGlobalIn(
    PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  if (Level.is<Loop *>()) {
    if (isParallel(Level.get<Loop *>(), mParallelizationInfo))
      return false;
    if (auto *Parent{Level.get<Loop *>()->getParentLoop()};
        Parent && isParallel(Parent, mParallelizationInfo))
      return false;
  } else {
    mToActual.clear();
    mToGetActual.clear();
  }
  auto &F{Level.is<Function *>()
              ? *Level.get<Function *>()
              : *Level.get<Loop *>()->getHeader()->getParent()};
  auto &SocketInfo{**Provider.value<AnalysisSocketImmutableWrapper *>()};
  auto &Socket{SocketInfo.getActive()->second};
  if (Level.is<Function *>()) {
    auto RF{Socket.getAnalysis<DIEstimateMemoryPass>(F)};
    if (!RF)
      return false;
    auto &ServerDIAT{RF->value<DIEstimateMemoryPass *>()->getAliasTree()};
    mDistinctMemory.clear();
    for (auto &DIM :
         make_range(ServerDIAT.memory_begin(), ServerDIAT.memory_end()))
      if (auto *DIUM{dyn_cast<DIUnknownMemory>(&DIM)};
          DIUM && DIUM->isDistinct())
        mDistinctMemory.insert(DIUM->getAliasNode());
  }
  auto RM{Socket.getAnalysis<AnalysisClientServerMatcherWrapper,
                             ClonedDIMemoryMatcherWrapper>()};
  auto &ClientToServer{**RM->value<AnalysisClientServerMatcherWrapper *>()};
  auto &ServerF{cast<Function>(*ClientToServer[&F])};
  auto &CSMemoryMatcher{
      *(**RM->value<ClonedDIMemoryMatcherWrapper *>())[ServerF]};
  auto collectForParallelBlock = [this, &CSMemoryMatcher](
                                     ParallelBlock &PB, auto Anchor) {
    for (auto &PI : PB) {
      if (auto *Actual{dyn_cast<PragmaActual>(PI.get())}) {
        for (auto &Var : Actual->getMemory()) {
          auto DIMI{CSMemoryMatcher.find<Origin>(&*Var.get<MD>())};
          auto I{
              mToActual.try_emplace(DIMI->get<Clone>()->getAliasNode()).first};
          if (!is_contained(I->get<VariableT>(), Var))
            I->get<VariableT>().push_back(Var);
          if (!Actual->isFinal())
            I->get<Hierarchy>() = mReplacementFor.size() - 1;
        }
        if (!Actual->isFinal())
          mReplacementFor.back().push_back(Actual);
      } else if (auto *GetActual{dyn_cast<PragmaGetActual>(PI.get())}) {
        for (auto &Var : GetActual->getMemory()) {
          auto DIMI{CSMemoryMatcher.find<Origin>(&*Var.get<MD>())};
          auto I{mToGetActual.try_emplace(DIMI->get<Clone>()->getAliasNode())
                     .first};
          if (!is_contained(I->get<VariableT>(), Var))
            I->get<VariableT>().push_back(Var);
          if (!GetActual->isFinal())
            I->get<Hierarchy>() = mReplacementFor.size() - 1;
        }
        if (!GetActual->isFinal())
          mReplacementFor.back().push_back(GetActual);
      }
    }
  };
  mReplacementFor.emplace_back();
  for (auto &BB : F)
    if (auto ParallelItr{mParallelizationInfo.find(&BB)};
        ParallelItr != mParallelizationInfo.end())
      for (auto &PL : ParallelItr->get<ParallelLocation>()) {
        collectForParallelBlock(PL.Entry, PL.Anchor);
        collectForParallelBlock(PL.Exit, PL.Anchor);
      }
  // We go downward only if there are directives to optimize.
  return !(mToActual.empty() && mToGetActual.empty());
}

bool ClangDVMHSMParallelization::optimizeGlobalOut(
    PointerUnion<Loop *, Function *> Level, const FunctionAnalysis &Provider) {
  auto &F{Level.is<Function *>()
              ? *Level.get<Function *>()
              : *Level.get<Loop *>()->getHeader()->getParent()};
  auto &SocketInfo{**Provider.value<AnalysisSocketImmutableWrapper *>()};
  auto &Socket{SocketInfo.getActive()->second};
  auto RM{Socket.getAnalysis<AnalysisClientServerMatcherWrapper,
                             ClonedDIMemoryMatcherWrapper>()};
  auto &ClientToServer{**RM->value<AnalysisClientServerMatcherWrapper *>()};
  auto &ServerF{cast<Function>(*ClientToServer[&F])};
  auto &CSMemoryMatcher{
      *(**RM->value<ClonedDIMemoryMatcherWrapper *>())[ServerF]};
  // Map from induction variable to a list of headers of top-level loops
  // in parallel nests which contain this induction variable.
  DenseMap<const DIMemory *, TinyPtrVector<BasicBlock *>> ParallelLoops;
  auto collectParallelInductions = [this, &ParallelLoops](BasicBlock &BB) {
    auto collectForParallelBlock = [&BB, &ParallelLoops](ParallelBlock &PB,
                                                         auto Anchor) {
      for (auto &PI : PB)
        if (auto *Parallel{dyn_cast<PragmaParallel>(PI.get())})
          for (auto &I : Parallel->getClauses().get<trait::Induction>())
            ParallelLoops.try_emplace(I.get<VariableT>().get<MD>())
                .first->second.push_back(&BB);
    };
    if (auto ParallelItr{mParallelizationInfo.find(&BB)};
        ParallelItr != mParallelizationInfo.end())
      for (auto &PL : ParallelItr->get<ParallelLocation>()) {
        collectForParallelBlock(PL.Entry, PL.Anchor);
        collectForParallelBlock(PL.Exit, PL.Anchor);
      }
  };
  auto &TLI{getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F)};
  auto RF{Socket.getAnalysis<DIEstimateMemoryPass>(F)};
  if (!RF)
    return false;
  auto &ServerDIAT{RF->value<DIEstimateMemoryPass *>()->getAliasTree()};
  SpanningTreeRelation<const DIAliasTree *> ServerSTR{&ServerDIAT};
  auto addGetActualIf =
      [this, &ServerSTR](Instruction &I, const DIAliasNode *AliasWith,
                         SmallPtrSetImpl<const DIAliasNode *> &InsertedList) {
        for (auto &Data : mToGetActual) {
          if (InsertedList.count(Data.get<DIAliasNode>()) ||
              ServerSTR.isUnreachable(AliasWith, Data.get<DIAliasNode>()))
            continue;
          InsertedList.insert(Data.get<DIAliasNode>());
          auto GetActualRef{mParallelizationInfo.emplace<PragmaGetActual>(
              I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
              true /*IsFinal*/)};
          for (auto &Var : Data.get<VariableT>())
            cast<PragmaGetActual>(GetActualRef)->getMemory().insert(Var);
          for (auto *PL : mReplacementFor[std::min<unsigned>(
                   Data.get<Hierarchy>(), mReplacementFor.size() - 1)]) {
            PL->child_insert(GetActualRef.getUnchecked());
            GetActualRef->parent_insert(PL);
          }
        }
      };
  auto addTransferToWrite =
      [this, &ServerSTR, &addGetActualIf](
          Instruction &I, const DIAliasNode *CurrentAN,
          SmallPtrSetImpl<const DIAliasNode *> &InsertedActuals,
          SmallPtrSetImpl<const DIAliasNode *> &InsertedGetActuals) {
        for (auto &Data : mToActual) {
          if (InsertedGetActuals.count(Data.get<DIAliasNode>()) ||
              ServerSTR.isUnreachable(CurrentAN, Data.get<DIAliasNode>()))
            continue;
          InsertedActuals.insert(Data.get<DIAliasNode>());
          auto ActualRef{mParallelizationInfo.emplace<PragmaActual>(
              I.getParent(), &I, false /*OnEntry*/, false /*IsRequired*/,
              true /*IsFinal*/)};
          for (auto &Var : Data.get<VariableT>())
            cast<PragmaActual>(ActualRef)->getMemory().insert(Var);
          for (auto *PL : mReplacementFor[std::min<unsigned>(
                   Data.get<Hierarchy>(), mReplacementFor.size() - 1)]) {
            PL->child_insert(ActualRef.getUnchecked());
            ActualRef->parent_insert(PL);
          }
          addGetActualIf(I, Data.get<DIAliasNode>(), InsertedGetActuals);
        }
      };
  auto processBB = [this, Level, &ParallelLoops, &TLI, &CSMemoryMatcher,
                    &Provider, &addGetActualIf,
                    &addTransferToWrite](BasicBlock &BB) {
    auto &LI{Provider.value<LoopInfoWrapperPass *>()->getLoopInfo()};
    // Process blocks which are only nested explicitly in the current level.
    if (auto *L{LI.getLoopFor(&BB)};
        !(!L && Level.is<Function *>() || L && Level.getOpaqueValue() == L))
      return;
    for (auto &I : BB) {
      SmallPtrSet<const DIAliasNode *, 1> InsertedActuals, InsertedGetActuals;
      for_each_memory(
          I, TLI,
          [this, &Level, &ParallelLoops, &Provider, &CSMemoryMatcher,
           &addGetActualIf, &addTransferToWrite, &InsertedActuals,
           &InsertedGetActuals](Instruction &I, MemoryLocation &&Loc,
                                unsigned OpIdx, AccessInfo IsRead,
                                AccessInfo IsWrite) {
            if (IsRead == AccessInfo::No && IsWrite == AccessInfo::No)
              return;
            auto &AT{Provider.value<EstimateMemoryPass *>()->getAliasTree()};
            auto &DIAT{
                Provider.value<DIEstimateMemoryPass *>()->getAliasTree()};
            auto &DT{
                Provider.value<DominatorTreeWrapperPass *>()->getDomTree()};
            auto &F{*I.getFunction()};
            const auto &DL{F.getParent()->getDataLayout()};
            auto *EM{AT.find(Loc)};
            assert(EM && "Estimate memory must be presented in alias tree!");
            auto RawDIM{getRawDIMemoryIfExists(*EM->getTopLevelParent(),
                                               F.getContext(), DL, DT)};
            if (!RawDIM) {
              for (auto *AN : mDistinctMemory) {
                if (IsWrite != AccessInfo::No)
                  addTransferToWrite(I, AN, InsertedActuals,
                                     InsertedGetActuals);
                if (IsRead != AccessInfo::No)
                  addGetActualIf(I, AN, InsertedGetActuals);
              }
              return;
            }
            auto DIMItr{DIAT.find(*RawDIM)};
            assert(DIMItr != DIAT.memory_end() &&
                   "Existing memory must be presented in metadata alias tree.");
            auto anyLoopPostDominates =
                [&ParallelLoops,
                 &PDT = Provider.value<PostDominatorTreeWrapperPass *>()
                            ->getPostDomTree()](DIMemory *DIM,
                                                const BasicBlock *WhatBB) {
                  if (auto ParallelItr{ParallelLoops.find(DIM)};
                      ParallelItr != ParallelLoops.end())
                    for (auto *BB : ParallelItr->second)
                      if (PDT.dominates(BB, WhatBB))
                        return true;
                  return false;
                };
            auto CSMemoryMatchItr{CSMemoryMatcher.find<Origin>(&*DIMItr)};
            DIAliasNode *CurrentAN{
                CSMemoryMatchItr->get<Clone>()->getAliasNode()};
            if (IsWrite != AccessInfo::No) {
              // Do not transfer induction variables explicitly if there is a
              // parallel loop which post-dominates induction variable access.
              if (anyLoopPostDominates(&*DIMItr, I.getParent()))
                return;
              addTransferToWrite(I, CurrentAN, InsertedActuals,
                                 InsertedGetActuals);
            }
            if (IsRead != AccessInfo::No)
              addGetActualIf(I, CurrentAN, InsertedGetActuals);
          },
          [this, &Provider, &CSMemoryMatcher, &addGetActualIf,
           &addTransferToWrite, &InsertedActuals, &InsertedGetActuals](
              Instruction &I, AccessInfo IsRead, AccessInfo IsWrite) {
            if (IsRead == AccessInfo::No && IsWrite == AccessInfo::No)
              return;
            auto &AT{Provider.value<EstimateMemoryPass *>()->getAliasTree()};
            auto *AN{AT.findUnknown(I)};
            if (!AN)
              return;
            auto &DIAT{
                Provider.value<DIEstimateMemoryPass *>()->getAliasTree()};
            auto &DT{
                Provider.value<DominatorTreeWrapperPass *>()->getDomTree()};
            auto RawDIM{getRawDIMemoryIfExists(I, I.getContext(), DT)};
            if (!RawDIM) {
              for (auto *AN : mDistinctMemory) {
                if (IsWrite != AccessInfo::No)
                  addTransferToWrite(I, AN, InsertedActuals,
                                     InsertedGetActuals);
                if (IsRead != AccessInfo::No)
                  addGetActualIf(I, AN, InsertedGetActuals);
              }
              return;
            }
            auto DIMItr{DIAT.find(*RawDIM)};
            assert(DIMItr != DIAT.memory_end() &&
                   "Existing memory must be presented in metadata alias tree.");
            auto CSMemoryMatchItr{CSMemoryMatcher.find<Origin>(&*DIMItr)};
            DIAliasNode *CurrentAN{
                CSMemoryMatchItr->get<Clone>()->getAliasNode()};
            if (IsWrite != AccessInfo::No)
              addTransferToWrite(I, CurrentAN, InsertedActuals,
                                 InsertedGetActuals);
            if (IsRead != AccessInfo::No)
              addGetActualIf(I, CurrentAN, InsertedGetActuals);
          });
    }
  };
  if (Level.is<Function *>()) {
    for (auto &BB : *Level.get<Function *>())
      collectParallelInductions(BB);
    // We conservatively actualize memory, which is available outside the
    // function, at the function entry point.
    // TODO (kaniandr@gmail.com): move `actual` directives to callers.
    if (!mReplacementFor.back().empty()) {
      auto &EntryBB{F.getEntryBlock()};
      auto ActualRef{mParallelizationInfo.emplace<PragmaActual>(
          &EntryBB, &F, true /*OnEntry*/, false /*IsRequired*/,
          true /*IsFinal*/)};
      for (auto &Data : mToActual)
        for (auto &Var : Data.get<VariableT>()) {
          if (auto *DIEM{dyn_cast<DIEstimateMemory>(Var.get<MD>())}) {
            auto *V{DIEM->getVariable()};
            if (isa<DIGlobalVariable>(V) ||
                cast<DILocalVariable>(V)->isParameter())
              cast<PragmaActual>(ActualRef)->getMemory().insert(Var);
          } else {
              cast<PragmaActual>(ActualRef)->getMemory().insert(Var);
          }
        }
      SmallVector<ParallelItem *, 2> ConservativeReplacements;
      if (!cast<PragmaActual>(ActualRef)->getMemory().empty())
        ConservativeReplacements.push_back(ActualRef.getUnchecked());
      // We conservatively copy memory from the device before the exit from the
      // function.
      // TODO (kaniandr@gmail.com): move `get_actual` directives to callers.
      for (auto &I : instructions(F)) {
        if (!isa<ReturnInst>(I))
          continue;
        auto GetActualRef{mParallelizationInfo.emplace<PragmaGetActual>(
            I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
            true /*IsFinal*/)};
        for (auto &Data : mToGetActual)
          for (auto &Var : Data.get<VariableT>())
            if (auto *DIEM{dyn_cast<DIEstimateMemory>(Var.get<MD>())}) {
              auto *V{DIEM->getVariable()};
              if (isa<DIGlobalVariable>(V) ||
                  cast<DILocalVariable>(V)->isParameter())
                cast<PragmaGetActual>(GetActualRef)->getMemory().insert(Var);
            } else {
              cast<PragmaGetActual>(GetActualRef)->getMemory().insert(Var);
            }
        if (!cast<PragmaGetActual>(GetActualRef)->getMemory().empty())
          ConservativeReplacements.push_back(GetActualRef.getUnchecked());
      }
      if (Level.get<Function *>() == getEntryPoint())
        for (auto *PI : ConservativeReplacements)
          cast<PragmaData>(PI)->skip();
      // The created directives are necessary to remove optimized ones. So, we
      // update replacement relation.
      for (auto *PL : mReplacementFor.back())
        for (auto *ToReplace : ConservativeReplacements) {
          PL->child_insert(ToReplace);
          ToReplace->parent_insert(PL);
        }
    }
    // Process blocks inside the function and add `actual/get_actual` to
    // instructions which access memory.
    for (auto &BB : *Level.get<Function *>())
      processBB(BB);
  } else {
    for (auto *BB : Level.get<Loop *>()->blocks())
      collectParallelInductions(*BB);
    if (mReplacementFor.back().empty()) {
      auto HeaderBB = Level.get<Loop *>()->getHeader();
      // We conservatively copy memory to the device, on the entry to the loop.
      SmallVector<ParallelItem *, 2> ConservativeReplacements;
      for (auto *BB : children<Inverse<BasicBlock *>>(HeaderBB)) {
        if (Level.get<Loop *>()->contains(BB))
          continue;
        auto ActualRef{mParallelizationInfo.emplace<PragmaActual>(
            BB, BB->getTerminator(), true /*OnEntry*/, false /*IsRequired*/)};
        for (auto &Data : mToActual)
          for (auto &Var : Data.get<VariableT>())
            cast<PragmaActual>(ActualRef)->getMemory().insert(Var);
        if (!cast<PragmaActual>(ActualRef)->getMemory().empty())
          ConservativeReplacements.push_back(ActualRef.getUnchecked());
      }
      // We conservatively copy memory from the device on the exit from the
      // loop.
      SmallVector<BasicBlock *, 4> ExitBlocks;
      Level.get<Loop *>()->getExitBlocks(ExitBlocks);
      for (auto *BB : ExitBlocks) {
        auto Inst{BB->begin()};
        for (; Inst->mayReadFromMemory() || &*Inst != BB->getTerminator();
             ++Inst)
          ;
        auto GetActualRef{mParallelizationInfo.emplace<PragmaGetActual>(
            BB, &*Inst, true /*OnEntry*/, false /*IsRequired*/)};
        for (auto &Data : mToGetActual)
          for (auto &Var : Data.get<VariableT>())
            cast<PragmaGetActual>(GetActualRef)->getMemory().insert(Var);
        if (!cast<PragmaGetActual>(GetActualRef)->getMemory().empty())
          ConservativeReplacements.push_back(GetActualRef.getUnchecked());
      }
      // The created directives are necessary to remove optimized ones. So, we
      // update replacement relation.
      for (auto *PL : mReplacementFor.back())
        for (auto *ToReplace : ConservativeReplacements) {
          PL->child_insert(ToReplace);
          ToReplace->parent_insert(PL);
        }
    }
    // Process blocks inside the function and add `actual/get_actual` to
    // instructions which access memory.
    for (auto *BB : Level.get<Loop *>()->blocks())
      processBB(*BB);
  }
  mReplacementFor.pop_back();
  return true;
}

static inline void addVarList(
    const std::set<std::string> &VarInfoList,
    SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I{ VarInfoList.begin() }, EI{ VarInfoList.end() };
  Clause.append(I->begin(), I->end());
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    Clause.append(I->begin(), I->end());
  }
  Clause.push_back(')');
}

static inline unsigned addVarList(const SortedVarListT &VarInfoList,
    SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto name = [](auto &V) { return V.template get<AST>()->getName(); };
  auto I{VarInfoList.begin()}, EI{VarInfoList.end()};
  auto N{I->template get<AST>()->getName()};
  Clause.append(N.begin(), N.end());
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    auto N{I->template get<AST>()->getName()};
    Clause.append(N.begin(), N.end());
  }
  Clause.push_back(')');
  return Clause.size();
}

template <typename FilterT>
static inline unsigned addVarList(const SortedVarListT &VarInfoList, FilterT &&F,
    SmallVectorImpl<char> &Clause) {
  unsigned Count{0};
  Clause.push_back('(');
  auto Itr{VarInfoList.begin()}, ItrE{VarInfoList.end()};
  for (; Itr != ItrE && !F(*Itr); ++Itr)
    ;
  if (Itr == ItrE) {
    Clause.push_back(')');
    return Count;
  }
  auto Name{Itr->get<AST>()->getName()};
  Clause.append(Name.begin(), Name.end());
  ++Count;
  for (++Itr; Itr != ItrE; ++Itr)
    if (F(*Itr)) {
      Clause.append({',', ' '});
      auto Name{Itr->get<AST>()->getName()};
      Clause.append(Name.begin(), Name.end());
      ++Count;
    }
  Clause.push_back(')');
  return Count;
}

static void addParallelMapping(Loop &L, const PragmaParallel &Parallel,
    const FunctionAnalysis &Provider, SmallVectorImpl<char> &PragmaStr) {
  if (Parallel.getClauses().get<trait::DirectAccess>().empty())
    return;
  auto &CL = Provider.value<CanonicalLoopPass *>()->getCanonicalLoopInfo();
  auto &RI = Provider.value<DFRegionInfoPass *>()->getRegionInfo();
  auto &MemoryMatcher =
      Provider.value<MemoryMatcherImmutableWrapper *>()->get();
  SmallVector<std::pair<ObjectID, StringRef>, 4> Inductions;
  getBaseInductionsForNest(L,
                           Parallel.getClauses().get<trait::Induction>().size(),
                           CL, RI, MemoryMatcher, Inductions);
  assert(Inductions.size() ==
             Parallel.getClauses().get<trait::Induction>().size() &&
    "Unable to find induction variable for some of the loops in a parallel nest!");
  PragmaStr.push_back('(');
  for (auto &LToI : Inductions) {
    PragmaStr.push_back('[');
    PragmaStr.append(LToI.second.begin(), LToI.second.end());
    PragmaStr.push_back(']');
  }
  PragmaStr.push_back(')');
  // We sort arrays to ensure the same order of variables after
  // different launches of parallelization.
  std::set<std::string, std::less<std::string>> MappingStr;
  for (auto &[Var, Mapping] : Parallel.getClauses().get<trait::DirectAccess>()) {
    if (Mapping.empty())
      continue;
    SmallString<32> Tie{Var.get<AST>()->getName()};
    for (auto &Map : Mapping) {
      Tie += "[";
      if (Map.first) {
        if (!Map.second)
          Tie += "-";
        Tie += find_if(Inductions, [&Map](auto &LToI) {
                 return LToI.first == Map.first;
               })->second;
      }
      Tie += "]";
    }
    MappingStr.insert(std::string(Tie));
  }
  if (MappingStr.empty())
    return;
  PragmaStr.append({ ' ', 't', 'i', 'e' });
  addVarList(MappingStr, PragmaStr);
}

static inline void addClauseIfNeed(StringRef Name, const SortedVarListT &Vars,
    SmallVectorImpl<char> &PragmaStr) {
  if (!Vars.empty()) {
    PragmaStr.append(Name.begin(), Name.end());
    addVarList(Vars, PragmaStr);
  }
}

/// Add clauses for all reduction variables from a specified list to
/// the end of `ParallelFor` pragma.
static void addReductionIfNeed(
    const ClangDependenceAnalyzer::ReductionVarListT &VarInfoList,
    SmallVectorImpl<char> &ParallelFor) {
  unsigned I = trait::Reduction::RK_First;
  unsigned EI = trait::Reduction::RK_NumberOf;
  for (; I < EI; ++I) {
    if (VarInfoList[I].empty())
      continue;
    SmallString<7> RedKind;
    switch (static_cast<trait::Reduction::Kind>(I)) {
    case trait::Reduction::RK_Add: RedKind += "sum"; break;
    case trait::Reduction::RK_Mult: RedKind += "product"; break;
    case trait::Reduction::RK_Or: RedKind += "or"; break;
    case trait::Reduction::RK_And: RedKind += "and"; break;
    case trait::Reduction::RK_Xor: RedKind += "xor"; break;
    case trait::Reduction::RK_Max: RedKind += "max"; break;
    case trait::Reduction::RK_Min: RedKind += "min"; break;
    default: llvm_unreachable("Unknown reduction kind!"); break;
    }
    ParallelFor.append({ 'r', 'e', 'd', 'u', 'c', 't', 'i', 'o', 'n' });
    ParallelFor.push_back('(');
    auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
    ParallelFor.append(RedKind.begin(), RedKind.end());
    ParallelFor.push_back('(');
    auto VarName{ VarItr->get<AST>()->getName() };
    ParallelFor.append(VarName.begin(), VarName.end());
    ParallelFor.push_back(')');
    for (++VarItr; VarItr != VarItrE; ++VarItr) {
      ParallelFor.push_back(',');
      ParallelFor.append(RedKind.begin(), RedKind.end());
      ParallelFor.push_back('(');
      auto VarName{ VarItr->get<AST>()->getName() };
      ParallelFor.append(VarName.begin(), VarName.end());
      ParallelFor.push_back(')');
    }
    ParallelFor.push_back(')');
  }
}

static inline clang::SourceLocation
shiftTokenIfSemi(clang::SourceLocation Loc, const clang::ASTContext &Ctx) {
  Token SemiTok;
  return (!getRawTokenAfter(Loc, Ctx.getSourceManager(), Ctx.getLangOpts(),
                            SemiTok) &&
          SemiTok.is(tok::semi))
             ? SemiTok.getLocation()
             : Loc;
}

static std::pair<clang::Stmt *, PointerUnion<llvm::Loop *, clang::Decl *>>
findLocationToInsert(Parallelization::iterator PLocListItr,
    Parallelization::location_iterator PLocItr, const Function &F, LoopInfo &LI,
    TransformationContext &TfmCtx, const ClangExprMatcherPass::ExprMatcher &EM,
    const LoopMatcherPass::LoopMatcher &LM) {
  if (PLocItr->Anchor.is<MDNode *>()) {
    auto *L{LI.getLoopFor(PLocListItr->get<BasicBlock>())};
    auto ID{PLocItr->Anchor.get<MDNode *>()};
    while (L->getLoopID() && L->getLoopID() != ID)
      L = L->getParentLoop();
    assert(L &&
      "A parallel directive has been attached to an unknown loop!");
    auto LMatchItr{LM.find<IR>(L)};
    assert(LMatchItr != LM.end() &&
      "Unable to find AST representation for a loop!");
    return std::pair{LMatchItr->get<AST>(), L};
  }
  assert(PLocItr->Anchor.is<Value *>() &&
         "Directives must be attached to llvm::Value!");
  if (isa<Function>(PLocItr->Anchor.get<Value *>())) {
    auto *FD{TfmCtx.getDeclForMangledName(F.getName())};
    assert(FD && "AST representation of a function must be available!");
    return std::pair{*FD->getBody()->child_begin(), FD};
  }
  auto MatchItr{EM.find<IR>(PLocItr->Anchor.get<Value *>())};
  if (MatchItr == EM.end())
    return std::pair{nullptr, nullptr};
  auto &ParentCtx{TfmCtx.getContext().getParentMapContext()};
  auto skipDecls = [&ParentCtx](auto Current) -> const Stmt * {
    for (; !Current.template get<DeclStmt>();) {
      auto Parents{ParentCtx.getParents(Current)};
      assert(!Parents.empty() &&
             "Declaration must be in declaration statement!");
      Current = *Parents.begin();
    }
    return &Current.template getUnchecked<DeclStmt>();
  };
  auto Current{MatchItr->get<AST>()};
  if (auto *D{Current.get<Decl>()})
    return std::pair{const_cast<Stmt *>(skipDecls(Current)),
                     const_cast<Decl *>(D)};
  Stmt *ToInsert{nullptr};
  if (auto *ParentStmt{Current.get<Stmt>()}) {
    for (;;) {
      ToInsert = const_cast<Stmt *>(ParentStmt);
      auto Parents{ParentCtx.getParents(*ParentStmt)};
      assert(!Parents.empty() &&
             (Parents.begin()->get<Stmt>() || Parents.begin()->get<Decl>()) &&
             "Executable statement must be in compound statement!");
      ParentStmt = Parents.begin()->get<Decl>()
                       ? skipDecls(*Parents.begin())
                       : &Parents.begin()->getUnchecked<Stmt>();
      if (isa<CompoundStmt>(ParentStmt))
        break;
      if (auto If{dyn_cast<IfStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives from Entry
        // attached to condition before the `if-stmt` and insert
        // directives from Exit at the beginning of each branch and
        // after if-stmt (if there is no `else` branch.
        if (If->getCond() == ToInsert ||
            If->getConditionVariableDeclStmt() == ToInsert)
          ToInsert = nullptr;
        break;
      }
      if (auto For{dyn_cast<ForStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives attached to another
        // parts of loops
        if (For->getBody() != ToInsert)
          ToInsert = nullptr;
        break;
      }
      if (auto While{dyn_cast<WhileStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives attached to another
        // parts of loops
        if (While->getBody() != ToInsert)
          ToInsert = nullptr;
        break;
      }
      if (auto Do{dyn_cast<DoStmt>(ParentStmt)}) {
        // TODO (kaniandr@gmail.com): insert directives attached to another
        // parts of loops
        if (Do->getBody() != ToInsert)
          ToInsert = nullptr;
        break;
      }
    }
  }
  return std::pair{ToInsert, nullptr};
}

static bool tryToIgnoreDirectives(Parallelization::iterator PLocListItr,
    Parallelization::location_iterator PLocItr) {
  for (auto PIItr{PLocItr->Entry.begin()}, PIItrE{PLocItr->Entry.end()};
       PIItr != PIItrE; ++PIItr) {
    if (auto *PD{dyn_cast<PragmaData>(PIItr->get())}) {
      // Some data directives could be redundant.
      // So, we will emit errors later when redundant directives are
      // already ignored.
      if (!PD->isRequired()) {
        PD->invalidate();
        continue;
      }
    }
    // TODO: (kaniandr@gmail.com): emit error
    LLVM_DEBUG(dbgs() << "ERROR: unable to insert on entry: "
                      << getName(static_cast<DirectiveId>((**PIItr).getKind()))
                      << "\n");
    return false;
  }
  for (auto PIItr{PLocItr->Exit.begin()}, PIItrE{PLocItr->Exit.end()};
       PIItr != PIItrE; ++PIItr) {
    if (auto *PD{dyn_cast<PragmaData>(PIItr->get())}) {
      // Some data directives could be redundant.
      // So, we will emit errors later when redundant directives are
      // already ignored.
      if (!PD->isRequired()) {
        PD->invalidate();
        continue;
      }
    }
    // TODO: (kaniandr@gmail.com): emit error
    LLVM_DEBUG(dbgs() << "ERROR: unable to insert on exit: "
                      << getName(static_cast<DirectiveId>((**PIItr).getKind()))
                      << "\n");
    return false;
  }
  return true;
}

static void pragmaParallelStr(const ParallelItemRef &PIRef, Loop &L,
    const FunctionAnalysis &Provider, SmallVectorImpl<char> &Str) {
  auto Parallel{cast<PragmaParallel>(PIRef)};
  getPragmaText(DirectiveId::DvmParallel, Str);
  Str.resize(Str.size() - 1);
  if (Parallel->getClauses().get<trait::DirectAccess>().empty()) {
    Str.push_back('(');
    auto NestSize{
        std::to_string(Parallel->getClauses().get<trait::Induction>().size())};
    Str.append(NestSize.begin(), NestSize.end());
    Str.push_back(')');
  } else {
    addParallelMapping(L, *Parallel, Provider, Str);
  }
  if (!Parallel->getClauses().get<trait::Dependence>().empty()) {
    Str.append({'a', 'c', 'r', 'o', 's', 's', '('});
    for (auto &Across : Parallel->getClauses().get<trait::Dependence>()) {
      Str.append(Across.first.get<AST>()->getName().begin(),
                 Across.first.get<AST>()->getName().end());
      for (auto &Range : Across.second) {
        Str.push_back('[');
        if (Range.first)
          Range.first->toString(Str);
        else
          Str.push_back('0');
        Str.push_back(':');
        if (Range.second)
          Range.second->toString(Str);
        else
          Str.push_back('0');
        Str.push_back(']');
      }
    }
    Str.push_back(')');
  }
  addClauseIfNeed(" private", Parallel->getClauses().get<trait::Private>(),
                  Str);
  addReductionIfNeed(Parallel->getClauses().get<trait::Reduction>(), Str);
}

static void pragmaRegionStr(const ParallelItemRef &PIRef,
                            SmallVectorImpl<char> &Str) {
  auto R{cast<PragmaRegion>(PIRef)};
  getPragmaText(DirectiveId::DvmRegion, Str);
  Str.resize(Str.size() - 1);
  addClauseIfNeed(" in", R->getClauses().get<trait::ReadOccurred>(), Str);
  addClauseIfNeed(" out", R->getClauses().get<trait::WriteOccurred>(), Str);
  addClauseIfNeed(" local", R->getClauses().get<trait::Private>(), Str);
  if (R->isHostOnly()) {
    StringRef Targets{" targets(HOST)"};
    Str.append(Targets.begin(), Targets.end());
  }
  Str.push_back('\n');
  Str.push_back('{');
}

template <typename FilterT>
static bool pragmaDataStr(FilterT Filter, const ParallelItemRef &PDRef,
    SmallVectorImpl<char> &Str) {
  auto PD{cast_or_null<PragmaData>(PDRef)};
  if (!PD || PD->getMemory().empty())
    return false;
  getPragmaText(static_cast<DirectiveId>(PD->getKind()), Str);
  // Remove the last '\n'.
  Str.pop_back();
  if constexpr (std::is_same_v<decltype(Filter), std::true_type>)
    addVarList(PD->getMemory(), Str);
  else if (addVarList(PD->getMemory(), std::move(Filter), Str) == 0)
    return false;
  return true;
}

static inline bool pragmaDataStr(ParallelItemRef &PDRef,
    SmallVectorImpl<char> &Str) {
  return pragmaDataStr(std::true_type{}, PDRef, Str);
}

static void
addPragmaToStmt(const Stmt *ToInsert,
                PointerUnion<llvm::Loop *, clang::Decl *> Scope,
                Parallelization::iterator PLocListItr,
                Parallelization::location_iterator PLocItr,
                const FunctionAnalysis &Provider,
                DenseMap<ParallelItemRef, const Stmt *> &DeferredPragmas,
                std::vector<ParallelItem *> &NotOptimizedPragmas,
                LocationToPragmas &PragmasToInsert) {
  auto PragmaLoc{ToInsert ? PragmasToInsert.try_emplace(ToInsert).first
                          : PragmasToInsert.end()};
  for (auto PIItr{PLocItr->Entry.begin()}, PIItrE{PLocItr->Entry.end()};
       PIItr != PIItrE; ++PIItr) {
    ParallelItemRef PIRef{PLocListItr, PLocItr, PIItr, true};
    SmallString<128> PragmaStr;
    if (isa<PragmaParallel>(PIRef)) {
      pragmaParallelStr(PIRef, *Scope.get<Loop *>(), Provider, PragmaStr);
    } else if (isa<PragmaRegion>(PIRef)) {
      pragmaRegionStr(PIRef, PragmaStr);
    } else if (auto *PD{dyn_cast<PragmaData>(PIRef)}) {
      // Even if this directive cannot be inserted (it is invalid) it should
      // be processed later. If it is replaced with some other directives,
      // this directive changes status to CK_Skip. The new status may allow us
      // to ignore some other directives later.
      DeferredPragmas.try_emplace(PIRef, ToInsert);
      if (PD->parent_empty())
        NotOptimizedPragmas.push_back(PD);
      // Mark the position of the directive in the source code. It will be later
      // created their only if necessary.
      if (ToInsert)
        PragmaLoc->second.Before.emplace_back(PIItr->get(), "");
      continue;
    } else {
      llvm_unreachable("An unknown pragma has been attached to a loop!");
    }
    PragmaStr += "\n";
    assert(ToInsert && "Insertion location must be known!");
    PragmaLoc->second.Before.emplace_back(PIItr->get(), std::move(PragmaStr));
  }
  if (PLocItr->Exit.empty())
    return;
  for (auto PIItr{PLocItr->Exit.begin()}, PIItrE{PLocItr->Exit.end()};
       PIItr != PIItrE; ++PIItr) {
    ParallelItemRef PIRef{PLocListItr, PLocItr, PIItr, false};
    SmallString<128> PragmaStr{"\n"};
    if (auto *PD{dyn_cast<PragmaData>(PIRef)}) {
      DeferredPragmas.try_emplace(PIRef, ToInsert);
      if (PD->parent_empty())
        NotOptimizedPragmas.push_back(PD);
      if (ToInsert)
        PragmaLoc->second.After.emplace_back(PIItr->get(), "");
      continue;
    } else if (auto *Marker{dyn_cast<ParallelMarker<PragmaRegion>>(PIRef)}) {
      PragmaStr = "}";
    } else {
      llvm_unreachable("An unknown pragma has been attached to a loop!");
    }
    assert(ToInsert && "Insertion location must be known!");
    PragmaLoc->second.After.emplace_back(PIItr->get(), std::move(PragmaStr));
  }
}

template<typename VisitorT>
static void traversePragmaDataPO(ParallelItem *PI,
    SmallPtrSetImpl<ParallelItem *> &Visited, VisitorT &&POVisitor) {
  if (!Visited.insert(PI).second)
    return;
  if (auto *PD{dyn_cast<PragmaData>(PI)}) {
    for (auto *Child : PD->children())
      traversePragmaDataPO(Child, Visited, std::forward<VisitorT>(POVisitor));
  }
  POVisitor(PI);
}

// This implements a post-ordered traversal of a forest of data transfer
// directives and applies a specified function to an each directive being
// visited.
template<typename VisitorT>
static inline void traversePragmaDataPO(ArrayRef<ParallelItem *> Roots,
    VisitorT &&POVisitor) {
  SmallPtrSet<ParallelItem *, 32> Visited;
  for (auto *PI : Roots)
    traversePragmaDataPO(PI, Visited, std::forward<VisitorT>(POVisitor));
}

static void
insertPragmaData(ArrayRef<PragmaData *> POTraverse,
                 DenseMap<ParallelItemRef, const Stmt *> DeferredPragmas,
                 LocationToPragmas &PragmasToInsert) {
  for (auto *PD : POTraverse) {
    // Do not skip directive (even if it is marked as skipped) if its children
    // are invalid.
    if (PD->isInvalid() ||
        (PD->isSkipped() &&
         (PD->child_empty() ||
          !cast<PragmaData>(*PD->child_begin())->isInvalid() ||
          cast<PragmaData>(*PD->child_begin())->isSkipped())))
      continue;
    auto &&[PIRef, ToInsert] = *DeferredPragmas.find_as(PD);
    assert(PragmasToInsert.count(ToInsert) &&
           "Pragma position must be cached!");
    auto &Position{PragmasToInsert[ToInsert]};
    if (PIRef.isOnEntry()) {
      auto BeforeItr{
          find_if(Position.Before, [PI = PIRef.getPI()->get()](auto &Pragma) {
            return std::get<ParallelItem *>(Pragma) == PI;
          })};
      assert(BeforeItr != Position.Before.end() &&
             "Pragma position must be cached!");
      auto &PragmaStr{std::get<Insertion::PragmaString>(*BeforeItr)};
      if (auto *DS{dyn_cast<DeclStmt>(ToInsert)})
        pragmaDataStr(
            [DS](const VariableT &V) {
              return !is_contained(DS->getDeclGroup(), V.get<AST>());
            },
            PIRef, PragmaStr);
      else
        pragmaDataStr(PIRef, PragmaStr);
      PragmaStr += "\n";
    } else {
      auto AfterItr{
          find_if(Position.After, [PI = PIRef.getPI()->get()](auto &Pragma) {
            return std::get<ParallelItem *>(Pragma) == PI;
          })};
      assert(AfterItr != Position.After.end() &&
             "Pragma position must be cached!");
      auto &PragmaStr{std::get<Insertion::PragmaString>(*AfterItr)};
      PragmaStr += "\n";
      pragmaDataStr(PIRef, PragmaStr);
    }
  }
}

bool ClangDVMHSMParallelization::runOnModule(llvm::Module &M) {
  ClangSMParallelization::runOnModule(M);
  auto *TfmCtx{getAnalysis<TransformationEnginePass>()->getContext(M)};
  for (auto F : make_range(mParallelizationInfo.func_begin(),
                           mParallelizationInfo.func_end())) {
    auto Provider{analyzeFunction(*F)};
    auto &LI{Provider.value<LoopInfoWrapperPass *>()->getLoopInfo()};
    auto &LM{Provider.value<LoopMatcherPass *>()->getMatcher()};
    auto &EM{Provider.value<ClangExprMatcherPass *>()->getMatcher()};
    LocationToPragmas PragmasToInsert;
    DenseMap<ParallelItemRef, const Stmt *> DeferredPragmas;
    std::vector<ParallelItem *> NotOptimizedPragmas;
    for (auto &BB : *F) {
      auto PLocListItr{mParallelizationInfo.find(&BB)};
      if (PLocListItr == mParallelizationInfo.end())
        continue;
      for (auto PLocItr{PLocListItr->get<ParallelLocation>().begin()},
           PLocItrE{PLocListItr->get<ParallelLocation>().end()};
           PLocItr != PLocItrE; ++PLocItr) {
        auto [ToInsert, Scope] =
            findLocationToInsert(PLocListItr, PLocItr, *F, LI, *TfmCtx, EM, LM);
        if (!ToInsert && !tryToIgnoreDirectives(PLocListItr, PLocItr))
          return false;
        addPragmaToStmt(ToInsert, Scope, PLocListItr, PLocItr, Provider,
                        DeferredPragmas, NotOptimizedPragmas, PragmasToInsert);
      }
    }
    std::vector<PragmaData *> POTraverse;
    // Optimize CPU-to-GPU data transfer. Try to skip unnecessary directives.
    // We use post-ordered traversal to propagate the `skip` property in upward
    // direction.
    traversePragmaDataPO(NotOptimizedPragmas, [&DeferredPragmas, F, TfmCtx, &LI,
                                               &EM, &LM,
                                               &POTraverse](ParallelItem *PI) {
      auto PD{cast<PragmaData>(PI)};
      POTraverse.push_back(PD);
      if (PD->isSkipped())
        return;
      bool IsRedundant{true};
      for (auto *Child : PD->children()) {
        auto ChildPD{cast<PragmaData>(Child)};
        IsRedundant &= (!ChildPD->isInvalid() || ChildPD->isSkipped());
      }
      if (IsRedundant && !PD->isFinal() && !PD->isRequired()) {
        PD->skip();
        return;
      }
      auto PIItr{DeferredPragmas.find_as(PI)};
      assert(PIItr != DeferredPragmas.end() &&
             "Internal pragmas must be cached!");
      auto [PIRef, ToInsert] = *PIItr;
      if (PD->getMemory().empty()) {
        PD->skip();
      } else if (PIRef.isOnEntry()) {
        // Do not mention variables in a directive if it has not been
        // declared yet.
        if (auto *DS{dyn_cast_or_null<DeclStmt>(ToInsert)};
            DS && all_of(PD->getMemory(), [DS](auto &V) {
              return is_contained(DS->getDeclGroup(), V.template get<AST>());
            }))
          PD->skip();
      }
    });
    bool IsOk{true};
    // Now we check that there is any way to actualize data for each parallel
    // region.
    for (auto *PI : NotOptimizedPragmas)
      if (cast<PragmaData>(PI)->isInvalid() &&
          !cast<PragmaData>(PI)->isSkipped()) {
        IsOk = false;
        // TODO: (kaniandr@gmail.com): emit error
      }
    if (!IsOk)
      return false;
    // We look up for the lowest valid levels in the forest of directives and
    // invalidate all directives below these levels.
    //
    // Let's consider the following not optimized yet example:
    //
    // function {
    //   actual
    //   region_1
    //   get_actual
    //   loop {
    //     actual
    //     region_2
    //     get_actual
    //   }
    // }
    // Optimization step produces the following forest of possible data transfer
    // directives:
    //              r2_a  r2_ga
    // r1_a r1_ga   l_a  l_ga  loop_internal_directives
    // f_a f_ga
    //
    // If we cannot insert some of directives from the
    // 'loop_internal_directives' list, we have to invalidate l_a, l_ga,
    // loop_internal_direcitves and also f_a and f_ga directives. Otherwise we
    // obtain the following optimized version which is not correct (we miss
    // directives surrounding the first region because f_a and f_ga suppress
    // them):
    // function {
    // f_a
    //   region_1
    //   loop {
    //     actual
    //     region_2
    //     get_actual
    //   }
    // f_ga
    // }
    for (auto *PD : llvm::reverse(POTraverse)) {
      // Stop optimization here if the current directive cannot be skipped.
      bool IsValid{PD->isSkipped()};
      if (IsValid)
        for (auto *Child : PD->children())
          IsValid &= (!cast<PragmaData>(Child)->isInvalid() ||
                      cast<PragmaData>(Child)->isSkipped());
      if (!IsValid) {
        for (auto *Child : PD->children()) {
          cast<PragmaData>(Child)->invalidate();
          cast<PragmaData>(Child)->actualize();
        }
      }
    }
    // Build pragmas for necessary data transfer directives.
    insertPragmaData(POTraverse, DeferredPragmas, PragmasToInsert);
    // Update sources.
    for (auto &&[ToInsert, Pragmas] : PragmasToInsert) {
      auto BeginLoc{ToInsert->getBeginLoc()};
      bool IsBeginChanged{false};
      for (auto &&[PI, Str] : Pragmas.Before)
        if (!Str.empty()) {
          if (!IsBeginChanged) {
            auto &SM{TfmCtx->getRewriter().getSourceMgr()};
            auto Identation{Lexer::getIndentationForLine(BeginLoc, SM)};
            bool Invalid{false};
            auto Column{SM.getPresumedColumnNumber(BeginLoc, &Invalid)};
            if (Invalid || Column > Identation.size() + 1)
              TfmCtx->getRewriter().InsertTextAfter(BeginLoc, "\n");
          }
          TfmCtx->getRewriter().InsertTextAfter(BeginLoc, Str);
          IsBeginChanged = true;
        }
      auto EndLoc{
          shiftTokenIfSemi(ToInsert->getEndLoc(), TfmCtx->getContext())};
      bool IsEndChanged{false};
      for (auto &&[PI, Str] : Pragmas.After)
        if (!Str.empty()) {
          TfmCtx->getRewriter().InsertTextAfterToken(EndLoc, Str);
          IsEndChanged = true;
        }
      if (IsBeginChanged || IsEndChanged) {
        bool HasEndNewline{false};
        if (IsEndChanged) {
          auto &Ctx{TfmCtx->getContext()};
          auto &SM{Ctx.getSourceManager()};
          Token NextTok;
          bool IsEndInvalid{false}, IsNextInvalid{false};
          if (getRawTokenAfter(EndLoc, SM, Ctx.getLangOpts(), NextTok) ||
              SM.getPresumedLineNumber(NextTok.getLocation(), &IsNextInvalid) ==
                  SM.getPresumedLineNumber(EndLoc, &IsEndInvalid) ||
              IsNextInvalid || IsEndInvalid) {
            TfmCtx->getRewriter().InsertTextAfterToken(EndLoc, "\n");
            HasEndNewline = true;
          }
        }
        auto &ParentCtx{TfmCtx->getContext().getParentMapContext()};
        auto Parents{ParentCtx.getParents(*ToInsert)};
        assert(!Parents.empty() && "Statement must be inside a function body!");
        if (!Parents.begin()->get<CompoundStmt>()) {
          TfmCtx->getRewriter().InsertTextBefore(BeginLoc, "{\n");
          TfmCtx->getRewriter().InsertTextAfterToken(
              EndLoc, HasEndNewline ? "}" : "\n}");
        }
      }
    }
  }
  return false;
}

ModulePass *llvm::createClangDVMHSMParallelization() {
  return new ClangDVMHSMParallelization;
}

char ClangDVMHSMParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(ClangDVMHSMParallelization,
  "clang-dvmh-sm-parallel", "Shared Memory DVMH-based Parallelization (Clang)")
