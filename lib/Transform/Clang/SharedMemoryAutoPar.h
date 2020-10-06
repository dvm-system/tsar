//===- SharedMemoryAutoPar.cpp - Shared Memory Parallelization ---*- C++ -*===//
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
// This file implements a general abstract pass to perform auto parallelization
// for a shared memory.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_SHARED_PARALLEL_H
#define TSAR_CLANG_SHARED_PARALLEL_H

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Support/PassGroupRegistry.h"
#include <bcl/cell.h>
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>

namespace clang {
class ForStmt;
}

namespace tsar {
class DFLoop;
class AnalysisSocketInfo;
class ClangDependenceAnalyzer;
class DIMemoryEnvironment;
class OptimizationRegion;
class TransformationContext;
struct GlobalOptions;
struct MemoryMatchInfo;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// This represents a parallel construct.
class ParallelItem {
  enum Flags : uint8_t {
    NoProperty = 0,
    Final = 1u << 0,
    ChildPossible = 1u << 1,
    Marker = 1u << 2,
    LLVM_MARK_AS_BITMASK_ENUM(Marker)
  };
public:
  explicit ParallelItem(unsigned Kind, bool IsFinal,
                        ParallelItem *Parent = nullptr)
    : ParallelItem(Kind, IsFinal, false, false, Parent) {}
  virtual ~ParallelItem() {}

  /// Return user-defined kind of a parallel item.
  unsigned getKind() const noexcept { return mKind; }

  /// Return true if this item may contain nested parallel items.
  bool isChildPossible() const noexcept { return mFlags & ChildPossible; }

  /// Return true if this item has been finalized. For hierarchical source code
  /// constructs (for example loops) it means that nested constructs should not
  /// be analyzed.
  ///
  /// Use finalize() method to mark this item as final.
  bool isFinal() const noexcept { return mFlags & Final; }

  /// Return true if this is a marker which is auxiliary construction.
  bool isMarker() const noexcept { return mFlags & Marker; }

  ParallelItem *getParent() noexcept { return mParent; }
  const ParallelItem *getParent() const noexcept { return mParent; }

  void setParent(ParallelItem *Parent) noexcept { mParent = Parent; }

  /// Mark item as final.
  ///
  /// \attention Overridden methods have to call this one to set corresponding
  /// flags.
  virtual void finalize() { mFlags |= Final; }

protected:
  ParallelItem(unsigned Kind, bool IsFinal, bool IsMarker, bool IsChildPossible,
               ParallelItem *Parent)
      : mParent(Parent), mFlags(NoProperty), mKind(Kind) {
    if (IsFinal)
      mFlags |= Final;
    if (IsMarker)
      mFlags |= Marker;
    if (IsChildPossible)
      mFlags |= ChildPossible;
  }

private:
  ParallelItem *mParent;
  Flags mFlags;
  unsigned mKind;
};

/// This represents a parallel construct which may contain other constructs
/// (for exmplae DVMH region contains parallel loops).
class ParallelLevel : public ParallelItem {
public:
  using child_iterator = std::vector<ParallelItem *>::iterator;
  using const_child_iterator = std::vector<ParallelItem *>::const_iterator;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->isChildPossible();
  }

  explicit ParallelLevel(unsigned Kind, bool IsFinal,
                        ParallelItem *Parent = nullptr)
      : ParallelItem(Kind, IsFinal, true, false, Parent) {
  }
  ~ParallelLevel() {
    for (auto *Child : mChildren)
      Child->setParent(nullptr);
  }

  child_iterator child_insert(ParallelItem *Item) {
    mChildren.push_back(Item);
    if (Item->getParent()) {
      auto PrevChildItr = llvm::find(
          llvm::cast<ParallelLevel>(Item->getParent())->children(), Item);
      assert(PrevChildItr !=
                 llvm::cast<ParallelLevel>(Item->getParent())->child_end() &&
             "Corrupted parallel item, parent must contain its child!");
      llvm::cast<ParallelLevel>(Item->getParent())->child_erase(PrevChildItr);
    }
    Item->setParent(this);
    return mChildren.end() - 1;
  }

  child_iterator child_erase(child_iterator I) {
    (*I)->setParent(nullptr);
    return mChildren.erase(I);
  }

  child_iterator child_begin() { return mChildren.begin(); }
  child_iterator child_end() { return mChildren.end(); }

  const_child_iterator child_begin() const { return mChildren.begin(); }
  const_child_iterator child_end() const { return mChildren.end(); }

  llvm::iterator_range<child_iterator> children() {
    return llvm::make_range(child_begin(), child_end());
  }

  llvm::iterator_range<const_child_iterator> children() const {
    return llvm::make_range(child_begin(), child_end());
  }

private:
  std::vector<ParallelItem *> mChildren;
};

/// This is auxiliary item which references to an item of a specified type.
///
/// Use get/setParent() to access original item. This class is useful to mark
/// the end of a parallel construct in source code.
template<class ItemT>
class ParallelMarker : public ParallelItem {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->isMarker() && Item->getParent() &&
           llvm::isa<ItemT>(Item->getParent());
  }

  ParallelMarker(unsigned Kind, ItemT *For)
      : ParallelItem(Kind, true, true, false, For) {}
};
}

