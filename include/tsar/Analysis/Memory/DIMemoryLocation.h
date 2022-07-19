//===- DIMemoryLocation.h - Debug Level Memory Location ---------*- C++ -*-===//
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
// This file provides utility analysis objects describing memory locations.
// Unlike llvm::MemoryLocation a memory is depicted here at a debug level.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_MEMORY_LOCATION_H
#define TSAR_DI_MEMORY_LOCATION_H

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace llvm {
class DIVariable;
class DIExpression;
class SmallBitVector;
}

namespace tsar {
/// \brief This represents memory location using metadata information.
///
/// DWARF expressions is used to calculate address of location starting point.
/// Address of variable is used as a basis for such calculation. If variable is
/// a pointer than DW_OP_deref expression can be used.
/// DW_OP_plus, DW_OP_minus, DW_OP_plus_uconst, DW_OP_constu
/// are also supported to move address of the beginning. Memory location may be
/// also described as a fragment of some variable (DW_OP_LLVM_fragment). Each
/// fragment has an offset from the variable beginning and size.
///
/// Size of location is unknown if DW_OP_deref is used without a fragment
/// specification. If DW_OP_deref is not used and fragment is not specified than
/// size of location depends on a variable size and address of the location
/// starting point.
///
/// Location may be marked as a template. It means that all zero offsets
/// (including implicit) in pointer and array accesses  should be treated
/// as unknown offsets. Consider an example. Element of an array A
/// (of integer values) is specified as {DW_OP_LLVM_fragment, 0, 4}. In case of
/// template location it means any element A[?] instead of A[0] only.
///
/// Note, that if size of DW_OP_LLVM_fragment is 0 then it means that size of
/// the location is unknown.
///
/// \attention Type casts can not be safely represented as template locations.
/// For example, (char *)P + ? where P has type 'int' will be unparsed as
/// P[?] and (char *)P + 1 + ? will be unparsed as (char *)P + 1. Existing of
/// casts determines if offset and type size is inconsistent. This implies
/// mentioned shortcomings in case of template offsets which are represented as
/// zero.
struct DIMemoryLocation {
  llvm::DIVariable *Var = nullptr;
  llvm::DIExpression *Expr = nullptr;
  llvm::DILocation *Loc = nullptr;
  bool Template = false;
  bool AfterPointer = false;

  /// Determines which memory location is exhibits by a specified instruction.
  static DIMemoryLocation get(llvm::DbgVariableIntrinsic *Inst);

  /// Determines which memory location is exhibits by a specified instruction.
  static inline DIMemoryLocation get(llvm::Instruction *Inst) {
    assert(Inst && "Instruction must not be null!");
    if (auto I = llvm::dyn_cast<llvm::DbgValueInst>(Inst))
      return get(I);
    llvm_unreachable("Unsupported memory instruction!");
    return DIMemoryLocation{nullptr, nullptr};
  }

  /// Constructs a new memory location. Note, that variable and expression
  /// must not be null).
  static inline DIMemoryLocation get(llvm::DIVariable *Var,
                                     llvm::DIExpression *Expr,
                                     llvm::DILocation *Loc = nullptr,
                                     bool Template = false,
                                     bool AfterPointer = false) {
    DIMemoryLocation DILoc{Var, Expr, Loc, Template, AfterPointer};
    if (!DILoc.getSize().mayBeBeforePointer())
      DILoc.AfterPointer = true;
    return DILoc;
  }

  /// If DW_OP_deref exists it returns true.
  bool hasDeref() const;

  /// If the first operation in expression is DW_OP_deref it returns true.
  bool startsWithDeref() const;

  /// Return true if size is known.
  ///
  /// \attention
  /// - It does not check whether memory out of range memory access
  /// occurs. In this case `isSized()` returns `true` but `getSize()` returns
  /// unknown size.
  /// - If size of DW_OP_LLVM_fragment is 0 the size is unknown and this method
  /// returns false.
  bool isSized() const;

  /// Return size of location, in address units if it is known.
  ///
  /// If out of range memory access occurs UnknownSize will be also returned.
  /// If size of DW_OP_LLVM_fragment is 0 then UnknownSize will be returned.
  llvm::LocationSize getSize() const;

  /// \brief Returns list of offsets of the location starting point from its
  /// basis, in address units.
  ///
  /// Presence of dereference operations produce multiple offsets (a separate
  /// value for each operation).
  void getOffsets(llvm::SmallVectorImpl<uint64_t> &Offsets,
      llvm::SmallBitVector &SignMask) const;

  /// Checks that representation of memory location is valid (the focus is on
  /// the expression.
  bool isValid() const;

private:
  friend struct llvm::DenseMapInfo<DIMemoryLocation>;

  /// Constructs a new memory location. Note, that variable and expression
  /// must not be null).
  DIMemoryLocation(llvm::DIVariable *Var, llvm::DIExpression *Expr,
                   llvm::DILocation *Loc = nullptr, bool Template = false,
                   bool AfterPointer = false)
      : Var(Var), Expr(Expr), Loc(Loc), Template(Template),
        AfterPointer(AfterPointer) {
    // Do not check here that location isValid() because this leads to crash
    // of construction of empty key in specialization of llvm::DenseMapInfo.
    assert(Var && "Variable must not be null!");
    assert(Expr && "Expression must not be null!");
  }
};

inline bool operator==(DIMemoryLocation LHS, DIMemoryLocation RHS) noexcept {
  return LHS.Var == RHS.Var && LHS.Expr == RHS.Expr;
}

inline bool operator!=(DIMemoryLocation LHS, DIMemoryLocation RHS) noexcept {
  return LHS.Var != RHS.Var || LHS.Var != RHS.Var;
}
}

namespace llvm {
template<> struct DenseMapInfo<tsar::DIMemoryLocation> {
  using PairInfo = DenseMapInfo<
    std::pair<DIVariable *, DIExpression *>>;
  static inline tsar::DIMemoryLocation getEmptyKey() {
    auto Pair = PairInfo::getEmptyKey();
    return { Pair.first, Pair.second };
  }
  static inline tsar::DIMemoryLocation getTombstoneKey() {
    auto Pair = PairInfo::getTombstoneKey();
    return { Pair.first, Pair.second };
  }
  static inline unsigned getHashValue(const tsar::DIMemoryLocation &Loc) {
    return PairInfo::getHashValue(std::make_pair(Loc.Var, Loc.Expr));
  }
  static inline bool isEqual(
      const tsar::DIMemoryLocation &LHS, const tsar::DIMemoryLocation &RHS) {
    return LHS.Var == RHS.Var &&
      LHS.Expr == RHS.Expr &&
      LHS.Template == RHS.Template &&
      LHS.AfterPointer == RHS.AfterPointer;
  }
};
}
#endif//TSAR_DI_MEMORY_LOCATION_H
