//===- AstWrapperImpl.h - LLVM Based Source Level Information ---*- C++ -*-===//
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
// This file implements classes which are declared and used in external APC
// library. These classes propose access to a source-level information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_AST_WRAPPER_IMPL_H
#define TSAR_AST_WRAPPER_IMPL_H

#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/Optional.h>

struct LoopGraph;
struct FuncInfo;

namespace Distribution{
class Array;
}

namespace tsar {
class APCContext;
}

class Symbol {
public:
  explicit Symbol(tsar::APCContext *Ctx, tsar::dvmh::VariableT DIM)
      : mContext(Ctx), mMemory(DIM) {
    assert(Ctx && "Context must not be null!");
  }
  explicit Symbol(tsar::APCContext *Ctx, tsar::dvmh::Template *DIM)
      : mContext(Ctx), mMemory(DIM) {
    assert(Ctx && "Context must not be null!");
  }

  virtual ~Symbol() {}

  const auto & getMemory() const noexcept { return mMemory; }

  tsar::APCContext *getContext() noexcept { return mContext; }
  const tsar::APCContext *getContext() const noexcept { return mContext; }
private:
  tsar::APCContext *mContext;
  std::variant<tsar::dvmh::VariableT, tsar::dvmh::Template *> mMemory;
};

class File {};

class Expression {
public:
  explicit Expression(tsar::APCContext *Ctx)
      : mContext(Ctx), mScope((FuncInfo *)(nullptr)) {
    assert(Ctx && "Context must not be null!");
  }

  Expression(tsar::APCContext *Ctx, std::variant<LoopGraph *, FuncInfo *> S)
      : mContext(Ctx), mScope(S) {
    assert(Ctx && "Context must not be null!");
  }

  Expression(const Expression &) = default;
  Expression(Expression &&) = default;

  Expression &operator=(const Expression &) = default;
  Expression &operator=(Expression &&) = default;

  virtual ~Expression() {}

  tsar::APCContext *getContext() noexcept { return mContext; }
  const tsar::APCContext *getContext() const noexcept { return mContext; }

  /// Return 'true' if this expression has been attached to any scope.
  bool isInScope() const {
    bool InScope{ false };
    std::visit([&InScope](auto *S) { InScope |= (S != nullptr); }, mScope);
    return InScope;
  }

  bool isInLoop() const {
    return isInScope() && std::holds_alternative<LoopGraph *>(mScope);
  }
  auto getScope() { return mScope; }

private:
  tsar::APCContext *mContext;
  std::variant<LoopGraph *, FuncInfo *> mScope;
};

class ArrayRefExp : public Expression {
  using RecurrenceList = llvm::SmallVector<LoopGraph *, 1>;
public:
  ArrayRefExp(tsar::APCContext *Ctx, Distribution::Array *A,
              std::size_t NumOfSubscripts)
      : Expression(Ctx), mArray(A), mRecInDim(NumOfSubscripts) {
    assert(A && "Array must not be null!");
  }

  ArrayRefExp(tsar::APCContext *Ctx, std::variant<LoopGraph *, FuncInfo *> S,
              Distribution::Array *A, std::size_t NumOfSubscripts)
      : Expression(Ctx, S), mArray(A), mRecInDim(NumOfSubscripts) {
    assert(A && "Array must not be null!");
  }

  auto *getArray() noexcept { return mArray; }
  const auto *getArray() const noexcept { return mArray; }

  void registerRecInDim(unsigned DimIdx, LoopGraph *L) {
    assert(DimIdx < mRecInDim.size() && "A dimension number is out of range!");
    if (!std::holds_alternative<RecurrenceList>(mRecInDim[DimIdx])) {
      mRecInDim[DimIdx] = RecurrenceList{};
    }
    auto &RL{std::get<RecurrenceList>(mRecInDim[DimIdx])};
    if (!is_contained(RL, L))
      RL.push_back(L);
  }

  llvm::ArrayRef<LoopGraph *> getRecInDim(unsigned DimIdx) const {
    if (auto RL{std::get_if<RecurrenceList>(&mRecInDim[DimIdx])})
      return *RL;
    return llvm::ArrayRef<LoopGraph *>{};
  }

  bool isConstantSubscript(unsigned DimIdx) const {
    return std::holds_alternative<llvm::APSInt>(mRecInDim[DimIdx]);
  }

  void setConstantSubscript(unsigned DimIdx, const llvm::APSInt &C) {
    mRecInDim[DimIdx] = C;
  }

  llvm::APSInt getConstantSubscript(unsigned DimIdx) const {
    assert(isConstantSubscript(DimIdx) &&
           "Subscript expression must be constant!");
    return std::get<llvm::APSInt>(mRecInDim[DimIdx]);
  }

