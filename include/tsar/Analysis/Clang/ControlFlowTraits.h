//===- ControlFlowTraits.h - Traits of Control Flow in Region ---*- C++ -*-===//
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
// This file declares passes to investigate a control flow inside a CFG region.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CONTROL_FLOW_TRAITS_H
#define TSAR_CONTROL_FLOW_TRAITS_H

#include "tsar/ADT/PersistentSet.h"
#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Pass.h>
#include <llvm/Support/type_traits.h>

namespace tsar {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// List of flags to describe behavior inside a region.
///
/// We do not consider call of a function inside a loop which can throw
/// exception as an exit from this loop even if there is a try-catch block and
/// catch is placed outside the loop.
enum CFFlags : uint8_t {
  DefaultFlags = 0,
  InOut = 1u << 0,
  Exit = 1u << 1,
  Entry = 1u << 2,
  MayNoReturn = 1u << 3,
  MayReturnTwice = 1u << 4,
  MayUnwind = 1u << 5,
  UnsafeCFG = MayNoReturn | MayReturnTwice | MayUnwind,
  LLVM_MARK_AS_BITMASK_ENUM(MayUnwind)
};

/// Description of a single control flow trait, this contains a statement which
/// is a source of a trait and a description of this traits with flags.
template <class StmtT> struct CFTraitsBase {
  StmtT Stmt;
  CFFlags Flags;
};
} // namespace tsar

namespace llvm {
template <class StmtT> struct DenseMapInfo<tsar::CFTraitsBase<StmtT>> {
  using T = tsar::CFTraitsBase<StmtT>;
  static inline T getEmptyKey() {
    return T{ llvm::DenseMapInfo<StmtT>::getEmptyKey(),
      tsar::CFFlags::DefaultFlags };
  }
  static inline T getTombstoneKey() {
    return T{ llvm::DenseMapInfo<StmtT>::getTombstoneKey(),
      tsar::CFFlags::DefaultFlags };
  }
  static inline unsigned getHashValue(const T &Val) {
    return llvm::DenseMapInfo<StmtT>::getHashValue(Val.Stmt);
  }
  static inline unsigned getHashValue(const StmtT &Val) {
    return llvm::DenseMapInfo<StmtT>::getHashValue(Val);
  }
  static inline bool isEqual(const T &LHS, const T &RHS) {
    return llvm::DenseMapInfo<StmtT>::isEqual(LHS.Stmt, RHS.Stmt);
  }
  static inline bool isEqual(const StmtT &LHS, const T &RHS) {
    return llvm::DenseMapInfo<StmtT>::isEqual(LHS, RHS.Stmt);
  }
};

// Specialize simplify_type to allow CFTraitsBase to participate in
// dyn_cast, isa, etc.
template<class StmtT> struct simplify_type<tsar::CFTraitsBase<StmtT>> {
  using SimpleType = StmtT;
  static SimpleType getSimplifiedValue(tsar::CFTraitsBase<StmtT> &Info) {
    return Info.Stmt;
  }
};
template<class StmtT> struct simplify_type<const tsar::CFTraitsBase<StmtT>> {
  using SimpleType = const StmtT;
  static SimpleType getSimplifiedValue(
    const tsar::CFTraitsBase<StmtT> &Info) {
    return Info.Stmt;
  }
};
} // namespace llvm

namespace tsar {
/// Information of traits which have been investigated for a region in a
/// control-flow graph. Note, that statement is used as key in a set of traits.
template<class FuncT, class StmtT>
class RegionCFInfoBase {
  using TraitSet = PersistentSet<CFTraitsBase<StmtT>>;
  using CallMap = llvm::DenseMap<FuncT,
    llvm::SmallVector<typename TraitSet::persistent_iterator, 16>>;

  using const_stmt_type_t =
    typename llvm::const_pointer_or_const_ref<StmtT>::type;
  using const_func_type_t =
    typename llvm::const_pointer_or_const_ref<FuncT>::type;

