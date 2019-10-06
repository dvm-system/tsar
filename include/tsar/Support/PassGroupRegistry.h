//===- PassGroupRegistry.h - Simple Group of Passes Registry ----*- C++ -*-===//
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
// This file defines stuff that is used to initialize Passes that should be
// joined in a group. A group is represented as a some simple storage.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_PASS_SUPPORT_H
#define TSAR_PASS_SUPPORT_H

#include <memory>
#include <vector>

namespace llvm {
class PassInfo;
namespace legacy {
class PassManager;
}
}

namespace tsar {
class PassGroupInfo {
public:
  virtual ~PassGroupInfo() {}

  virtual void addBeforePass(llvm::legacy::PassManager &PM) const {}

  virtual void addAfterPass(llvm::legacy::PassManager& PM) const {}

  /// Return true if a specified past should be executed before the current pass.
  ///
  /// This method may be useful if there is no explicit dependence between two
  /// passes, however results of the necessary pass drastically influence on
  /// the results of the current one. So, there are no reasons to execute
  /// the current pass if necessary passes has not been executed.
  virtual bool isNecessaryPass(const void * ID) const { return false; }
};

/// Group of passes, which stores already allocated and registered PassInfo.
class PassGroupRegistry {
  using PassList = std::vector<const llvm::PassInfo *>;
  using PassGroupInfoList = std::vector<std::unique_ptr<PassGroupInfo>>;

public:
  using iterator = PassList::iterator;
  using const_iterator = PassList::const_iterator;

  void add(const llvm::PassInfo &PI, std::unique_ptr<PassGroupInfo> GI) {
    mPasses.push_back(&PI);
    mGroupInfo.push_back(std::move(GI));
  }

  iterator begin() { return mPasses.begin();}
  iterator end() { return mPasses.end(); }

  const_iterator begin() const { return mPasses.begin();}
  const_iterator end() const { return mPasses.end(); }

  bool exist(const llvm::PassInfo &PI) const {
    for (auto *InListPI : mPasses)
      if (InListPI == &PI)
        return true;
    return false;
  }

  PassGroupInfo * groupInfo(const llvm::PassInfo &PI) const {
    for (std::size_t I = 0, EI = mPasses.size(); I < EI; ++I)
      if (mPasses[I] == &PI)
        return mGroupInfo[I].get();
    return nullptr;
  }

private:
  PassList mPasses;
  PassGroupInfoList mGroupInfo;
};
}

#define INITIALIZE_PASS_IN_GROUP_INFO(passGroupInfo) \
    GI = std::unique_ptr<passGroupInfo>(new passGroupInfo);

#define INITIALIZE_PASS_IN_GROUP(passName, arg, name, cfg, analysis, groupRegistry) \
  static void *initialize##passName##PassOnce(PassRegistry &Registry) {        \
    PassInfo *PI = new PassInfo(                                               \
        name, arg, &passName::ID,                                              \
        PassInfo::NormalCtor_t(callDefaultCtor<passName>), cfg, analysis);     \
    groupRegistry.add(*PI, nullptr);                                                    \
    Registry.registerPass(*PI, true);                                          \
    return PI;                                                                 \
  }                                                                            \
  static llvm::once_flag Initialize##passName##PassFlag;                       \
  void llvm::initialize##passName##Pass(PassRegistry &Registry) {              \
    llvm::call_once(Initialize##passName##PassFlag,                            \
                    initialize##passName##PassOnce, std::ref(Registry));       \
  }

#define INITIALIZE_PASS_IN_GROUP_BEGIN(passName, arg, name, cfg, analysis, groupRegistry) \
  static void *initialize##passName##PassOnce(PassRegistry &Registry) { \
    std::unique_ptr<PassGroupInfo> GI;

#define INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, cfg, analysis, groupRegistry) \
  PassInfo *PI = new PassInfo(                                                 \
      name, arg, &passName::ID,                                                \
      PassInfo::NormalCtor_t(callDefaultCtor<passName>), cfg, analysis);       \
  groupRegistry.add(*PI, std::move(GI));                                                      \
  Registry.registerPass(*PI, true);                                            \
  return PI;                                                                   \
  }                                                                            \
  static llvm::once_flag Initialize##passName##PassFlag;                       \
  void llvm::initialize##passName##Pass(PassRegistry &Registry) {              \
    llvm::call_once(Initialize##passName##PassFlag,                            \
                    initialize##passName##PassOnce, std::ref(Registry));       \
  }
#endif//TSAR_PASS_SUPPORT_H
