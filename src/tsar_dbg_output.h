//===---- tsar_dbg_output.h - Output functions for debugging ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines a set of output functions for various bits of information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DBG_OUTPUT_H
#define TSAR_DBG_OUTPUT_H

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace llvm {
class raw_ostream;
class Function;
class Value;
class LoopInfo;
}

namespace tsar {
/// \brief Prints information available from a source code for the
/// specified memory location.
///
/// \pre At this moment location can be represented as a sequence of 'load' or
/// 'getelementptr' instructions ending 'alloca' instruction, 'alloca'
/// instructions or global variables.
/// A location must not be null.
/// \par Example
/// \code
///    ...
/// 1: int *p;
/// 2: *p = 5;
/// \endcode
/// \code
/// %p = alloca i32*, align 4
/// %0 = load i32*, i32** %p, align 4
/// \endcode
/// If debug information is available the result for
/// %0 will be *p otherwise it will be *(%p = alloca i32*, align 4).
void printLocationSource(llvm::raw_ostream &o, const llvm::Value *V);

/// \brief Print description of a type from a source code.
///
/// \param [in] DITy Meta information for a type.
void printDIType(llvm::raw_ostream &o, const llvm::DITypeRef &DITy);

/// \brief Print description of a variable from a source code.
///
/// \param [in] DIVar Meta information for a variable.
void printDIVariable(llvm::raw_ostream &o, llvm:: DIVariable *DIVar);

/// \brief Prints loop tree which is calculated by the LoopInfo pass.
///
/// \param [in] LI Information about natural loops identified
/// by the LoopInfor pass.
void printLoops(llvm::raw_ostream &o, const llvm::LoopInfo &LI);

namespace detail {
/// Applies a specified function object to each loop in a loop tree.
template<class Function>
void for_each(llvm::LoopInfo::reverse_iterator ReverseI,
              llvm::LoopInfo::reverse_iterator ReverseEI,
              Function F) {
  for (; ReverseI != ReverseEI; ++ReverseI) {
    F(*ReverseI);
    for_each((*ReverseI)->rbegin(), (*ReverseI)->rend(), F);
  }
}
}

/// Applies a specified function object to each loop in a loop tree.
template<class Function>
Function for_each(const llvm::LoopInfo &LI, Function F) {
  detail::for_each(LI.rbegin(), LI.rend(), F);
  return std::move(F);
}
}

#endif//TSAR_DBG_OUTPUT_H