namespace llvm {
class GlobalsAAResult;
class Function;
class Loop;

class CanonicalLoopPass;
class ClangPerfectLoopPass;
class ClangDIMemoryMatcherPass;
class DIEstimateMemoryPass;
class DFRegionInfoPass;
class LoopMatcherPass;
class LoopInfoWrapperPass;
class ParallelLoopPass;

/// This provide access to function-level analysis results on client.
using FunctionAnalysis =
    bcl::StaticTypeMap<AnalysisSocketImmutableWrapper *, LoopInfoWrapperPass *,
                       ParallelLoopPass *, CanonicalLoopPass *,
                       LoopMatcherPass *, DFRegionInfoPass *,
                       ClangDIMemoryMatcherPass *, DIEstimateMemoryPass *,
                       MemoryMatcherImmutableWrapper *, ClangPerfectLoopPass *>;

/// This pass try to insert directives into a source code to obtain
/// a parallel program for a shared memory.
class ClangSMParallelization: public ModulePass, private bcl::Uncopyable {
  struct Preorder {};
  struct ReversePreorder {};
  struct Postorder {};
  struct ReversePostorder {};

  /// Storage for numbers of call graph nodes.
  using CGNodeNumbering = llvm::DenseMap<
    Function *,
    std::tuple<
      std::size_t, std::size_t,
      std::size_t, std::size_t>,
    DenseMapInfo<Function *>,
    tsar::TaggedDenseMapTuple<
      bcl::tagged<Function *, Function>,
      bcl::tagged<std::size_t, Preorder>,
      bcl::tagged<std::size_t, ReversePreorder>,
      bcl::tagged<std::size_t, Postorder>,
      bcl::tagged<std::size_t, ReversePostorder>>>;

public:
  ClangSMParallelization(char &ID);

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override {
    mRegions.clear();
    mTfmCtx = nullptr;
    mGlobalOpts = nullptr;
    mMemoryMatcher = nullptr;
    mGlobalsAA = nullptr;
    mSocketInfo = nullptr;
  }

protected:
  /// Exploit parallelism for a specified loop.
  ///
  /// This function is will be called if some general conditions are
  /// successfully checked.
  /// \return true if a specified loop could be parallelized and inner loops
  /// should not be processed.
  virtual tsar::ParallelItem *
  exploitParallelism(const tsar::DFLoop &IR, const clang::ForStmt &AST,
                     const FunctionAnalysis &Provider,
                     tsar::ClangDependenceAnalyzer &ASTDepInfo,
                     tsar::ParallelItem *PI) = 0;

  /// Process loop after its body parallelization.
  virtual void optimizeLevel(PointerUnion<Loop *, Function *> Level,
      const FunctionAnalysis &Provider) {}

  /// Return analysis results computed on the client for a specified function.
  FunctionAnalysis analyzeFunction(llvm::Function &F);
private:
  /// Initialize provider before on the fly passes will be run on client.
  void initializeProviderOnClient(Module &M);

  /// Check whether it is possible to parallelize a specified loop, analyze
  /// inner loops on failure.
  bool findParallelLoops(Loop &L, const FunctionAnalysis &Provider,
      tsar::ParallelItem *PI);

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
  bool findParallelLoops(PointerUnion<Loop *, Function *> Parent,
      ItrT I, ItrT EI, const FunctionAnalysis &Provider,
      tsar::ParallelItem *PI) {
    auto Parallelized = false;
    for (; I != EI; ++I)
      Parallelized |= findParallelLoops(**I, Provider, PI);
    if (Parallelized)
      optimizeLevel(Parent, Provider);
    return Parallelized;
  }

  tsar::TransformationContext *mTfmCtx = nullptr;
  const tsar::GlobalOptions *mGlobalOpts = nullptr;
  tsar::MemoryMatchInfo *mMemoryMatcher = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;
  tsar::AnalysisSocketInfo *mSocketInfo = nullptr;
  tsar::DIMemoryEnvironment *mDIMEnv = nullptr;
  SmallVector<const tsar::OptimizationRegion *, 4> mRegions;
  CGNodeNumbering mCGNodes;
  CGNodeNumbering mParallelCallees;
};

/// This specifies additional passes which must be run on client.
class ClangSMParallelizationInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
}

#define INITIALIZE_SHARED_PARALLELIZATION(passName, arg, name)                 \
  INITIALIZE_PASS_IN_GROUP_BEGIN(                                              \
      passName, arg, name, false, false,                                       \
      TransformationQueryManager::getPassRegistry())                           \
  INITIALIZE_PASS_IN_GROUP_INFO(ClangSMParallelizationInfo)                    \
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)                             \
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)                              \
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)                                 \
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)                             \
  INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)                         \
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)                         \
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)                    \
  INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)                       \
  INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)                          \
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)                    \
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)                       \
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)                         \
  INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)                     \
  INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)                                 \
  INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)                                \
  INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)                             \
  INITIALIZE_PASS_DEPENDENCY(DIArrayAccessWrapper)                             \
  INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, false, false,              \
                               TransformationQueryManager::getPassRegistry())
#endif//TSAR_CLANG_SHARED_PARALLEL_H