  /// This is a bidirectional iterator which allows to iterate over all calls
  /// of a function in `CallMap`.
  template<class ItrTy>
  class call_iterator_impl : std::iterator<std::bidirectional_iterator_tag,
      typename ItrTy::value_type> {
    template<class RHSItrTy> friend class call_iterator_impl;
  public:
    using pointer = typename ItrTy::value_type *;
    using reference = typename ItrTy::value_type &;

    explicit call_iterator_impl(const ItrTy *P = nullptr) : mItrPtr(P) {}

    template<class RHSItrTy>
    call_iterator_impl(const call_iterator_impl<RHSItrTy> &RHS) :
      mItrPtr(RHS.mItrPtr) {}

    template<class RHSItrTy>
    call_iterator_impl & operator=(const call_iterator_impl<RHSItrTy> &RHS) {
      mItrPtr = RHS.mItr;
    }

    reference operator*() const noexcept { return **mItrPtr; }
    pointer operator->() const noexcept { return &operator*(); }

    bool operator==(const call_iterator_impl &RHS) const noexcept {
      // Do not dereference mItrPtr here because it may be invalid.
      return mItrPtr == RHS.mItrPtr;
    }
    bool operator!=(const call_iterator_impl &RHS) const noexcept {
      return !operator==(RHS);
    }

    call_iterator_impl & operator++() noexcept { return ++mItrPtr; }
    call_iterator_impl & operator++(int) noexcept { return mItrPtr++; }

    call_iterator_impl & operator--() noexcept { return --mItrPtr; }
    call_iterator_impl & operator--(int) noexcept { return mItrPtr--; }

  private:
    const ItrTy *mItrPtr;
  };

public:
  using CFTraits = CFTraitsBase<StmtT>;

  /// This type used to iterate over traits.
  using iterator = typename TraitSet::iterator;

  /// This type used to iterate over traits.
  using const_iterator = typename TraitSet::const_iterator;

  /// Returns iterator that points to the beginning of the traits list.
  iterator begin() { return mTraits.begin(); }

  /// Returns iterator that points to the beginning of the traits list.
  const_iterator begin() const { return mTraits.begin(); }

  /// Returns iterator that points to the ending of the traits list.
  iterator end() { return mTraits.end(); }

  /// Returns iterator that points to the ending of the traits list.
  const_iterator end() const { return mTraits.end(); }

  /// Returns list of traits.
  llvm::iterator_range<iterator> traits() {
    return llvm::make_range(begin(), end());
  }

  /// Returns list of traits.
  llvm::iterator_range<const_iterator> traits() const {
    return llvm::make_range(begin(), end());
  }

  /// Returns number of traits in region.
  std::size_t size() const { return mTraits.size(); }

  /// Returns `true` if there are no traits in a region/
  bool empty() const { return mTraits.empty(); }

  /// This type used to iterate over calls, each call is represented as a trait
  /// and can be also visited with begin(), end() methods.
  using call_iterator =
    call_iterator_impl<typename TraitSet::persistent_iterator>;

  /// This type used to iterate over calls, each call is represented as a trait
  /// and can be also visited with begin(), end() methods.
  using const_call_iterator =
    call_iterator_impl<typename TraitSet::const_persistent_iterator>;

  /// Returns iterator that points to the beginning of the calls of a specified
  /// function inside a region.
  ///
  /// Note, that 'calls()' method is more effective than a separate calculation
  /// of begin and end iterators.
  call_iterator call_begin(const_func_type_t F) {
    auto I = mCalls.find(F);
    return I == mCalls.end() ? call_iterator() :
      call_iterator(&I->second.front());
  }

  /// Returns iterator that points to the beginning of the calls of a specified
  /// function inside a region.
  ///
  /// Note, that 'calls()' method is more effective than a separate calculation
  /// of begin and end iterators.
  const_call_iterator call_begin(const_func_type_t F) const {
    auto I = mCalls.find(F);
    return I == mCalls.end() ? call_iterator() :
      const_call_iterator(&I->second.front());
  }

  /// Returns iterator that points to the ending of the calls of a specified
  /// function inside a region.
  ///
  /// Note, that 'calls()' method is more effective than a separate calculation
  /// of begin and end iterators.
  call_iterator call_end(const_func_type_t F) {
    auto I = mCalls.find(F);
    return I == mCalls.end() ? call_iterator() :
      call_iterator(&I->second.back() + 1);
  }

  /// Returns iterator that points to the ending of the calls of a specified
  /// function inside a region.
  ///
  /// Note, that 'calls()' method is more effective than a separate calculation
  /// of begin and end iterators.
  const_call_iterator call_end(const_func_type_t F) const {
    auto I = mCalls.find(F);
    return I == mCalls.end() ? call_iterator() :
      const_call_iterator(&I->second.back() + 1);
  }

