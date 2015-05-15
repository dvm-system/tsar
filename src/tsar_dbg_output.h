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
}

#endif//TSAR_DBG_OUTPUT_H
