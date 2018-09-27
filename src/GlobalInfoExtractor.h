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

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>

namespace clang {
class SourceManager;
class NamedDecl;
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

  /// Set of files.
  using FileSet = llvm::DenseSet<const clang::FileEntry *>;

  /// Represents outermost parent (Root) of a global declaration (Descendant).
  ///
  /// For example, in case of `struct A { struct B { ... }; };` A and B will
  /// be global (for C). `Descendant` will be B and `Root` will be A if A is
  /// an outermost declaration (its parent is a translation unit). Note, that if
  /// A is available at a location than B will be also available at this
  /// location.
  struct OutermostDecl {
    OutermostDecl(clang::NamedDecl *Descendant, clang::Decl *Root) :
        mDescendant(Descendant), mRoot(Root) {
      assert(!Root || Descendant &&
        "If root specified descendant must not be null!");
    }
    bool isValid() const noexcept { return mRoot; }
    bool isInvalid() const noexcept { return !isValid(); }
    operator bool() const noexcept { return isValid(); }
    clang::NamedDecl * getDescendant() noexcept { return mDescendant; }
    clang::NamedDecl * getDescendant() const noexcept { return mDescendant; }
    clang::Decl * getRoot() noexcept { return mRoot; }
    clang::Decl * getRoot() const noexcept { return mRoot; }
  private:
    clang::NamedDecl *mDescendant = nullptr;
    clang::Decl *mRoot = nullptr;
  };

  struct OutermostDeclMapInfo {
    static inline OutermostDecl * getEmptyKey() {
      return llvm::DenseMapInfo<OutermostDecl *>::getEmptyKey();
    }
    static inline OutermostDecl * getTombstoneKey() {
      return llvm::DenseMapInfo<OutermostDecl *>::getTombstoneKey();
    }
    static inline unsigned getHashValue(const OutermostDecl *D) {
      return llvm::DenseMapInfo<clang::NamedDecl *>::
        getHashValue(D->getDescendant());
    }
    static inline unsigned getHashValue(const clang::NamedDecl *D) {
      return llvm::DenseMapInfo<clang::NamedDecl *>::getHashValue(D);
    }
    static inline bool isEqual(const OutermostDecl *LHS,
        const OutermostDecl *RHS) noexcept {
      return LHS == RHS;
    }
    static inline bool isEqual(const clang::NamedDecl *LHS,
        const OutermostDecl *RHS) noexcept {
      return RHS != getEmptyKey() && RHS != getTombstoneKey() &&
        LHS == RHS->getDescendant();
    }
  };

  struct OutermostDeclNameMapInfo {
    static inline OutermostDecl * getEmptyKey() {
      return llvm::DenseMapInfo<OutermostDecl *>::getEmptyKey();
    }
    static inline OutermostDecl * getTombstoneKey() {
      return llvm::DenseMapInfo<OutermostDecl *>::getTombstoneKey();
    }
    static inline unsigned getHashValue(const OutermostDecl *D) {
      return llvm::DenseMapInfo<llvm::StringRef>::
        getHashValue(D->getDescendant()->getName());
    }
    static inline unsigned getHashValue(const clang::NamedDecl *D) {
      return llvm::DenseMapInfo<llvm::StringRef>::getHashValue(D->getName());
    }
    static inline unsigned getHashValue(llvm::StringRef Name) {
      return llvm::DenseMapInfo<llvm::StringRef>::getHashValue(Name);
    }
    static inline bool isEqual(const OutermostDecl *LHS,
        const OutermostDecl *RHS) noexcept {
      return LHS == RHS;
    }
    static inline bool isEqual(const clang::NamedDecl *LHS,
        const OutermostDecl *RHS) noexcept {
      return RHS != getEmptyKey() && RHS != getTombstoneKey() &&
        LHS == RHS->getDescendant();
    }
    static inline bool isEqual(llvm::StringRef LHS,
        const OutermostDecl *RHS) noexcept {
      return RHS != getEmptyKey() && RHS != getTombstoneKey() &&
        LHS == RHS->getDescendant()->getName();
    }
  };

  /// \brief Map from name of Descendant declaration to list of corresponding
  /// declarations.
  ///
  /// List contains multiple declarations in case of static declarations from
  /// different files with the same name. It also contains multiple declarations
  /// in case of declarations with empty name.
  using OutermostDeclMap = llvm::StringMap<llvm::SmallVector<OutermostDecl, 1>>;

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
  const FileSet & getFiles() const noexcept { return mFiles; }

  /// Returns a map from a name of global declaration to the list
  /// of global declarations with this name.
  const OutermostDeclMap & getOutermostDecls() const noexcept {
    return mOutermostDecls;
  }

  /// Returns outermost parent for the specified global declaration. If a
  /// specified declaration is not global return `nullptr`.
  OutermostDecl * findOutermostDecl(const clang::NamedDecl *ND) {
    return const_cast<OutermostDecl *>(
      static_cast<const GlobalInfoExtractor *>(this)->findOutermostDecl(ND));
  }

  /// Returns outermost parent for the specified global declaration. If a
  /// specified declaration is not global return `nullptr`.
  const OutermostDecl * findOutermostDecl(const clang::NamedDecl *ND) const {
    auto I = mOutermostDecls.find(ND->getName());
    if (I != mOutermostDecls.end()) {
      for (auto &D : I->second) {
        if (D.getDescendant() == ND)
          return &D;
      }
    }
    return nullptr;
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

  FileSet mFiles;
  RawLocationSet mVisitedExpLocs;
  RawLocationSet mVisitedIncludeLocs;
  OutermostDeclMap mOutermostDecls;

  /// Outermost declaration (its parent in AST is a translation unit) which
  /// is traversed at this moment.
  clang::Decl *mOutermostDecl = nullptr;
};
}
#endif//TSAR_GLOBAL_INFO_EXTRACTOR_H
