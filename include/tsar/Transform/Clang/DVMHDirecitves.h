//===- DVMHDirectives.h - Representation of DVMH Directives ------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file provides class to represent DVMH directives.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DVMH_DIRECTIVES_H
#define TSAR_DVMH_DIRECTIVES_H

#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Parallel/Parallellelization.h"
#include "tsar/Support/Directives.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Pass.h>
#include <map>
#include <variant>

namespace llvm {
class Loop;
class SCEVConstant;
class Instruction;
}

namespace tsar {
class DIArrayAccessInfo;

namespace dvmh {

struct Shadow {};
struct Corner {};
struct Remote {};

using DistanceInfo = ClangDependenceAnalyzer::DistanceInfo;
using VariableT = ClangDependenceAnalyzer::VariableT;
using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
using SortedVarMultiListT = ClangDependenceAnalyzer::SortedVarMultiListT;
using ShadowVarListT =
    std::map<VariableT,
             bcl::tagged_pair<bcl::tagged<trait::DIDependence::DistanceVector,
                                          trait::DIDependence::DistanceVector>,
                              bcl::tagged<bool, Corner>>,
             ClangDependenceAnalyzer::VariableLess>;
using InductionInfo =
    bcl::tagged_tuple<bcl::tagged<VariableT, trait::Induction>,
                      bcl::tagged<trait::DIInduction::Constant, Begin>,
                      bcl::tagged<trait::DIInduction::Constant, End>,
                      bcl::tagged<trait::DIInduction::Constant, Step>>;

class Template {
public:
  explicit Template(llvm::StringRef Name, Template *Origin = nullptr)
      : mName(Name), mOrigin(Origin) {}
  llvm::StringRef getName() const noexcept { return mName; }

  Template *getCloneOrCreate(llvm::StringRef Name) {
    auto I{mClones.insert(std::pair{Name, nullptr}).first};
    if (!I->second)
      I->second = std::make_unique<Template>(Name, this);
    return I->second.get();
  }

  Template *getOrigin() noexcept { return mOrigin; }
  const Template *getOrigin() const noexcept { return mOrigin; }

private:
  std::string mName;
  llvm::StringMap<std::unique_ptr<Template>> mClones;
  Template *mOrigin{nullptr};
};

struct Align {
  Align(const std::variant<VariableT, Template *> &T, unsigned NumberOfDims = 0)
      : Target(T), Relation(NumberOfDims) {}

  struct Axis {
    unsigned Dimension;
    llvm::APSInt Step;
    llvm::APSInt Offset;
  };
  std::variant<VariableT, Template *> Target;
  llvm::SmallVector<llvm::Optional<std::variant<Axis, llvm::APSInt>>, 4>
      Relation;
};

struct AlignLess {
  bool operator()(const Align &LHS, const Align &RHS) const {
    auto getName = [](const Align &A) {
      llvm::StringRef Name;
      std::visit(
          [&Name](auto &&V) {
            if constexpr (std::is_same_v<VariableT, std::decay_t<decltype(V)>>)
              Name = V.template get<AST>()->getName();
            else
              Name = V->getName();
          },
          A.Target);
      return Name;
    };
    auto LHSName{getName(LHS)};
    auto RHSName{getName(RHS)};
    if (LHSName != RHSName)
      return LHSName < RHSName;
    if (LHS.Relation.size() != RHS.Relation.size())
      return LHS.Relation.size() < RHS.Relation.size();
    for (unsigned I = 0, EI = LHS.Relation.size(); I < EI; ++I) {
      if (!LHS.Relation[I] && !RHS.Relation[I])
        continue;
      if (LHS.Relation[I] && !RHS.Relation[I])
        return false;
      if (RHS.Relation[I] && !LHS.Relation[I])
        return true;
      if (std::holds_alternative<llvm::APSInt>(*LHS.Relation[I]) &&
          !std::holds_alternative<llvm::APSInt>(*RHS.Relation[I]))
        return false;
      if (std::holds_alternative<llvm::APSInt>(*RHS.Relation[I]) &&
          !std::holds_alternative<llvm::APSInt>(*LHS.Relation[I]))
        return true;
      if (std::holds_alternative<llvm::APSInt>(*LHS.Relation[I]))
        return std::get<llvm::APSInt>(*LHS.Relation[I]) <
               std::get<llvm::APSInt>(*RHS.Relation[I]);
      auto LHSAxis{std::get<Align::Axis>(*LHS.Relation[I])};
      auto RHSAxis{std::get<Align::Axis>(*LHS.Relation[I])};
      auto LHSTuple{
          std::tuple{LHSAxis.Dimension, LHSAxis.Step, LHSAxis.Offset}};
      auto RHSTuple{
          std::tuple{RHSAxis.Dimension, RHSAxis.Step, RHSAxis.Offset}};
      if (LHSTuple != RHSTuple)
        return LHSTuple < RHSTuple;
    }
    return false;
  }
};

using AlignVarListT = std::set<Align, AlignLess>;

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

  AlignVarListT &getMemory() noexcept { return mMemory; }
  const AlignVarListT &getMemory() const noexcept { return mMemory; }

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
  AlignVarListT mMemory;
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

class PragmaRemoteAccess : public PragmaData {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() ==
           static_cast<unsigned>(DirectiveId::DvmRemoteAccess);
  }

