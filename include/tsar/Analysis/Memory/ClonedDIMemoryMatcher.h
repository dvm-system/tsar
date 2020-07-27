//=== ClonedDIMemoryMatcher.h - Original-to-Clone Memory Matcher *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
//===----------------------------------------------------------------------===//
//
// This file defines a pass to match metadata-level memory locations in
// an original module and its clone.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLONED_MEMORY_MATCHER_H
#define TSAR_CLONED_MEMORY_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/Memory/BimapDIMemoryHandle.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/Tags.h"
#include <bcl/tagged.h>
#include <llvm/ADT/DenseMapInfo.h>

namespace llvm {
class MDNode;
}

namespace tsar {
/// Map from raw metadata-level memory representation to a memory location
/// in metadata-level alias tree.
///
/// Note, that memory locations in different functions have different
/// representations in alias tree. However, some locations (for example
/// globals) have the same raw metadata-level memory representations. Hence,
/// the key will be a pair (function and raw memory).
using MDToDIMemoryMap = llvm::DenseMap<
  std::pair<llvm::Function *, llvm::MDNode *>, WeakDIMemoryHandle>;

/// This is a map from a memory location in original module to a cloned one.
class ClonedDIMemoryMatcher :
  public Bimap<
    bcl::tagged<BimapDIMemoryHandle<Origin, ClonedDIMemoryMatcher>, Origin>,
    bcl::tagged<BimapDIMemoryHandle<Clone, ClonedDIMemoryMatcher>, Clone>,
    DIMemoryMapInfo, DIMemoryMapInfo> {};

using OriginDIMemoryHandle = BimapDIMemoryHandle<Origin, ClonedDIMemoryMatcher>;
using CloneDIMemoryHandle = BimapDIMemoryHandle<Clone, ClonedDIMemoryMatcher>;

/// For each function this matcher contains a map from a memory location in
/// original module to a cloned one.
class ClonedDIMemoryMatcherInfo {
  /// This defines callback that run when underlying function has RAUW
  /// called on it or destroyed.
  ///
  /// This updates map from function to memory matcher.
  class FunctionCallbackVH final : public llvm::CallbackVH {
    ClonedDIMemoryMatcherInfo *mInfo;
    void deleted() override {
      mInfo->erase(llvm::cast<llvm::Function>(*getValPtr()));
    }
    void allUsesReplacedWith(llvm::Value *V) override {
      mInfo->erase(llvm::cast<llvm::Function>(*getValPtr()));
    }
  public:
    FunctionCallbackVH(llvm::Value *V, ClonedDIMemoryMatcherInfo *I = nullptr) :
      CallbackVH(V), mInfo(I) {}
    FunctionCallbackVH & operator=(llvm::Value *V) {
      return *this = FunctionCallbackVH(V, mInfo);
    }
  };

  friend class FunctionCabackVH;

  struct FunctionCallbackVHDenseMapInfo :
    public llvm::DenseMapInfo<llvm::Value *> {};

  using FunctionToMatcherMap = llvm::DenseMap<FunctionCallbackVH,
    std::unique_ptr<ClonedDIMemoryMatcher>, FunctionCallbackVHDenseMapInfo>;

public:
  std::pair<ClonedDIMemoryMatcher *, bool> insert(llvm::Function &F) {
    auto Pair = mMatchers.try_emplace(FunctionCallbackVH(&F, this));
    if (Pair.second)
      Pair.first->second = std::make_unique<ClonedDIMemoryMatcher>();
    return std::make_pair(Pair.first->second.get(), Pair.second);
  }

  void erase(llvm::Function &F) {
    auto Itr = mMatchers.find_as(&F);
    if (Itr != mMatchers.end())
      mMatchers.erase(Itr);
  }

  void clear() { mMatchers.clear(); }

  ClonedDIMemoryMatcher * find(llvm::Function &F) {
    auto Itr = mMatchers.find_as(&F);
    return Itr == mMatchers.end() ? nullptr : Itr->second.get();
  }

  const ClonedDIMemoryMatcher * find(llvm::Function &F) const {
    auto Itr = mMatchers.find_as(&F);
    return Itr == mMatchers.end() ? nullptr : Itr->second.get();
  }

  ClonedDIMemoryMatcher * operator[](llvm::Function &F) { return find(F); }
  const ClonedDIMemoryMatcher *operator[](llvm::Function &F) const {
    return find(F);
  }

private:
  FunctionToMatcherMap mMatchers;
};

} // namespace tsar

namespace llvm {
/// Wrapper to allow server passes to determine relation between original
/// and cloned metadata-level memory locations.
using ClonedDIMemoryMatcherWrapper =
    AnalysisWrapperPass<tsar::ClonedDIMemoryMatcherInfo>;

/// Create immutable storage for matched memory.
ImmutablePass *createClonedDIMemoryMatcherStorage();

/// Create a pass with a specified math between cloned MDNodes and original
/// DIMemory.
FunctionPass *
createClonedDIMemoryMatcher(const tsar::MDToDIMemoryMap &CloneToOriginal);

/// Create a pass with a specified math between cloned MDNodes and original
/// DIMemory.
FunctionPass *createClonedDIMemoryMatcher(tsar::MDToDIMemoryMap &&);

/// Initialize a pass to store matched memory.
void initializeClonedDIMemoryMatcherStoragePass(PassRegistry &);

/// Initialize a pass to access matched memory.
void initializeClonedDIMemoryMatcherWrapperPass(PassRegistry &);

/// Initialize a pass to match memory (original to cloned).
void initializeClonedDIMemoryMatcherPassPass(PassRegistry &);
} // namespace llvm

#endif // TSAR_CLONED_MEMORY_MATCHER_H
