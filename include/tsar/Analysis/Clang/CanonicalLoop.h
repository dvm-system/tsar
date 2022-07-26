//=== CanonicalLoop.h --- High Level Canonical Loop Analyzer ----*- C++ -*-===//
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
// This file defines classes to identify canonical for-loops in a source code.
// Canonical Loop Form is described here:
// http://www.openmp.org/wp-content/uploads/openmp-4.5.pdf#page=62
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CANONICAL_LOOP_H
#define TSAR_CANONICAL_LOOP_H

#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Pass.h>
#include <set>

namespace clang {
class ForStmt;
}

namespace llvm {
class SCEV;
class Value;
}

namespace tsar {
class DFLoop;
class DFNode;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

///\brief A Loop syntactically written in canonical form.
///
/// This loop in a source code has a head like
/// `for (/*var initialization*/; /*var comparison*/; /*var increment*/)`.
/// However, some conditions may be semantically violated. To obtain
/// accurate information CanonicalLoopInfo::isCanonical() should be used.
class CanonicalLoopInfo : private bcl::Uncopyable {
  enum Flags : uint8_t {
    DefaultFlags = 0,
    IsCanonical = 1u << 0,
    IsRectangular = 1u << 1,
    IsSigned = 1u << 2,
    IsUnsigned = 1u << 3,
    LLVM_MARK_AS_BITMASK_ENUM(IsUnsigned)
  };
public:
  using Predicate = llvm::CmpInst::Predicate;

  /// Creates information for a specified syntactically canonical loop.
  explicit CanonicalLoopInfo(DFLoop *L, clang::ForStmt *For = nullptr) :
    mLoop(L), mASTLoop(For) {
    assert(L && "Low-level representation of loop must not be null!");
  }

  /// Returns true if this loop is semantically canonical.
  bool isCanonical() const noexcept { return mFlags & IsCanonical; }

  /// Marks this syntactically canonical loop as semantically canonical.
  void markAsCanonical() noexcept { mFlags |= IsCanonical; }

  /// Returns true if the loop bounds are unchanging in the parent loop.
  bool isRectangular() const noexcept { return mFlags & IsRectangular; }

  /// Marks this loop if its loop bounds are unchanging in the parent loop.
  void markAsRectangular() noexcept { mFlags |= IsRectangular; }

  /// Returns low-level representation of the loop.
  DFLoop * getLoop() noexcept { return mLoop; }

  /// Returns low-level representation of the loop.
  const DFLoop * getLoop() const noexcept { return mLoop; }

  /// Returns source-level representation of the loop if available.
  clang::ForStmt * getASTLoop() noexcept { return mASTLoop; }

  /// Returns source-level representation of the loop if available.
  const clang::ForStmt * getASTLoop() const noexcept { return mASTLoop; }

  /// Returns induction variable which is specified in a head of the loop.
  llvm::Value * getInduction() const noexcept { return mInduction; }

  /// Sets induction variable.
  void setInduction(llvm::Value *I) { mInduction = I; }

  llvm::Value * getStart() const noexcept { return mStart; }
  llvm::Value * getEnd() const noexcept { return mEnd; }
  const llvm::SCEV * getStep() const noexcept { return mStep; }

  void setStart(llvm::Value *Start) noexcept { mStart = Start; }
  void setEnd(llvm::Value *End) noexcept { mEnd = End; }
  void setStep(const llvm::SCEV *Step) noexcept { mStep = Step; }

  /// Returns comparison predicate.
  Predicate getPredicate() const noexcept { return mPredicate; }

  /// Set comparison predicate.
  void setPredicate(Predicate P) noexcept { mPredicate = P; }

  /// Returns true if comparison is signed.
  bool isSigned() const { return mFlags & IsSigned; }
  void markAsSigned() { mFlags &= ~IsUnsigned; mFlags |= IsSigned; }

  /// Returns true if comparison is unsigned.
  bool isUnsigned() const { return mFlags & IsUnsigned; }
  void markAsUnsigned() { mFlags &= ~IsSigned; mFlags |= IsUnsigned; }

  /// Drops sign property for comparison operation.
  void markAsUnknownSign() { mFlags &= ~(IsSigned | IsUnsigned); }

private:
  DFLoop *mLoop;
  clang::ForStmt *mASTLoop;
  Flags mFlags = DefaultFlags;
  llvm::Value *mInduction = nullptr;
  llvm::Value *mStart = nullptr;
  llvm::Value *mEnd = nullptr;
  const llvm::SCEV *mStep = nullptr;
  Predicate mPredicate = Predicate::BAD_ICMP_PREDICATE;
};

/// Replacement of default llvm::DenseMapInfo<CanonicalLoopInfo *>.
struct CanonicalLoopMapInfo {
  static inline CanonicalLoopInfo * getEmptyKey() {
    return llvm::DenseMapInfo<CanonicalLoopInfo *>::getEmptyKey();
  }
  static inline CanonicalLoopInfo * getTombstoneKey() {
    return llvm::DenseMapInfo<CanonicalLoopInfo *>::getTombstoneKey();
  }
  static inline unsigned getHashValue(const CanonicalLoopInfo *LI) {
    return llvm::DenseMapInfo<const DFNode *>
      ::getHashValue(reinterpret_cast<const DFNode *>(LI->getLoop()));
  }
  static inline unsigned getHashValue(const DFNode *N) {
    return llvm::DenseMapInfo<const DFNode *>::getHashValue(N);
  }
  static inline bool isEqual(
    const CanonicalLoopInfo *LHS, const CanonicalLoopInfo *RHS) {
    return LHS == RHS;
  }
  static inline bool isEqual(
    const DFNode*LHS, const CanonicalLoopInfo *RHS) {
    return !isEqual(RHS, getEmptyKey()) && !isEqual(RHS, getTombstoneKey())
      && LHS == reinterpret_cast<const DFNode *>(RHS->getLoop());
  }
};
}

namespace tsar {
///\brief Set of loops syntactically written in canonical form.
///
/// Any loop in a source code is presented in this set if it has header like
/// `for (/*var initialization*/; /*var comparison*/; /*var increment*/)`.
/// However, some conditions may be semantically violated. To obtain
/// accurate information CanonicalLoopInfo::isCanonical() should be used.
using CanonicalLoopSet =
llvm::DenseSet<const CanonicalLoopInfo *, CanonicalLoopMapInfo>;
}

namespace llvm {
/// \brief This pass determines canonical for-loops in a source code.
///
/// A for-loop is treated as canonical if it has header like
/// for (/*var initialization*/; /*var comparison*/; /*var increment*/)
class CanonicalLoopPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  CanonicalLoopPass() : FunctionPass(ID) {
    initializeCanonicalLoopPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about loops for an analyzed function.
  const tsar::CanonicalLoopSet & getCanonicalLoopInfo() const noexcept {
    return mCanonicalLoopInfo;
  }

  /// Determines canonical loops in a specified functions.
  bool runOnFunction(Function &F) override;

  /// Deletes information about analyzed loops.
  void releaseMemory() override {
    for (auto *LI : mCanonicalLoopInfo)
      delete LI;
    mCanonicalLoopInfo.clear();
  }

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Prints out the internal state of the pass. This also used to produce
  /// analysis correctness tests.
  void print(raw_ostream &OS, const Module *M) const override;

private:
  tsar::CanonicalLoopSet mCanonicalLoopInfo;
};
}
#endif// TSAR_CANONICAL_LOOP_H
