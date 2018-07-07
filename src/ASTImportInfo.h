//===--- ASTImportInfo.h - AST Import Process Information--------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines storage to access import process information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_AST_IMPORT_INFO_H
#define TSAR_AST_IMPORT_INFO_H

#include "tsar_pass.h"
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>
#include <vector>

namespace clang {
class ASTImporter;
}

namespace tsar {
/// Extended information about the import process.
struct ASTImportInfo {
  /// \brief This collection contains synonyms for a location.
  ///
  /// List of synonyms also contains location which is a key.
  ///
  /// Importer merges imported external declarations to the existing one. So,
  /// the information of locations of an original declaration may be lost.
  /// For example, Import(FileID of From) != FileID of To. In this case it is
  /// not possible to find include which makes the From location visible at
  /// some point (such information is necessary for example in ClangInliner).
  using RedeclLocList =
    llvm::DenseMap<unsigned, std::vector<clang::SourceLocation>>;
  RedeclLocList RedeclLocs;

  /// True if import has been performed.
  bool WasImport = false;
};
}

namespace llvm {
/// Gives access to the import process information.
class ImmutableASTImportInfoPass :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;
  ImmutableASTImportInfoPass(const tsar::ASTImportInfo *Info = nullptr) :
    ImmutablePass(ID), mImportInfo(Info) {
    assert(Info && "Import information must not be null!");
    initializeImmutableASTImportInfoPassPass(*PassRegistry::getPassRegistry());
  }
  const tsar::ASTImportInfo & getImportInfo() const noexcept {
    return *mImportInfo;
  }
private:
  const tsar::ASTImportInfo *mImportInfo;
};
}
#endif//TSAR_AST_IMPORT_INFO_H
