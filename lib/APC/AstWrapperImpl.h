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
#include <bcl/Equation.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/Optional.h>

struct LoopGraph;
struct FuncInfo;

namespace Distribution {
class Array;
}

namespace tsar {
class APCContext;
}

class Statement;

class Symbol {
public:
  using Redeclarations = llvm::SmallVector<tsar::dvmh::VariableT, 1>;

  Symbol(tsar::APCContext *Ctx, tsar::dvmh::VariableT DIM)
      : mContext(Ctx), mMemory(Redeclarations{DIM}) {
    assert(Ctx && "Context must not be null!");
  }
  Symbol(tsar::APCContext *Ctx, tsar::dvmh::Template *DIM)
      : mContext(Ctx), mMemory(DIM) {
    assert(Ctx && "Context must not be null!");
  }
  Symbol(tsar::APCContext *Ctx, Statement *S)
      : mContext(Ctx), mMemory(S) {
    assert(Ctx && "Context must not be null!");
  }

  virtual ~Symbol() {}

  bool isTemplate() const {
    return std::holds_alternative<tsar::dvmh::Template *>(mMemory);
  }

  auto *getTemplate() { return std::get<tsar::dvmh::Template *>(mMemory); }
  const auto *getTemplate() const {
    return std::get<tsar::dvmh::Template *>(mMemory);
  }

  bool isStatement() const {
    return std::holds_alternative<Statement *>(mMemory);
  }

  auto *getStatement() { return std::get<Statement *>(mMemory); }
  const auto *getStatement() const {
    return std::get<Statement *>(mMemory);
  }

  bool isVariable() const {
    return std::holds_alternative<Redeclarations>(mMemory);
  }

  auto &getVariable() { return std::get<Redeclarations>(mMemory); }
  const auto &getVariable() const { return std::get<Redeclarations>(mMemory); }

  llvm::Optional<tsar::dvmh::VariableT>
  getVariable(const llvm::Function *F) const {
    assert(F && "Function must be specified!");
    auto DIMItr{find_if(getVariable(), [F](auto Var) {
      auto &MH{Var.template get<tsar::MD>()};
      return F == &MH->getAliasNode()->getAliasTree()->getFunction();
    })};
    if (DIMItr == getVariable().end())
      return llvm::None;
    return *DIMItr;
  }

  void addRedeclaration(tsar::dvmh::VariableT Var) {
    assert(!isTemplate() && "Unable to attach memory to a template!");
    assert(getVariable().front().get<tsar::MD>()->getAsMDNode() ==
               Var.get<tsar::MD>()->getAsMDNode() &&
           "Memory locations must have the same raw implementation!");
    std::get<Redeclarations>(mMemory).push_back(Var);

  }

  tsar::APCContext *getContext() noexcept { return mContext; }
  const tsar::APCContext *getContext() const noexcept { return mContext; }
private:
  tsar::APCContext *mContext;
  std::variant<Redeclarations, tsar::dvmh::Template *, Statement *> mMemory;
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
  struct AffineSubscript {
    AffineSubscript(llvm::APSInt A, LoopGraph *I, llvm::APSInt B) {
      assert(I && "Induction variable must not be null!");
      mConstants.push_back(std::move(B));
      mConstants.push_back(std::move(A));
      mLoops.push_back(I);
    }

    void emplace_back(llvm::APSInt A, LoopGraph *I) {
      assert(I && "Induction variable must not be null!");
      mConstants.push_back(std::move(A));
      mLoops.push_back(I);
    }

    unsigned size() const { return mLoops.size(); }

    auto getMonom(unsigned Idx) {
      return milp::AMonom<LoopGraph *, llvm::APSInt>{mLoops[Idx],
                                                     mConstants[Idx + 1]};
    }

    const auto &getConstant() { return mConstants.front(); }

    llvm::ArrayRef<LoopGraph *> getRec() const { return mLoops; }
  private:
    llvm::SmallVector<llvm::APSInt, 2> mConstants;
    RecurrenceList mLoops;
  };

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
    if (auto AS{std::get_if<AffineSubscript>(&mRecInDim[DimIdx])})
      return AS->getRec();
    return llvm::ArrayRef<LoopGraph *>{};
  }

  bool isAffineSubscript(unsigned DimIdx) const {
    return std::holds_alternative<AffineSubscript>(mRecInDim[DimIdx]);
  }

  void setAffineSubscript(unsigned DimIdx, AffineSubscript S) {
    mRecInDim[DimIdx] = std::move(S);
  }

  const AffineSubscript &getAffineSubscript(unsigned DimIdx) const {
    assert(isAffineSubscript(DimIdx) && "Subscript expression must be affine!");
    return std::get<AffineSubscript>(mRecInDim[DimIdx]);
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
  llvm::SmallVector<std::variant<RecurrenceList, AffineSubscript, llvm::APSInt>,
                    4>
      mRecInDim;
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
      bcl::tagged<tsar::dvmh::SortedVarListT, tsar::trait::Local>,
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

  void addImmediateAccess(ArrayRefExp *A) {
    assert(A && "Access must not be null!");
    mImmediateAccess.push_back(A);
  }
  llvm::ArrayRef<ArrayRefExp *> getImmediateAccesses() const noexcept {
    return mImmediateAccess;
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
  std::vector<ArrayRefExp *> mImmediateAccess;
};
}
#endif//TSAR_AST_WRAPPER_IMPL_H
