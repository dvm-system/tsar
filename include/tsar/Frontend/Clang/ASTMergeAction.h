//===--- ASTMergeAction.h - AST Merging Frontend Action----------*- C++ -*-===//
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
// This file implements frontend action adapter which merges ASTs together.
// Proposed implementation is similar to clang::ASTMergeAction, however it
// contains some improvements which are necessary for merging arbitrary source
// files (.c, .cpp).
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_AST_MERGE_ACTION_H
#define TSAR_AST_MERGE_ACTION_H

#include "tsar/Frontend/Clang/FrontendActions.h"
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/DenseMap.h>
#include <vector>

namespace clang {
class ASTImporter;
class ASTUnit;
class Decl;
class VarDecl;
}

namespace tsar {
/// \brief Frontend action adapter that merges ASTs together.
///
/// This action takes an existing AST file and "merges" it into the AST
/// context, producing a merged context. This action is an action
/// adapter, which forwards most of its calls to another action that
/// will consume the merged context.
class ASTMergeAction : public PublicWrapperFrontendAction {
public:
  /// Creates adapter for a specified action, this adapter merge all
  /// files from a specified set.
  ASTMergeAction(std::unique_ptr<clang::FrontendAction> WrappedAction,
    clang::ArrayRef<std::string> ASTFiles);

  /// This action can not evaluate LLVM IR.
  bool hasIRSupport() const override { return false; }

protected:
  bool BeginSourceFileAction(clang::CompilerInstance &CI) override;
  void ExecuteAction() override;

  /// Allocates new importer.
  virtual clang::ASTImporter * newImporter(
    clang::ASTContext &ToContext, clang::FileManager &ToFileManager,
    clang::ASTContext &FromContext, clang::FileManager &FromFileManager,
    bool MinimalImport);

  /// Imports variable declaration, tentative definitions will be stored in
  /// a specified collection.
  std::pair<clang::Decl *, clang::Decl *> ImportVarDecl(
    clang::VarDecl *FromV, clang::ASTImporter &Importer,
    std::vector<clang::VarDecl *> &TentativeDefinitions) const;

  /// Imports function declaration.
  std::pair<clang::Decl *, clang::Decl *> ImportFunctionDecl(
    clang::FunctionDecl *FromF, clang::ASTImporter &Importer) const;

  /// Prepares to import a specified unit.
  ///
  /// For example, try to perform manual imported of objects which can not be
  /// successfully processed by clang::ASTImporter.
  void PrepareToImport(clang::ASTUnit &Unit, clang::DiagnosticsEngine &Diags,
    clang::ASTImporter &Importer) const;

  /// Updates AST after successful import if necessary.
  void FinalizeImport(clang::ASTContext &ToContext,
    clang::DiagnosticsEngine &Diags) const;

  std::vector<std::string> mASTFiles;
};

struct ASTImportInfo;

/// Frontend action adapter that merges ASTs together and store some
/// information about the import process in a specified external storage.
class ASTMergeActionWithInfo : public ASTMergeAction {
public:
  /// Creates adapter for a specified action, this adapter merge all
  /// files from a specified set and store some information about the import
  /// process in a specified external storage.
  ASTMergeActionWithInfo(std::unique_ptr<clang::FrontendAction> WrappedAction,
      clang::ArrayRef<std::string> ASTFiles, ASTImportInfo *Out) :
    ASTMergeAction(std::move(WrappedAction), ASTFiles), mImportInfo(Out) {
    assert(mImportInfo && "External storage must not be null!");
  }

protected:
  /// Allocates new importer.
  virtual clang::ASTImporter * newImporter(
    clang::ASTContext &ToContext, clang::FileManager &ToFileManager,
    clang::ASTContext &FromContext, clang::FileManager &FromFileManager,
    bool MinimalImport) override;

private:
  ASTImportInfo *mImportInfo;
};
}
#endif//TSAR_AST_MERGE_ACTION_H
