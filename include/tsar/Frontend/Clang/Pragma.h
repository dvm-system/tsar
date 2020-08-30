//===--- Pragma.h ------------  Pragma Parser -------------------*- C++ -*-===//
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
// This files defines helpful method to find simplify search for pragmas in AST.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRAGMA_H
#define TSAR_PRAGMA_H

#include "tsar/Support/Directives.h"
#include <clang/AST/Stmt.h>
#include <clang/Lex/Pragma.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/SmallVector.h>

namespace clang {
class Rewriter;
}

namespace llvm {
template<class T> class SmallVectorImpl;
}

namespace tsar {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

struct ASTImportInfo;

/// Description of a directive.
class Pragma {
public:
  using clause_iterator = clang::CompoundStmt::body_iterator;
  using clause_range = clang::CompoundStmt::body_range;

  class Clause : public clang::CompoundStmt::body_range {
  public:
    using body_iterator = clang::CompoundStmt::body_iterator;
    Clause(llvm::StringRef Name, body_iterator Begin, body_iterator End) :
      mName(Name), clang::CompoundStmt::body_range(Begin, End) {}
    operator bool() const { return !mName.empty(); }
    llvm::StringRef getName() const noexcept { return mName; }
  private:
    llvm::StringRef mName;
  };

  /// Returns description of a specified clause.
  static Clause clause(clause_iterator I);

  /// Creates description of a prgama if `S` represents known directive.
  explicit Pragma(clang::Stmt &S);

  /// Returns true if a specified statement (in constructor) represent a
  /// known directive.
  operator bool() const noexcept {
    return mDirectiveId != DirectiveId::NotDirective;
  }

  clang::Stmt * getNamespace() const noexcept { return mStmt; }
  clang::Stmt * getDirective() const noexcept { return mDirective; }

  DirectiveNamespaceId getNamespaceId() const noexcept { return mNamespaceId; }
  DirectiveId getDirectiveId() const noexcept { return mDirectiveId; }

  /// Returns true if there is no clauses in a pragma.
  bool clause_empty() const noexcept { return clause_size() == 0; }

  /// Returns number of clauses.
  unsigned clause_size() const {
    assert(*this && "This must be a pragma");
    return mDirective->size() - 1;
  }

  /// Returns clauses.
  clause_range clause() {
    assert(*this && "This must be a pragma");
    return clause_range(mClauseBegin, mDirective->body_end());
  }
  clause_iterator clause_begin() { return mClauseBegin; }
  clause_iterator clause_end() { return mDirective->body_end(); }

private:
  DirectiveNamespaceId mNamespaceId = DirectiveNamespaceId::NotNamespace;
  DirectiveId mDirectiveId = DirectiveId::NotDirective;
  clang::Stmt *mStmt = nullptr;
  clang::CompoundStmt *mDirective = nullptr;
  clause_iterator mClauseBegin;
};

/// \brief Find all occurrences of a specified clause in a specified pragma.
///
/// \return `false` if a specified statement is not a pragma or clause is not
/// found.
bool findClause(Pragma &P, ClauseId Id,
  llvm::SmallVectorImpl<clang::Stmt *> &Clauses);

/// Represents some pragma properties.
struct PragmaFlags {
  enum Flags : uint8_t {
    DefaultFlags = 0,
    IsInMacro = 1u << 0,
    IsInHeader = 1u << 1,
    LLVM_MARK_AS_BITMASK_ENUM(IsInHeader)
  };
};

/// \brief Collects source ranges to removes all clauses from a specified list.
///
/// \pre A specified pragma `P` contains all clauses from a specified list
/// `Clauses`.
/// \return `false` if clauses can not be removed.
/// \post If pragma is in macro it can not be removed. If there are no other
/// clauses in the pragma `P` except clauses from the list `Clauses` the whole
/// pragma can be removed. So, the range of this pragma will be stored.
/// To disable some checks use `Ignore` parameter. Currently macros cannot be
/// processed even in the presence of this parameter.
///
/// TODO (kaniandr@gmail.com): check this for Microsoft-like pragmas.
std::pair<bool, PragmaFlags::Flags> pragmaRangeToRemove(const Pragma &P,
  const llvm::SmallVectorImpl<clang::Stmt *> &Clauses,
  const clang::SourceManager &SM, const clang::LangOptions &LangOpts,
  const ASTImportInfo &ImportInfo,
  llvm::SmallVectorImpl<clang::CharSourceRange> &ToRemove,
  PragmaFlags::Flags Ignore = PragmaFlags::DefaultFlags);

/// Constructs a string representation of a pragma according to introducer kind.
llvm::StringRef getPragmaText(DirectiveId Id, llvm::SmallVectorImpl<char> &Out,
  clang::PragmaIntroducerKind PIK = clang::PIK_HashPragma);

/// Constructs a string representation of a pragma according to introducer kind.
llvm::StringRef getPragmaText(ClauseId Id, llvm::SmallVectorImpl<char> &Out,
  clang::PragmaIntroducerKind PIK = clang::PIK_HashPragma);
}
#endif//TSAR_PRAGMA_H
