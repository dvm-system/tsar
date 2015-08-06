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

namespace llvm {
  class AllocaInst;
  class raw_ostream;
  class Function;
  class LoopInfo;
}

namespace tsar {
/// \brief Prints description of a variable from a source code
/// for specified alloca.
///
/// The alloca must not be null.
void printAllocaSource(llvm::raw_ostream &o, llvm::AllocaInst *AI);

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
  for (; ReverseI != ReverseEI; --ReverseEI) {
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
