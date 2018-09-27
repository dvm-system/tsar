//===- PassGroupRegistry.h - Simple Group of Passes Registry ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines stuff that is used to initialize Passes that should be
// joined in a group. A group is represented as a some simple storage.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_PASS_SUPPORT_H
#define TSAR_PASS_SUPPORT_H

#include <vector>

namespace llvm {
class PassInfo;
}

namespace tsar {
/// Group of passes, which stores already allocated and registered PassInfo.
class PassGroupRegistry {
  using PassList = std::vector<llvm::PassInfo *>;

public:
  using iterator = PassList::iterator;
  using const_iterator = PassList::const_iterator;

  void add(llvm::PassInfo *PI) { mPasses.push_back(PI); }

  iterator begin() { return mPasses.begin();}
  iterator end() { return mPasses.end(); }

  const_iterator begin() const { return mPasses.begin();}
  const_iterator end() const { return mPasses.end(); }

  bool exist(llvm::PassInfo *PI) const {
    for (auto *InListPI : mPasses)
      if (InListPI == PI)
        return true;
    return false;
  }

private:
  std::vector<llvm::PassInfo *> mPasses;
};
}

#define INITIALIZE_PASS_IN_GROUP_BEGIN(passName, arg, name, cfg, analysis, groupRegistry) \
  static void *initialize##passName##PassOnce(PassRegistry &Registry) {

#define INITIALIZE_PASS_IN_GROUP_END(passName, arg, name, cfg, analysis, groupRegistry) \
  PassInfo *PI = new PassInfo(                                                 \
      name, arg, &passName::ID,                                                \
      PassInfo::NormalCtor_t(callDefaultCtor<passName>), cfg, analysis);       \
  Registry.registerPass(*PI, true);                                            \
  groupRegistry.add(PI);                                                       \
  return PI;                                                                   \
  }                                                                            \
  LLVM_DEFINE_ONCE_FLAG(Initialize##passName##PassFlag);                       \
  void llvm::initialize##passName##Pass(PassRegistry &Registry) {              \
    llvm::call_once(Initialize##passName##PassFlag,                            \
                    initialize##passName##PassOnce, std::ref(Registry));       \
  }
#endif//TSAR_PASS_SUPPORT_H
