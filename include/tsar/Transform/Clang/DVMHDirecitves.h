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
}

namespace tsar {
namespace dvmh {
using DistanceInfo = ClangDependenceAnalyzer::DistanceInfo;
using VariableT = ClangDependenceAnalyzer::VariableT;
using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
using SortedVarMultiListT = ClangDependenceAnalyzer::SortedVarMultiListT;

class Template {
public:
  explicit Template(llvm::StringRef Name) : mName(Name) {}
  llvm::StringRef getName() const noexcept { return mName; }
private:
  std::string mName;
};

struct Align {
  struct Axis {
    unsigned Dimension;
    llvm::APSInt Step;
    llvm::APSInt Offset;
  };
  std::variant<VariableT, Template *> Target;
  llvm::SmallVector<llvm::Optional<std::variant<Axis, llvm::APSInt>>, 4>
      Relation;
};

struct Shadow {};

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
  using ShadowVarListT =
      std::map<VariableT, trait::DIDependence::DistanceVector,
               ClangDependenceAnalyzer::VariableLess>;
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
}
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
