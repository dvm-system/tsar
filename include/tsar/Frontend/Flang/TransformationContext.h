//===- TransformationContext.h - TSAR Transformation Engine (Flang) C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file defines Flang-based source level transformation engine which.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_TRANSFORMATION_CONTEXT_H
#define TSAR_FLANG_TRANSFORMATION_CONTEXT_H

#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Flang/Rewriter.h"
#include <flang/Parser/parsing.h>
#include <flang/Semantics/semantics.h>
#include <llvm/ADT/StringMap.h>

namespace llvm {
class Module;
class DICompileUnit;
}

namespace tsar {
class FlangTransformationContext : public TransformationContextBase {
  using MangledToSourceMapT = llvm::StringMap<Fortran::semantics::Symbol *>;
public:
  static bool classof(const TransformationContextBase *Ctx) noexcept {
    return Ctx->getKind() == TC_Flang;
  }

  FlangTransformationContext(const Fortran::parser::Options &Opts,
      const Fortran::common::IntrinsicTypeDefaultKinds &DefaultKinds)
    : TransformationContextBase(TC_Flang)
    , mOptions(Opts)
    , mContext(DefaultKinds, Opts.features, mAllSources) {}

  void initialize(const llvm::Module &M, const llvm::DICompileUnit &CU);

  bool hasInstance() const override {
    auto *This{const_cast<FlangTransformationContext *>(this)};
    return This->mParsing.parseTree().has_value() &&
           !This->mParsing.messages().AnyFatalError() &&
           !This->mContext.AnyFatalError() &&
           mRewriter;
  }

  bool hasModification() const override {
    return hasInstance() && mRewriter->hasModification();
  }

  std::pair<std::string, bool> release(
      const FilenameAdjuster &FA = getDumpFilenameAdjuster()) override;

  auto &getParsing() noexcept { return mParsing; }
  const auto &getParsing() const noexcept { return mParsing; }

  const auto &getOptions() const noexcept { return mOptions; }

  auto &getContext() noexcept { return mContext; }
  const auto &getContext() const noexcept { return mContext; }

  auto &getRewriter() {
    assert(hasInstance() && "Transformation context is not configured!");
    return *mRewriter;
  }

  const auto &getRewriter() const {
    assert(hasInstance() && "Transformation context is not configured!");
    return *mRewriter;
  }

  /// Return a declaration for a mangled name.
  ///
  /// \pre Transformation instance must be configured.
  Fortran::semantics::Symbol * getDeclForMangledName(llvm::StringRef Name) {
    assert(hasInstance() && "Transformation context is not configured!");
    auto I = mGlobals.find(Name);
    return (I != mGlobals.end()) ? I->getValue() : nullptr;
  }

private:
  Fortran::parser::AllSources mAllSources;
  Fortran::parser::Options mOptions;
  Fortran::parser::Parsing mParsing{mAllSources};
  Fortran::semantics::SemanticsContext mContext;
  MangledToSourceMapT mGlobals;
  std::unique_ptr<FlangRewriter> mRewriter{nullptr};
};
}

#endif//TSAR_FLANG_TRANSFORMATION_CONTEXT_H
