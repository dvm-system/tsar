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
#include <clang/Sema/SemaDiagnostic.h>
#include <llvm/ADT/SmallPtrSet.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace tsar {
std::pair<Decl *, Decl *> ASTMergeAction::ImportVarDecl(VarDecl *FromV,
    ASTImporter &Importer, std::vector<VarDecl *> &TentativeDefinitions) {
  auto ToD = Importer.Import(FromV);
  if (!ToD)
    return std::make_pair(FromV, nullptr);
  auto ToV = cast<VarDecl>(ToD);
  unsigned IDNS = Decl::IDNS_Ordinary;
  SmallVector<NamedDecl *, 2> FoundDecls;
  // The following loop checks for redefinitions of a variable. ASTImporter
  // can import a variable if a function with the same name has been imported
  // previously. However, it is not possible to generate LLVM IR in this case.
  // Note that it is not possible to import a function if a variable has been
  // already imported (ASTImporter checks this case).
  ToV->getDeclContext()->getRedeclContext()->localUncachedLookup(
    ToV->getDeclName(), FoundDecls);
  for (unsigned I = 0, N = FoundDecls.size(); I != N; ++I) {
    NamedDecl *OldD = FoundDecls[I];
    if (!OldD->isInIdentifierNamespace(IDNS) ||
        isa<VarDecl>(OldD))
      continue;
    Importer.ToDiag(ToV->getLocation(), diag::err_redefinition_different_kind)
      << ToV->getDeclName();
    if (OldD->getLocation().isValid())
      Importer.ToDiag(OldD->getLocation(), diag::note_previous_definition);
    return std::make_pair(FromV, nullptr);
  }
  switch (ToV->isThisDeclarationADefinition()) {
  case VarDecl::TentativeDefinition:
    TentativeDefinitions.push_back(ToV); break;
  case VarDecl::DeclarationOnly:
    // Let us check that the result of previously imported declaration
    // which is similar to FromV is used but some data is lost.
    // Let us consider an example.
    //   extern int X; // (1)
    //   int X; // (2)
    // At first, (1) will be imported and ToV will be constructed.
    // So result of import of (2) is this ToV (new VarDecl will not be
    // constructed). However the storage class of ToV is extern and
    // in LLVM IR it will be external location without definition. To
    // solve this problem additional VarDecl can be created which has
    // the same storage class as imported declaration (2).
    if (FromV->isThisDeclarationADefinition() ==
        VarDecl::TentativeDefinition) {
      VarDecl *ToTentative = VarDecl::Create(
        ToV->getASTContext(), ToV->getDeclContext(),
        Importer.Import(FromV->getInnerLocStart()),
        Importer.Import(FromV->getLocation()),
        ToV->getDeclName().getAsIdentifierInfo(),
        ToV->getType(), ToV->getTypeSourceInfo(),
        FromV->getStorageClass());
      ToTentative->setQualifierInfo(ToV->getQualifierLoc());
      ToTentative->setAccess(ToV->getAccess());
      ToTentative->setLexicalDeclContext(ToV->getLexicalDeclContext());
      ToV->getLexicalDeclContext()->addDeclInternal(ToTentative);
      if (!ToV->isFileVarDecl() && ToV->isUsed())
        ToTentative->setIsUsed();
      TentativeDefinitions.push_back(ToTentative);
      return std::make_pair(FromV, ToTentative);
    }
    break;
  }
  return std::make_pair(FromV, ToV);
}

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
      Decl *ToD = nullptr;
      if (const auto *F = dyn_cast<FunctionDecl>(D)) {
        // It is not safe to import prototype. In this case parameters from
        // prototype will be imported but body will be imported from definition.
        // This leads to loss of information about parameters in the body.
        // Parameters in the definition and prototype does not linked together.
        F->hasBody(F);
        D = const_cast<FunctionDecl *>(F);
        ToD = Importer.Import(D);
      } else if (auto *V = dyn_cast<VarDecl>(D)) {
        std::tie(D, ToD) = ImportVarDecl(V, Importer, TentativeDefinitions);
      } else {
        ToD = Importer.Import(D);
      }
      if (ToD) {
        DeclGroupRef DGR(ToD);
        CI.getASTConsumer().HandleTopLevelDecl(DGR);
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
