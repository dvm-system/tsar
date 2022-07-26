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
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <set>

namespace tsar {
class DIEstimateMemory;
class DIAliasNode;
class DIAliasTrait;

/// Look up for locations referenced and declared in the scope.
struct VariableCollector
    : public clang::RecursiveASTVisitor<VariableCollector> {
  using DIMemoryMatcher = llvm::ClangDIMemoryMatcherPass::DIMemoryMatcher;

  using VariableT = bcl::tagged_pair<bcl::tagged<clang::VarDecl *, AST>,
                                     bcl::tagged<WeakDIMemoryHandle, MD>>;

  struct VariableLess {
    bool operator()(const VariableT &LHS, const VariableT &RHS) const {
      return LHS.get<AST>()->getName() < RHS.get<AST>()->getName();
    }
  };

  /// Sorted list of variables (to print their in algoristic order).
  using SortedVarListT = std::set<VariableT, VariableLess>;

  /// Sorted list of variables (to print their in algoristic order).
  /// Variables with the same key may have different metadata-level locations
  /// attached to them.
  using SortedVarMultiListT = std::multiset<VariableT, VariableLess>;

  enum DerivedKind : uint8_t {
    // Set if a metadata-level memory location covers all memory which
    // is represented with a corresponding source-level location.
    // For example, <*A, ?> and int *A.
    DK_Strong,
    // Set if metadata-level memory location has a limited size. However,
    // the corresponding source-level location has unknown size.
    // For example, <*A. 100> and int *A. In some cases analyzer can determine
    // a number of only accessible bytes.
    DK_Bounded,
    // Set if metadata-level memory location overlaps with a source-level
    // location. For examle, <A[5], 1>, <A[6], 1> and int *A.
    DK_Partial,
    DK_Weak
  };

  struct DerivedMemory {
    llvm::TinyPtrVector<DIEstimateMemory *> Memory;
    DerivedKind Kind;

    DerivedMemory() = default;

    DerivedMemory(DIEstimateMemory *M, DerivedKind K) : Memory(M), Kind(K) {
      assert(M && "Memory location must not be null!");
    }

    operator bool() const { return Kind != DK_Weak && !Memory.empty(); }
  };

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

  /// Traverse internal parts of the loop, do not traverse an initialization
  /// part executed outside of the loop.
  bool TraverseLoopIteration(clang::ForStmt *For);

  /// Find declaration for a specified memory, remember memory if it safely
  /// represent a found variable or its part (update `CanonicalRefs` map).
  std::tuple<clang::VarDecl *, DIMemory *, DeclSearch>
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
  /// nullptr if the variable not found). In this case the third argument in
  /// tuple is false. Otherwise, it is true. The fourth argument is set to true
  /// if the variable is defined inside the loop head or the loop body.
  std::tuple<clang::VarDecl *, DIMemory *, bool, bool>
  localize(DIMemoryTrait &T, const DIAliasNode &DIN,
           const DIMemoryMatcher &ASTToClient,
           const ClonedDIMemoryMatcher &ClientToServer);

  /// Check whether it is possible to use high-level syntax to create copy of a
  /// specified memory `T` for each thread.
  ///
  /// On success to create a local copy of a memory source-level variable
  /// should be mentioned in a clauses like private or reduction.
  /// This variable will be stored in a list of variables `VarNames`.
  /// \post On failure if `Error` not nullptr add the variable which
  /// prevents localization inside the list.
  bool localize(DIMemoryTrait &T, const DIAliasNode &DIN,
                const DIMemoryMatcher &ASTToClient,
                const ClonedDIMemoryMatcher &ClientToServer,
                SortedVarListT &VarNames, SortedVarListT &LocalVarNames,
                SortedVarMultiListT *Error = nullptr);

  /// Check whether it is possible to use high-level syntax to create copy for
  /// all memory locations in `TS` for each thread.
  ///
  /// \post On failure if `Error` not nullptr add the variable which
  /// prevents localization inside the list.
  bool localize(DIAliasTrait &TS, const DIMemoryMatcher &ASTToClient,
                const ClonedDIMemoryMatcher &ClientToServer,
                SortedVarListT &VarNames, SortedVarListT &LocalVarNames,
                SortedVarMultiListT *Error = nullptr);

  /// Map of variable which is referenced in a scope. The value in the map is
  /// metadat-level memory locations which represent a derived memory from
  /// a specified variable (key in the map).
  ///
  /// For example, in case of `int **A`, `A` is a key in the map,
  /// and value contains `<A,8>` to represent A, <*A,?> to represent *A,
  /// and <*A[?],?> to represent **A. If there are no `nullptr` in a value
  /// then all memory derived from the key variable can be safely represented.
  llvm::DenseMap<clang::VarDecl *, llvm::SmallVector<DerivedMemory, 2>>
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