  /// Return number of subscripts.
  unsigned size() const { return mRecInDim.size(); }

  /// Return true if there are no subscript expressions.
  bool empty() const { return mRecInDim.empty(); }

private:
  Distribution::Array *mArray;
  llvm::SmallVector<std::variant<RecurrenceList, llvm::APSInt>, 4> mRecInDim;
};

class Statement {
public:
  enum Kind {
    FIRST_KIND = 0,
    KIND_LOOP = FIRST_KIND,
    LAST_KIND = KIND_LOOP,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };

  virtual ~Statement() {}

  Kind getKind() const noexcept { return mKind; }

protected:
  explicit Statement(Kind K) : mKind(K) {}

private:
  Kind mKind{ INVALID_KIND};
};

namespace llvm{
class Function;
}

namespace apc {
class LoopStatement : public Statement {
public:
  using TraitList = bcl::tagged_tuple<
      bcl::tagged<tsar::dvmh::ShadowVarListT, tsar::trait::Dependence>,
      bcl::tagged<tsar::dvmh::SortedVarListT, tsar::trait::Private>,
      bcl::tagged<tsar::dvmh::ReductionVarListT, tsar::trait::Reduction>,
      bcl::tagged<tsar::dvmh::SortedVarListT, tsar::trait::ReadOccurred>,
      bcl::tagged<tsar::dvmh::SortedVarListT, tsar::trait::WriteOccurred>>;

  static bool classof(const Statement *R) {
    return R->getKind() == KIND_LOOP;
  }

  LoopStatement(llvm::Function *F, tsar::ObjectID Id, LoopGraph *L)
      : Statement(KIND_LOOP), mFunction(F), mId(Id), mLoop(L) {
    assert(F && "Function must not be null!");
    assert(L && "Loop must not be null!");
    mInduction.get<tsar::trait::Induction>() = {nullptr, nullptr};
  }

  LoopStatement(llvm::Function *F, tsar::ObjectID Id, LoopGraph *L,
                tsar::dvmh::InductionInfo I)
      : Statement(KIND_LOOP), mFunction(F), mId(Id), mLoop(L), mInduction(I) {
    assert(F && "Function must not be null!");
    assert(L && "Loop must not be null!");
  }

  bool hasInduction() const noexcept {
    return mInduction.get<tsar::trait::Induction>().get<tsar::AST>() &&
           mInduction.get<tsar::trait::Induction>().get<tsar::MD>();
  }
  const tsar::dvmh::VariableT &getInduction() const noexcept {
    return mInduction.get<tsar::trait::Induction>();
  }
  tsar::ObjectID getId() const noexcept { return mId; }

  llvm::Function *getFunction() noexcept { return mFunction; }
  llvm::Function *getFunction() const noexcept { return mFunction; }

  LoopGraph *getLoop() noexcept { return mLoop; }
  const LoopGraph *getLoop() const noexcept { return mLoop; }

  TraitList &getTraits() noexcept { return mTraits; }
  const TraitList &getTraits() const noexcept { return mTraits; }

  bool isHostOnly() const noexcept { return mIsHostOnly; }
  void setIsHostOnly(bool IsHostOnly) noexcept { mIsHostOnly = IsHostOnly; }

  bool isScheduled() const noexcept { return mIsScheduledToParallelization; }
  void scheduleToParallelization(bool Is = true) noexcept {
    mIsScheduledToParallelization = Is;
  };

  unsigned getPossibleAcrossDepth() const noexcept {
    return mPossibleAcrossDepth;
  }

  void setPossibleAcrossDepth(unsigned Depth) noexcept {
    mPossibleAcrossDepth = Depth;
  }

  const llvm::Optional<llvm::APSInt> &getStart() const noexcept {
    return mInduction.get<tsar::Begin>();
  }
  const llvm::Optional<llvm::APSInt> &getStep() const noexcept {
    return mInduction.get<tsar::Step>();
  }
  const llvm::Optional<llvm::APSInt> &getEnd() const noexcept {
    return mInduction.get<tsar::End>();
  }

private:
  llvm::Function *mFunction{nullptr};
  tsar::ObjectID mId{nullptr};
  LoopGraph *mLoop{nullptr};
  tsar::dvmh::InductionInfo mInduction;
  TraitList mTraits;
  bool mIsHostOnly{true};
  bool mIsScheduledToParallelization{false};
  unsigned mPossibleAcrossDepth{0};
};
}
#endif//TSAR_AST_WRAPPER_IMPL_H