  /// Returns list of calls of a specified function inside a region.
  ///
  /// Note, that this method is more effective than a separate calculation
  /// of begin and end iterators.
  llvm::iterator_range<call_iterator> calls(const_func_type_t F) {
    auto I = mCalls.find(F);
    return I == mCalls.end() ?
      llvm::make_range(call_iterator(), call_iterator()) :
      llvm::make_range(call_iterator(&I->second.front(),
                 call_iterator(&I->second.back() + 1)));
  }

  /// Returns list of calls of a specified function inside a region.
  ///
  /// Note, that this method is more effective than a separate calculation
  /// of begin and end iterators.
  llvm::iterator_range<const_call_iterator> calls(const_func_type_t F) const {
    auto I = mCalls.find(F);
    return I == mCalls.end() ?
      llvm::make_range(const_call_iterator(), const_call_iterator()) :
      llvm::make_range(const_call_iterator(&I->second.front(),
                 const_call_iterator(&I->second.back() + 1)));
  }

  /// Returns `true` if there are no calls of a specified function inside
  /// a region.
  bool call_empty(const_func_type_t F) const {
    return !mCalls.count(F);
  }

  /// Returns number of calls of a specified function inside a region.
  std::size_t call_size(FuncT F) const {
    auto I = mCalls.find(F);
    return I == mCalls.end() ? 0 : I->second.size();
  }

  /// Add a specified trait, return false if it is already exist.
  ///
  /// Attention, use insert_call() method to add a call.
  std::pair<iterator, bool> insert(const CFTraits &T) {
    return mTraits.insert(T);
  }

  /// Add a specified trait, return false if it is already exist.
  ///
  /// Attention, use insert_call() method to add a call.
  std::pair<iterator, bool> insert(CFTraits &&T) {
    return mTraits.insert(std::move(T));
  }

  /// Add a specified call into a set of traits, return false if it is already
  /// exist.
  std::pair<iterator, bool> insert_call(const FuncT &F, const CFTraits &T) {
    auto Info = mTraits.insert(T);
    if (Info.second) {
      auto &Calls = mCalls.try_emplace(F).first->second;
      Calls.emplace_back(Info.first);
    }
    return Info;
  }

  /// Add a specified call into a set of traits, return false if it is already
  /// exist.
  std::pair<iterator, bool> insert_call(const FuncT &F, CFTraits &&T) {
    auto Info = mTraits.insert(std::move(T));
    if (F && Info.second) {
      auto &Calls = mCalls.try_emplace(F).first->second;
      Calls.emplace_back(Info.first);
    }
    return Info;
  }

  /// Find a trait, note that statement is a key.
  iterator find(const CFTraits &T) { return mTraits.find(T); }

  /// Find a trait, note that statement is a key.
  const_iterator find(const CFTraits &T) const { return mTraits.find(T); }

  /// Find a trait.
  iterator find(const_stmt_type_t S) { return mTraits.find_as(S); }

  /// Find a trait.
  const_iterator find(const_stmt_type_t S) const { return mTraits.find_as(S); }

  /// Remove all traits.
  void clear() {
    mCalls.clear();
    mTraits.clear();
  }

private:
  TraitSet mTraits;
  CallMap mCalls;
};
}

namespace llvm {
/// This per-function pass collects control-flow traits for function and its
/// loops. This pass uses Clang AST to represent traits.
class ClangCFTraitsPass : public FunctionPass, private bcl::Uncopyable {
public:
  using RegionCFInfo = tsar::RegionCFInfoBase<clang::Decl *, clang::Stmt *>;
  using LoopCFInfo = llvm::DenseMap<clang::Stmt *, RegionCFInfo>;
  using CFTraits = typename RegionCFInfo::CFTraits;

  static char ID;

  ClangCFTraitsPass() : FunctionPass(ID) {
    initializeClangCFTraitsPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override {
    mFuncInfo.clear();
    mLoopInfo.clear();
  }

  /// Returns control-flow traits of a function.
  const RegionCFInfo & getFuncInfo() const noexcept { return mFuncInfo; }

  /// Returns control-flow traits for each explicit loop.
  /// TODO (kaniandr@gmail.com): analyze implicit loops.
  const LoopCFInfo & getLoopInfo() const noexcept { return mLoopInfo; }

private:
  RegionCFInfo mFuncInfo;
  LoopCFInfo mLoopInfo;
};
}
#endif//TSAR_CONTROL_FLOW_TRAITS_H
