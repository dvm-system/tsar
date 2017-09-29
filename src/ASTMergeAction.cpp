//===- ASTMergeAction.cpp - AST Merging Frontend Action----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements frontend action adapter which merges ASTs together.
//
//===----------------------------------------------------------------------===//

#include "ASTMergeAction.h"
#include <clang/Frontend/ASTUnit.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTDiagnostic.h>
#include <clang/AST/ASTImporter.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/ADT/SmallPtrSet.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace tsar {
void ASTMergeAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  CI.getDiagnostics().getClient()->BeginSourceFile(
    CI.getASTContext().getLangOpts());
  CI.getDiagnostics().SetArgToStringFn(&FormatASTNodeDiagnosticArgument,
    &CI.getASTContext());
  IntrusiveRefCntPtr<DiagnosticIDs>
    DiagIDs(CI.getDiagnostics().getDiagnosticIDs());
  std::vector<VarDecl *> TentativeDefinitions;
  for (unsigned I = 0, N = mASTFiles.size(); I != N; ++I) {
    IntrusiveRefCntPtr<DiagnosticsEngine>
      Diags(new DiagnosticsEngine(DiagIDs, &CI.getDiagnosticOpts(),
        new ForwardingDiagnosticConsumer(
          *CI.getDiagnostics().getClient()), /*ShouldOwnClient=*/true));
    std::unique_ptr<ASTUnit> Unit =
      ASTUnit::LoadFromASTFile(mASTFiles[I], CI.getPCHContainerReader(),
        Diags, CI.getFileSystemOpts(), false);
    if (!Unit)
      continue;
    ASTImporter Importer(CI.getASTContext(), CI.getFileManager(),
      Unit->getASTContext(), Unit->getFileManager(), /*MinimalImport=*/false);
    TranslationUnitDecl *TU = Unit->getASTContext().getTranslationUnitDecl();
    for (auto *D : TU->decls()) {
      // Don't re-import __va_list_tag, __builtin_va_list.
      if (const auto *ND = dyn_cast<NamedDecl>(D))
        if (IdentifierInfo *II = ND->getIdentifier())
          if (II->isStr("__va_list_tag") || II->isStr("__builtin_va_list"))
            continue;
      if (const auto *FuncDecl = dyn_cast<FunctionDecl>(D)) {
        // It is not safe to import prototype. In this case parameters from
        // prototype will be imported but body will be imported from definition.
        // This leads to loss of information about parameters in the body.
        // Parameters in the definition and prototype does not linked together.
        FuncDecl->hasBody(FuncDecl);
        D = const_cast<FunctionDecl *>(FuncDecl);
      }
      Decl *ToD = Importer.Import(D);
      if (ToD) {
        DeclGroupRef DGR(ToD);
        CI.getASTConsumer().HandleTopLevelDecl(DGR);
        if (isa<VarDecl>(ToD) &&
            cast<VarDecl>(ToD)->isThisDeclarationADefinition() ==
              VarDecl::TentativeDefinition)
          TentativeDefinitions.push_back(cast<VarDecl>(ToD));
      }
    }
  }
  // Note LLVM IR will not be generated for tentative definitions
  // without call of ASTConsumer::CompleteTentativeDefinition() function.
  SmallPtrSet<VarDecl *, 32> Seen;
  for (auto *V : TentativeDefinitions) {
    VarDecl *VD = V->getActingDefinition();
    if (!VD || VD->isInvalidDecl() || !Seen.insert(VD).second)
      continue;
    CI.getASTConsumer().CompleteTentativeDefinition(VD);
  }
  WrapperFrontendAction::ExecuteAction();
  CI.getDiagnostics().getClient()->EndSourceFile();
}

bool ASTMergeAction::BeginSourceFileAction(
    CompilerInstance &CI, StringRef Filename) {
  /// TODO(kaniandr@gmail.com): This is a hack. It is necessary to set ASTUnit
  /// for an action to be wrapped, but WrapperFrontendAction is set it to null.
  /// So we believe that it is safe to set it after BeginSourceFileAction() call
  /// for the wrapped action. Note, that this is protected member, so only
  /// WrapperFrontendAction can access it.
  /// Note that WrapperFrontendAction will unset ASTUnit for both (current and
  /// wrapped actions), so it is necessary to save ASTUnit.
  auto ASTUnit = takeCurrentASTUnit();
  auto Ret = WrapperFrontendAction::BeginSourceFileAction(CI, Filename);
  getWrappedAction().setCurrentInput(getCurrentInput(), std::move(ASTUnit));
  return Ret;
}

ASTMergeAction::ASTMergeAction(
    std::unique_ptr<clang::FrontendAction> WrappedAction,
    clang::ArrayRef<std::string> ASTFiles) :
  PublicWrapperFrontendAction(WrappedAction.release()),
  mASTFiles(ASTFiles.begin(), ASTFiles.end()) {}
}
