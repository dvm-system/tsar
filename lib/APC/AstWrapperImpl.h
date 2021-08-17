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

class Symbol {
public:
  explicit Symbol(tsar::dvmh::VariableT DIM) : mMemory(DIM) {}
  explicit Symbol(tsar::dvmh::Template *DIM) : mMemory(DIM) {}

  const auto & getMemory() const noexcept { return mMemory; }
private:
  std::variant<tsar::dvmh::VariableT, tsar::dvmh::Template *> mMemory;
};

class Expression {
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

  LoopStatement(llvm::Function *F, tsar::ObjectID Id, tsar::dvmh::VariableT I)
      : Statement(KIND_LOOP), mFunction(F), mId(Id), mInduction(I) {}

  const tsar::dvmh::VariableT &getInduction() const noexcept {
    return mInduction;
  }
  tsar::ObjectID getId() const noexcept { return mId; }

  llvm::Function *getFunction() noexcept { return mFunction; }
  llvm::Function *getFunction() const noexcept { return mFunction; }

  TraitList &getTraits() noexcept { return mTraits; }
  const TraitList &getTraits() const noexcept { return mTraits; }

  bool isHostOnly() const noexcept { return mIsHostOnly; }
  void setIsHostOnly(bool IsHostOnly) noexcept { mIsHostOnly = IsHostOnly; }

  unsigned getPossibleAcrossDepth() const noexcept {
    return mPossibleAcrossDepth;
  }

  void setPossibleAcrossDepth(unsigned Depth) noexcept {
    mPossibleAcrossDepth = Depth;
  }

private:
  llvm::Function *mFunction{nullptr};
  tsar::ObjectID mId{nullptr};
  tsar::dvmh::VariableT mInduction;
  TraitList mTraits;
  bool mIsHostOnly{true};
  unsigned mPossibleAcrossDepth{0};
};
}

#endif//TSAR_AST_WRAPPER_IMPL_H
