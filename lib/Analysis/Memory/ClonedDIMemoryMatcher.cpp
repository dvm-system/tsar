//=== ClonedDIMemoryMatcher.cpp Original-to-Clone Memory Matcher *- C++ -*-===//
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
// This file implements a pass to match metadata-level memory locations in
// an original module and its clone.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Unparse/Utils.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "di-memory-handle"

using namespace llvm;
using namespace tsar;

namespace {
class ClonedDIMemoryMatcherStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;
  ClonedDIMemoryMatcherStorage() : ImmutablePass(ID) {
    initializeClonedDIMemoryMatcherStoragePass(*PassRegistry::getPassRegistry());
  }
  void initializePass() override {
    getAnalysis<ClonedDIMemoryMatcherWrapper>().set(mMatcher);
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ClonedDIMemoryMatcherWrapper>();
  }
private:
  ClonedDIMemoryMatcherInfo mMatcher;
};

/// This memory pass establishes relation between metadata-level memory
/// locations in original and cloned modules.
class ClonedDIMemoryMatcherPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClonedDIMemoryMatcherPass() : FunctionPass(ID) {
    initializeClonedDIMemoryMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  /// Create a pass with a specified math between cloned MDNodes and original
  /// DIMemory.
  ClonedDIMemoryMatcherPass(const MDToDIMemoryMap &CloneToOriginal)
      : FunctionPass(ID), mCloneToOrigin(CloneToOriginal) {
    initializeClonedDIMemoryMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  /// Create a pass with a specified math between cloned MDNodes and original
  /// DIMemory.
  ClonedDIMemoryMatcherPass(MDToDIMemoryMap &&CloneToOriginal)
      : FunctionPass(ID), mCloneToOrigin(std::move(CloneToOriginal)) {
    initializeClonedDIMemoryMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
    auto *OriginToClone =
        getAnalysis<ClonedDIMemoryMatcherWrapper>()->insert(F).first;
    assert(OriginToClone && "Unable to create memory matcher!");
    for (auto &DIM : make_range(DIAT.memory_begin(), DIAT.memory_end())) {
      auto Itr = mCloneToOrigin.find(std::make_pair(&F, DIM.getAsMDNode()));
      // Some memory locations are always distinct after tree rebuilding.
      // So, this memory locations does not exist in the map.
      if (Itr == mCloneToOrigin.end()) {
        LLVM_DEBUG(dbgs() << "[CLONED DI MEMORY]: original memory location "
                             "is not found for '";
                   if (auto DWLang = getLanguage(F))
                       printDILocationSource(*DWLang, DIM, dbgs());
                   dbgs() << "'\n");
        continue;
      }
      OriginToClone->emplace(ClonedDIMemoryMatcher::value_type(
        std::piecewise_construct,
        std::forward_as_tuple(Itr->second, OriginToClone),
        std::forward_as_tuple(&DIM, OriginToClone)));
    }
    return false;
  }

  void getAnalysisUsage(AnalysisUsage& AU) const override {
    AU.addRequired<ClonedDIMemoryMatcherWrapper>();
    AU.addRequired<DIEstimateMemoryPass>();
    AU.setPreservesAll();
  }

  MDToDIMemoryMap mCloneToOrigin;
};
}

char ClonedDIMemoryMatcherPass::ID = 0;
INITIALIZE_PASS_BEGIN(ClonedDIMemoryMatcherPass, "cloned-di-memory-matcher",
  "Cloned Memory Matcher (Metadata)", false, false)
  INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_END(ClonedDIMemoryMatcherPass, "cloned-di-memory-matcher",
  "Cloned Memory Matcher (Metadata)", false, false)

char ClonedDIMemoryMatcherStorage::ID = 0;
INITIALIZE_PASS_BEGIN(ClonedDIMemoryMatcherStorage,
  "cloned-di-memory-matcher-is", "Cloned Memory Matcher (Metadata, Storage)",
   false, false)
  INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_END(ClonedDIMemoryMatcherStorage,
  "cloned-di-memory-matcher-is", "Cloned Memory Matcher (Metadata, Storage)",
  false, false)

ImmutablePass* llvm::createClonedDIMemoryMatcherStorage() {
  return new ClonedDIMemoryMatcherStorage;
}

template<> char ClonedDIMemoryMatcherWrapper::ID = 0;
INITIALIZE_PASS(ClonedDIMemoryMatcherWrapper,"cloned-di-memory-matcher-iw",
  "Cloned Memory Matcher (Metadata, Wrapper)", false, false)

FunctionPass* llvm::createClonedDIMemoryMatcher(
    const tsar::MDToDIMemoryMap &CloneToOriginal) {
  return new ClonedDIMemoryMatcherPass(CloneToOriginal);
}

FunctionPass* llvm::createClonedDIMemoryMatcher(
    tsar::MDToDIMemoryMap &&CloneToOriginal) {
  return new ClonedDIMemoryMatcherPass(std::move(CloneToOriginal));
}
