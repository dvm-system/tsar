//===--- ClangUtils.cpp - Utilities To Examine Clang AST  -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities to examine Clang AST.
//
//===----------------------------------------------------------------------===//

#include "ClangUtils.h"
#include <clang/Analysis/CFG.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <vector>

using namespace clang;
using namespace llvm;
using namespace tsar;

void tsar::unreachableBlocks(clang::CFG &Cfg,
    llvm::SmallPtrSetImpl<clang::CFGBlock *> &Blocks) {
  DenseSet<clang::CFGBlock *> ReachableBlocks;
  std::vector<clang::CFGBlock *> Worklist;
  Worklist.push_back(&Cfg.getEntry());
  ReachableBlocks.insert(&Cfg.getEntry());
  while (!Worklist.empty()) {
    auto Curr = Worklist.back();
    Worklist.pop_back();
    for (auto &Succ : Curr->succs()) {
      if (Succ.isReachable() &&
          ReachableBlocks.insert(Succ.getReachableBlock()).second)
        Worklist.push_back(Succ.getReachableBlock());
    }
  }
  for (auto *BB : Cfg)
    if (!ReachableBlocks.count(BB))
      Blocks.insert(BB);
}
