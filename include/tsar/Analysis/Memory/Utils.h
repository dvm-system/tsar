//===--- Utils.h -------------- Memory Utils ---------------------*- C++ -*===//
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
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_UTILS_H
#define TSAR_MEMORY_UTILS_H

#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Optional.h>

namespace llvm {
class Instruction;
class BasicBlock;
class DataLayout;
class DIGlobalVariableExpression;
class DominatorTree;
class Function;
class GlobalObject;
class GlobalVariable;
class Loop;
class ScalarEvolution;
class SCEV;
class TargetLibraryInfo;
class DIExpression;
class DISubprogram;
class Value;
}

namespace tsar {
class AliasTree;
class DefUseSet;
template<class GraphType> class SpanningTreeRelation;

/// Finds the debug info intrinsics describing a value regardless whether it is
/// a constant or not.
void findAllDbgUsers(
    llvm::SmallVectorImpl<llvm::DbgVariableIntrinsic *> &DbgUsers,
    llvm::Value *V);

/// Finds the debug info intrinsics describing a constant.
void findConstDbgUsers(
    llvm::SmallVectorImpl<llvm::DbgVariableIntrinsic *> &DbgUsers,
    llvm::Constant *C);

/// This function strips off any GEP address adjustments and pointer casts from
/// the specified value while it is possible or until the object with attached
/// metadata describing a value will be found. The second returned value is
/// true if a returned object has attached metadata.
std::pair<llvm::Value *, bool> GetUnderlyingObjectWithMetadata(llvm::Value *V,
  const llvm::DataLayout &DL);

/// Returns a meta information for function or nullptr;
llvm::DISubprogram * findMetadata(const llvm::Function *F);

/// Find meta information available a global variable.
/// Returns 'true' if at least one valid metadata has been found.
bool findGlobalMetadata(const llvm::GlobalVariable *Var,
  llvm::SmallVectorImpl<DIMemoryLocation> &DILocs);

/// Find meta expressions attached to a specified global object.
///
/// Return `true` if at least one expression has been found.
bool findGlobalDIExpression(const llvm::GlobalObject *GO,
  llvm::SmallVectorImpl<llvm::DIGlobalVariableExpression *> &DIExprs);

/// \brief Checks that two fragments of a variable may overlap.
///
/// Two fragments of zero size may not overlap. Note that there is no reason
/// to invoke this functions for fragments of different variables. Complex
/// expressions which contains elements other then dwarf::DW_OP_LLVM_fragment
/// does not analyzed accurately. In this case overlapping is conservatively
/// assumed.
bool mayAliasFragments(
  const llvm::DIExpression &LHS, const llvm::DIExpression &RHS);

/// Return true if a specified expression contains dereference.
bool hasDeref(const llvm::DIExpression &Expr);

/// Specify kind of metadata which should be found.
enum class MDSearch { AddressOfVariable, ValueOfVariable, Any };

/// \brief Returns a meta information for a specified value or nullptr.
///
/// \param [in] V Analyzed value.
/// \param [in] DT If it is specified then llvm.dbg.value will be analyzed if
/// necessary. Otherwise llvm.dbg.declare, llvm.dbg.address and global
/// variables will be analyzed only.
/// \param [in] MDS Kind of metadata which should be found. If AddressOfVariable
/// is used then `V` is an address of a variable and dbg.declare, dbg.address
/// should be found only. If ValueOfVariable is used then `V` is a value of a
/// variable and dbg.value should be found only.
/// \param [out] DILocs This will contain all metadata-level locations which are
/// associated with a specified value. For this reason llvm.dbg. ...
/// intrinsics will be analyzed. Intrinsics which dominates all
/// uses of `V` will be only considered. The condition mentioned bellow is also
/// checked. Let us consider some llvm.dbg.value `I` which dominates all uses of
/// `V` and associates a metadata-level `DILoc` with `V`. Paths from `I` to each
/// use of V will be checked. There should be no other intrinsics which
/// associates `DILoc` with some other value on these paths.
/// \return A variable from `DILocs`, llvm.dbg.value for this
/// variable dominates llvm.dbg.value for other variables from `DILocs`. If
/// there is no such variable, None is returned.
/// `Status` will be set if it is not nullptr.
llvm::Optional<DIMemoryLocation> findMetadata(const llvm::Value * V,
  llvm::SmallVectorImpl<DIMemoryLocation> &DILocs,
  const llvm::DominatorTree *DT = nullptr, MDSearch MDS = MDSearch::Any,
  MDSearch *Status = nullptr);

/// Return meta information for a specified value or None.
///
/// This function is similar to previously defined function findMetadata(),
/// however it performs only MDSearch::ValueOfVariable search. The main
/// difference is it consider a specified users 'Users' of a specified value `V'.
/// A general findMetadata() function consider all users of `V` instead.
llvm::Optional<DIMemoryLocation> findMetadata(const llvm::Value *V,
  llvm::ArrayRef<llvm::Instruction *> Users, const llvm::DominatorTree &DT,
  llvm::SmallVectorImpl<DIMemoryLocation> &DILocs);

/// Return 'true' if values in memory accessed in a specified instruction 'I'
/// is region invariant.
bool accessInvariantMemory(llvm::Instruction &I, llvm::TargetLibraryInfo &TLI,
  const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);

/// Return 'true' if result of a specified instruction is loop invariant.
bool isLoopInvariant(llvm::Instruction &I, const llvm::Loop &L,
  llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);

/// Return 'true' if result of a specified instruction is function invariant.
bool isFunctionInvariant(llvm::Instruction &I,
  llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);

/// Return 'true' if result of a specified instruction is block invariant.
bool isBlockInvariant(llvm::Instruction &I, const llvm::BasicBlock &BB,
  llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);

/// Return 'true' if a specified expression 'Expr' is loop invariant. If loop is
/// null then the whole function will be evaluated.
bool isLoopInvariant(const llvm::SCEV *Expr, const llvm::Loop *L,
  llvm::TargetLibraryInfo &TLI, llvm::ScalarEvolution &SE,
  const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);

/// The first returned value is 'true' if calls of a specified function has no
/// side effects and do not access global memory. Otherwise, if it accesses
/// global variables in a simple way only (without dereferences) the second
/// value will be 'true'.
std::pair<bool, bool> isPure(const llvm::Function &F, const DefUseSet &DUS);
}
#endif//TSAR_MEMORY_UTILS_H
