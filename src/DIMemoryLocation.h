//===- DIMemoryLocation.h - Debug Level Memory Location ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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
#include <llvm/IR/IntrinsicInst.h>

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
/// a pointer than DW_OP_deref expression can be used. DW_OP_plus, DW_OP_minus,
/// are also supported to move address of the beginning. Memory location may be
/// also described as a fragment of some variable (DW_OP_LLVM_fragment). Each
/// fragment has an offset from the variable beginning and size.
///
/// Size of location is unknown if DW_OP_deref is used without a fragment
/// specification. If DW_OP_deref is not used and fragment is not specified than
/// size of location depends on a variable size and address of the location
/// starting point.
struct DIMemoryLocation {
  llvm::DIVariable *Var;
  llvm::DIExpression *Expr;

  /// Determines which memory location is exhibits by a specified instruction.
  static inline DIMemoryLocation get(llvm::DbgValueInst *Inst) {
    assert(Inst && "Instruction must not be null!");
    return {
      llvm::cast<llvm::DIVariable>(Inst->getVariable()),
      Inst->getExpression()
    };
  }

  /// Determines which memory location is exhibits by a specified instruction.
  static inline DIMemoryLocation get(llvm::Instruction *Inst) {
    assert(Inst && "Instruction must not be null!");
    if (auto I = llvm::dyn_cast<llvm::DbgValueInst>(Inst))
      return get(I);
    llvm_unreachable("Unsupported memory instruction!");
  }

  /// Constructs a new memory location. Note, that variable and expression
  /// must not be null).
  DIMemoryLocation(llvm::DIVariable *Var, llvm::DIExpression *Expr) :
      Var(Var), Expr(Expr) {
    // Do not check here that location isValid() because this leads to crash
    // of construction of empty key for in specialization of llvm::DenseMapInfo.
    assert(Var && "Variable must not be null!");
    assert(Expr && "Expression must not be null!");
  }

  /// If DW_OP_deref exists it returns true.
  bool hasDeref() const;

  /// \brief Returns true if size is known.
  ///
  /// \attention It does not check whether memory out of range memory access
  /// occurs. In this case `isSized()` returns `true` but `getSize()` returns
  /// llvm::MemoryLocation::UnknownSize.
  bool isSized() const;

  /// \brief Returns size of location, in address units, or
  /// llvm::MemoryLocation::UnknownSize if the size is not known.
  ///
  /// If out of range memory access occurs UnknownSize will be also returned.
  uint64_t getSize() const;

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
};
}
#endif//TSAR_DI_MEMORY_LOCATION_H
