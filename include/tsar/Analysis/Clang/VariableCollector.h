//===-- VariableCollector.h - Variable Collector (Clang) ---------*- C++ -*===//
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
// This file implements a visitor to collect variables referenced in a scope and
// to check whether metadata-level memory locations are safely represent these
// variables.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_VARIABLE_COLLECTOR_H
#define TSAR_CLANG_VARIABLE_COLLECTOR_H

#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

namespace tsar {
class DIEstimateMemory;
class DIAliasNode;
class DIAliasTrait;

/// Look up for locations referenced and declared in the scope.
struct VariableCollector
    : public clang::RecursiveASTVisitor<VariableCollector> {
  using DIMemoryMatcher = llvm::ClangDIMemoryMatcherPass::DIMemoryMatcher;

  // Sorted list of variables (to print their in algoristic order).
  using SortedVarListT = std::set<std::string, std::less<std::string>>;

  enum DeclSearch : uint8_t {
    /// Set if memory safely represent a local variable.
    CoincideLocal,
    /// Set if memory safely represent a global variable.
    CoincideGlobal,
    /// Set if memory does not represent the whole variable, however
    /// a corresponding variable can be used to describe a memory location.
    Derived,
    /// Set if corresponding variable does not exist.
    Invalid,
    /// Set if a found declaration is not explicitly mentioned in a loop body.
    /// For example, global variables may be used in called functions instead
    /// of a loop body.
    Implicit,
    /// Set if memory does not represent a variable. For example, it may
    /// represent a memory used in a function call or result of a function.
    Useless,
    /// Set if it is not known whether corresponding variable must exist or not.
    /// For example, a memory may represent some internal object which is not
    /// referenced in the original source code.
    Unknown,
  };

  /// Remember all referenced canonical declarations and compute number of
  /// estimate memory locations which should be built for this variable.
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE);

  /// Remember all canonical declarations declared inside the loop.
  bool VisitDeclStmt(clang::DeclStmt *DS);

  /// Find declaration for a specified memory, remember memory if it safely
  /// represent a found variable or its part (update `CanonicalRefs` map).
  std::pair<clang::VarDecl *, DeclSearch>
  findDecl(const DIMemory &DIM, const DIMemoryMatcher &ASTToClient,
           const ClonedDIMemoryMatcher &ClientToServer);

  /// Check whether it is possible to use high-level syntax to create copy of a
  /// specified memory `T` for each thread.
  ///
  /// On success to create a local copy of a memory source-level variable
  /// should be mentioned in a clauses like private or reduction.
  /// \attention  This method does not check whether it is valid to create a
  /// such copy, for example global variables must be checked later.
  /// Localized global variables breaks relation with original global variables.
  /// And program may become invalid if such variables are used in calls inside
  /// the loop body.
  /// \return On failure it returns the variable which prevents localization (or
  /// to nullptr if variable not found). In this case the second argument in
  /// tuple is false. Otherwise, it is true. The third argument is set to true
  /// if the variable is defined inside the loop head or the loop body.
  std::tuple<clang::VarDecl *, bool, bool>
  localize(DIMemoryTrait &T, const DIAliasNode &DIN,
           const DIMemoryMatcher &ASTToClient,
           const ClonedDIMemoryMatcher &ClientToServer);

  /// Check whether it is possible to use high-level syntax to create copy of a
  /// specified memory `T` for each thread.
  ///
  /// On success to create a local copy of a memory source-level variable
  /// should be mentioned in a clauses like private or reduction.
  /// This variable will be stored in a list of variables `VarNames`.
  /// \post On failure if `Error` not nullptr set it to the variable which
  /// prevents localization (or to nullptr if variable not found).
  bool localize(DIMemoryTrait &T, const DIAliasNode &DIN,
                const DIMemoryMatcher &ASTToClient,
                const ClonedDIMemoryMatcher &ClientToServer,
                SortedVarListT &VarNames, clang::VarDecl **Error = nullptr);

  /// Check whether it is possible to use high-level syntax to create copy for
  /// all memory locations in `TS` for each thread.
  ///
  /// On failure if `Error` not nullptr set it to the first variable which
  /// prevents localization (or to nullptr if variable not found).
  bool localize(DIAliasTrait &TS, const DIMemoryMatcher &ASTToClient,
                const ClonedDIMemoryMatcher &ClientToServer,
                SortedVarListT &VarNames, clang::VarDecl **Error = nullptr);

  /// Induction variable mentioned in a head of a canonical loop.
  ///
  /// TODO (kaniandr@gmail.com): set it to nullptr if analyzed scope is not a
  /// canonical loop.
  clang::VarDecl * Induction = nullptr;

  /// Map of variable which is referenced in a scope. The value in the map is
  /// metadat-level memory locations which represent a derived memory from
  /// a specified variable (key in the map).
  ///
  /// For example, in case of `int **A`, `A` is a key in the map,
  /// and value contains `<A,8>` to represent A, <*A,?> to represent *A,
  /// and <*A[?],?> to represent **A. If there are no `nullptr` in a value
  /// then all memory derived from the key variable can be safely represented.
  llvm::DenseMap<clang::VarDecl *, llvm::SmallVector<DIEstimateMemory *, 2>>
      CanonicalRefs;

  /// Set of local variables declared in an analyzed scope (canonical
  /// declarations are stored.
  llvm::DenseSet<clang::VarDecl *> CanonicalLocals;

  /// Map from alias node which contains global memory to one of global
  /// variables which represents this memory.
  llvm::DenseMap<DIAliasNode *, clang::VarDecl *> GlobalRefs;
};
}
#endif//TSAR_CLANG_VARIABLE_COLLECTOR
