//===- DIMemoryEnvironment.h - DIMemory Global State Manager ----*- C++ -*-===//
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
// This file defines DIMemoryEnvironment, a container of "global" state of
// debug-level memory locations, such as the alias trees and memory handles
// containers.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_MEMORY_ENVIRONMENT_H
#define TSAR_DI_MEMORY_ENVIRONMENT_H

#include "tsar/Support/AnalysisWrapperPass.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/ValueHandle.h>
#include <memory>

namespace tsar {
class DIAliasTree;
class DIMemory;
class DIMemoryHandleBase;

class DIMemoryEnvironment final {
  /// \brief This defines callback that run when underlying function has RAUW
  /// called on it or destroyed.
  ///
  /// This updates map from function to its debug alias tree.
  class FunctionCallbackVH final : public llvm::CallbackVH {
    DIMemoryEnvironment *mEnv;
    void deleted() override {
      mEnv->erase(llvm::cast<llvm::Function>(*getValPtr()));
    }
    void allUsesReplacedWith(llvm::Value *V) override {
      mEnv->erase(llvm::cast<llvm::Function>(*getValPtr()));
    }
  public:
    FunctionCallbackVH(llvm::Value *V, DIMemoryEnvironment *Env = nullptr) :
      CallbackVH(V), mEnv(Env) {}
    FunctionCallbackVH & operator=(llvm::Value *V) {
      return *this = FunctionCallbackVH(V, mEnv);
    }
  };

  struct FunctionCallbackVHDenseMapInfo :
    public llvm::DenseMapInfo<llvm::Value *> {};

  /// Map from a function to its debug alias tree.
  using FunctionToTreeMap =
    llvm::DenseMap<FunctionCallbackVH, std::unique_ptr<DIAliasTree>,
    FunctionCallbackVHDenseMapInfo>;

public:
  ~DIMemoryEnvironment() {
    // It is not possible to delete handles here, because a handle may not be
    // a dynamic object. So, we only check that there is no active handles.
    assert(mMemoryHandles.empty() &&
      "Memory handles must be deleted before environment!");
  }

  /// Map from a memory to list of handles.
  using DIMemoryHandleMap = llvm::DenseMap<DIMemory *, DIMemoryHandleBase *>;

  /// Resets alias tree for a specified function with a specified alias tree
  /// and returns pointer to a new tree.
  DIAliasTree * reset(llvm::Function &F, std::unique_ptr<DIAliasTree> &&AT) {
    auto Itr = mTrees.try_emplace(FunctionCallbackVH(&F, this)).first;
    Itr->second = std::move(AT);
    return Itr->second.get();
  }

  /// Extracts alias tree for a specified function from storage and returns it.
  std::unique_ptr<DIAliasTree> release(llvm::Function &F) {
    auto Itr = mTrees.find_as(&F);
    if (Itr != mTrees.end()) {
      auto AT = std::move(Itr->second);
      mTrees.erase(Itr);
      return AT;
    }
    return nullptr;
  }

  /// Erases alias tree for a specified function from the storage.
  void erase(llvm::Function &F) {
    auto Itr = mTrees.find_as(&F);
    if (Itr != mTrees.end())
      mTrees.erase(Itr);
  }

  /// Returns alias tree for a specified function or nullptr.
  DIAliasTree * get(llvm::Function &F) const {
    auto Itr = mTrees.find_as(&F);
    return Itr == mTrees.end() ? nullptr : Itr->second.get();
  }

  /// Returns alias tree for a specified function or nullptr.
  DIAliasTree * operator[](llvm::Function &F) const { return get(F); }

  /// Returns all available memory handles.
  DIMemoryHandleMap & getMemoryHandles() noexcept {
    return mMemoryHandles;
  }

  /// Returns memory handles for a specified memory location.
  DIMemoryHandleBase *& operator[](DIMemory *M)  {
    return mMemoryHandles[M];
  }

private:
  FunctionToTreeMap mTrees;
  DIMemoryHandleMap mMemoryHandles;
};
}

namespace llvm {
/// Wrapper to access debug-level memory location environment.
using DIMemoryEnvironmentWrapper =
  AnalysisWrapperPass<tsar::DIMemoryEnvironment>;
}

#endif//TSAR_DI_MEMORY_ENVIRONMENT_H