  PragmaRemoteAccess(bool IsRequired, bool IsFinal = false)
      : PragmaData(DirectiveId::DvmRemoteAccess, IsRequired, IsFinal) {}
};

class PragmaRealign : public ParallelItem {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->getKind() == static_cast<unsigned>(DirectiveId::DvmRealign);
  }

  PragmaRealign(const VariableT &What, unsigned WhatDimSize,
                const std::variant<VariableT, Template *> &With)
      : ParallelItem(static_cast<unsigned>(DirectiveId::DvmRealign), false),
        mAlign(With), mWhat(What), mWhatDimSize(WhatDimSize) {
  }

  const VariableT &what() const noexcept { return mWhat; }
  unsigned getWhatDimSize() const noexcept { return mWhatDimSize; }
  const Align &with() const noexcept { return mAlign; }

  Align &with() noexcept { return mAlign; };

private:
  VariableT mWhat;
  unsigned mWhatDimSize;
  Align mAlign;
};

class PragmaParallel : public ParallelItem {
public:
  using LoopNestT =
      llvm::SmallVector<bcl::tagged_pair<bcl::tagged<ObjectID, llvm::Loop>,
                                         bcl::tagged<VariableT, VariableT>>,
                        4>;
  using VarMappingT =
      std::multimap<VariableT, llvm::SmallVector<std::pair<ObjectID, bool>, 4>,
                    ClangDependenceAnalyzer::VariableLess>;

  using ClauseList =
      bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                        bcl::tagged<ReductionVarListT, trait::Reduction>,
                        bcl::tagged<ShadowVarListT, trait::Dependence>,
                        bcl::tagged<LoopNestT, trait::Induction>,
                        bcl::tagged<VarMappingT, trait::DirectAccess>,
                        bcl::tagged<ShadowVarListT, Shadow>,
                        bcl::tagged<AlignVarListT, Remote>,
                        bcl::tagged<llvm::Optional<Align>, Align>>;

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

/// Return a parallelization directive which is attached to a loop if the loop
/// has been parallelized.
PragmaParallel *isParallel(const llvm::Loop *L,
                           Parallelization &ParallelizationInfo);

/// Return parallelization directive and an outermost loop in a parallel nest
/// if a specified instruction belongs to any parallel nest.
std::pair<PragmaParallel *, llvm::Loop *>
isInParallelNest(const llvm::Instruction &I, const llvm::LoopInfo &LI,
                 Parallelization &PI);

/// Build across clauses for a specified loop.
///
/// \pre Loop must contain regular data dependencies and constant step.
/// \return Size of loop nest (the current loop is a top-level loop in the nest)
/// that can be parallelized.
unsigned processRegularDependencies(
    ObjectID LoopID, const llvm::SCEVConstant *ConstStep,
    const ClangDependenceAnalyzer::ASTRegionTraitInfo &ASTDepInfo,
    DIArrayAccessInfo &AccessInfo, ShadowVarListT &AcrossInfo);
} // namespace dvmh
}

namespace llvm {
class DVMHParallelizationContext : public llvm::ImmutablePass,
                                   private bcl::Uncopyable {
  using TemplateList = SmallVector<std::unique_ptr<tsar::dvmh::Template>, 1>;

public:
  using template_iterator = llvm::raw_pointer_iterator<TemplateList::iterator>;
  using const_template_iterator =
      llvm::raw_pointer_iterator<TemplateList::const_iterator>;

  static char ID;
  DVMHParallelizationContext() : ImmutablePass(ID) {
    initializeDVMHParallelizationContextPass(*PassRegistry::getPassRegistry());
  }

  tsar::Parallelization &getParallelization() noexcept {
    return mParallelizationInfo;
  }

  const tsar::Parallelization &getParallelization() const noexcept {
    return mParallelizationInfo;
  }

  auto &getIPORoot() noexcept { return mIPORoot; }
  const auto &getIPORoot() const noexcept { return mIPORoot; }

  /// Return true if a specified function is called from a parallel loop (may be
  /// implicitly).
  bool isParallelCallee(const Function &F) const {
    return mParallelCallees.count(&F);
  }

  void markAsParallelCallee(const Function &F) {
    mParallelCallees.insert(&F);
  }

  tsar::dvmh::Template *makeTemplate(StringRef Name) {
    mTemplates.push_back(std::make_unique<tsar::dvmh::Template>(Name));
    return mTemplates.back().get();
  }

  template_iterator template_begin() {
    return template_iterator(mTemplates.begin());
  }
  template_iterator template_end() {
    return template_iterator(mTemplates.end());
  }
  auto templates() { return make_range(template_begin(), template_end()); }

  const_template_iterator template_begin() const {
    return const_template_iterator(mTemplates.begin());
  }
  const_template_iterator template_end() const {
    return const_template_iterator(mTemplates.end());
  }
  auto templates() const {
    return make_range(template_begin(), template_end());
  }

  bool template_empty() { return mTemplates.empty(); }
  std::size_t template_size() { return mTemplates.size(); }

private:
  tsar::Parallelization mParallelizationInfo;

  /// This directive is a stub which specifies in a replacement tree whether IPO
  /// was successful.
  ///
  /// Optimization is successful if all descendant nodes of the IPO root are
  /// valid.
  tsar::dvmh::PragmaActual mIPORoot{false, false};

  TemplateList mTemplates;
  SmallPtrSet<const Function *, 16> mParallelCallees;
};
}
#endif//TSAR_DVMH_DIRECTIVES_H
