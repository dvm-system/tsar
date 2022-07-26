//===- TransformationContext.h - TSAR Transformation Engine (Clang) C++ -*-===//
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
// This file defines Clang-based source level transformation engine which.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_TRANSFORMATION_CONTEXT_H
#define TSAR_CLANG_TRANSFORMATION_CONTEXT_H

#include "tsar/Core/TransformationContext.h"
#include <clang/Rewrite/Core/Rewriter.h>

namespace clang {
class Decl;
class CodeGenerator;
class CompilerInstance;
class DiagnosticsEngine;
class ASTContext;
}

namespace tsar {
class ClangTransformationContext : public TransformationContextBase {
public:
  static bool classof(const TransformationContextBase *Ctx) noexcept {
    return Ctx->getKind() == TC_Clang;
  }

  ClangTransformationContext(clang::CompilerInstance &CI,
                             clang::ASTContext &Ctx, clang::CodeGenerator &Gen);

  /// Return an input source file.
  ///
  /// \pre Transformation instance must be configured.
  llvm::StringRef getInput() const;

  /// Return rewriter.
  clang::Rewriter & getRewriter() {
    assert(hasInstance() && "Context is not configured!");
    return mRewriter;
  }

  /// Return context.
  clang::ASTContext & getContext() {
    assert(hasInstance() && "Context is not configured!");
    return *mCtx;
  }

  /// Return context.
  const clang::ASTContext & getContext() const {
    assert(hasInstance() && "Context is not configured!");
    return *mCtx;
  }

  /// Return compiler instance.
  const clang::CompilerInstance & getCompilerInstance() const {
    assert(hasInstance() && "Context is not configured!");
    return *mCI;
  }

  /// \brief Returns a declaration for a mangled name.
  ///
  /// \pre Transformation instance must be configured.
  clang::Decl * getDeclForMangledName(llvm::StringRef Name);

  /// Returns true if transformation engine is configured.
  bool hasInstance() const override { return mGen && mCtx && mCI; }

  /// Returns true if modifications have been made to some files.
  bool hasModification() const override {
    return hasInstance() &&
      mRewriter.buffer_begin() != mRewriter.buffer_end();
  }

  /// \brief Save all changed files to disk.
  ///
  /// \param FA Filename adjuster which modifies names of files where the
  /// changes must be saved. The parameter for an adjuster is a name of a
  /// modified file.
  /// \return Pair of values. The first value is the name of file where main
  /// input file has been saved. The second value is 'true' if all changes
  /// have been successfully saved.
  std::pair<std::string, bool> release(
    const FilenameAdjuster &FA = getDumpFilenameAdjuster()) override;

  /// Save all changes in a specified buffer to disk (in file with a specified
  /// name), emits diagnostic messages in case of error.
  void release(llvm::StringRef Filename, const clang::RewriteBuffer &Buffer);

  /// Reset existence configuration of transformation engine.
  ///
  /// \post Transformation engine is configured.
  void reset(clang::CompilerInstance &CI, clang::ASTContext &Ctx,
      clang::CodeGenerator &Gen);

  /// Reset existence configuration of transformation engine.
  ///
  /// \post Transformation engine is NOT configured.
  void reset() { mGen = nullptr; mCtx = nullptr; mCI = nullptr; }

private:
  clang::Rewriter mRewriter;
  clang::CompilerInstance *mCI = nullptr;
  clang::CodeGenerator *mGen = nullptr;
  clang::ASTContext *mCtx = nullptr;
};
}

#endif//TSAR_CLANG_TRANSFORMATION_CONTEXT_H
