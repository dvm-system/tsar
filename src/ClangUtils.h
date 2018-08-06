//===--- ClangUtils.h --- Utilities To Examine Clang AST  -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities to examine Clang AST.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_UTILS_H
#define TSAR_CLANG_UTILS_H

namespace llvm {
template <typename PtrType> class SmallPtrSetImpl;
}

namespace clang {
class CFG;
class CFGBlock;
}

namespace tsar {
/// Finds unreachable basic blocks for a specified CFG.
void unreachableBlocks(clang::CFG &Cfg,
  llvm::SmallPtrSetImpl<clang::CFGBlock *> &Blocks);
}
#endif//TSAR_CLANG_UTILS_H
