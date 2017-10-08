//===--- ASTMergeAction.h - AST Merging Frontend Action----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

#include "FrontendActions.h"
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
  bool BeginSourceFileAction(clang::CompilerInstance &CI,
    clang::StringRef Filename) override;
  void ExecuteAction() override;

private:
  /// Imports variable declaration, tentative definitions will be stored in
  /// a specified collection.
  std::pair<clang::Decl *, clang::Decl *> ImportVarDecl(
    clang::VarDecl *V, clang::ASTImporter &Importer,
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

  std::vector<std::string> mASTFiles;
};
}
#endif//TSAR_AST_MERGE_ACTION_H
