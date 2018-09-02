//===- GlobalInfoExtractor.h - AST Based Global Information -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file provides functionality to collect global information about the
// whole translation unit. It collects all mentioned files, include locations,
// and global declarations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_GLOBAL_INFO_EXTRACTOR_H
#define TSAR_GLOBAL_INFO_EXTRACTOR_H

#include "NamedDeclMapInfo.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

namespace clang {
class SourceManager;
}

namespace tsar {
/// \brief This recursive visitor collects global information about
/// a translation unit.
///
/// Usage: GIE.TraverseDecl(TUD), where
/// - GIE is an object of GlobalInfoExtractor,
/// - TUD is an object of TranslationUnitDecl.
class GlobalInfoExtractor :
  public clang::RecursiveASTVisitor<GlobalInfoExtractor> {
public:
  /// Set of raw encodings for clang::SourceLocation.
  using RawLocationSet = llvm::DenseSet<unsigned>;

  /// Map from file content to one of file IDs.
  using FileMap = llvm::DenseMap<const llvm::MemoryBuffer *, clang::FileID>;

  using OutermostDeclMap =
    llvm::DenseMap<clang::NamedDecl *, clang::Decl *, NamedDeclMapInfo>;

  /// Creates extractor.
  explicit GlobalInfoExtractor(const clang::SourceManager &SM,
    const clang::LangOptions &LangOpts) : mSM(SM), mLangOpts(LangOpts) {}

  /// Returns source manager.
  const clang::SourceManager & getSourceManager() const noexcept { return mSM; }

  /// Returns set of visited expansion locations.
  const RawLocationSet & getExpansionLocs() const noexcept {
    return mVisitedExpLocs;
  }

  /// Returns set of visited locations of file names in #include directive.
  const RawLocationSet & getIncludeLocs() const noexcept {
    return mVisitedIncludeLocs;
  }

  /// \brief Returns a list of source files associated with visited locations.
  ///
  /// Note, that some source files will be omitted if there are no
  /// appropriate locations in AST or if some locations have not been visited.
  /// For example, a file will be omitted if it contains only macros
  /// which are never used.
  const FileMap & getFiles() const noexcept { return mFiles; }

  /// \brief Returns a map from a global named declaration to the outermost
  /// global declaration which contains this named declaration.
  ///
  /// For example, in case of `struct A { struct B { ... }; };` A and B will
  /// be global (for C). Key in this map will be B and value will be A if A is
  /// an outermost declaration (its parent is a translation unit). Note, that if
  /// A is available at a location than B will be also available at this
  /// location.
  const OutermostDeclMap  & getOutermostDecls() const noexcept {
    return mOutermostDecls;
  }

  bool VisitStmt(clang::Stmt *S);
  bool VisitTypeLoc(clang::TypeLoc TL);
  bool TraverseDecl(clang::Decl *D);

private:
  /// Collect chain of #include files.
  void collectIncludes(clang::FileID FID);

  /// Checks that a specified location contains result of macro expansion
  /// and collects all files referenced by this location (including #include
  /// files).
  void visitLoc(clang::SourceLocation Loc);

  const clang::SourceManager &mSM;
  const clang::LangOptions &mLangOpts;

  FileMap mFiles;
  RawLocationSet mVisitedExpLocs;
  RawLocationSet mVisitedIncludeLocs;
  OutermostDeclMap mOutermostDecls;

  /// Outermost declaration (its parent in AST is a translation unit) which
  /// is traversed at this moment.
  clang::Decl *mOutermostDecl = nullptr;
};
}
#endif//TSAR_GLOBAL_INFO_EXTRACTOR_H
