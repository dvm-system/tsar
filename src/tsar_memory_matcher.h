//=== tsar_memory_matcher.h - High and Low Level Memory Matcher -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Classes and functions from this file match variables in a source high-level
// code and appropriate allocas or globals in low-level LLVM IR. This file
// implements pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_MATCHER_H
#define TSAR_MEMORY_MATCHER_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>
#include <set>
#include <utility.h>
#include "tsar_bimap.h"
#include "tsar_pass.h"
#include "tsar_utility.h"

namespace clang {
class FuncDecl;
class VarDecl;
}

namespace llvm {
class Value;

class MemoryMatcherPass :
  public ModulePass, private bcl::Uncopyable {
public:
  typedef tsar::Bimap<
    bcl::tagged<clang::VarDecl*, tsar::AST>,
    bcl::tagged<llvm::Value *, tsar::IR>> MemoryMatcher;

  typedef std::set<clang::VarDecl *> MemoryASTSet;

  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  MemoryMatcherPass() : ModulePass(ID) {
    initializeMemoryMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  /// Matches different memory locations.
  bool runOnModule(llvm::Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Returns memory matcher for the last analyzed module.
  const MemoryMatcher & getMatcher() const noexcept { return mMatcher; }

  /// Returns unmatched memory in AST.
  const MemoryASTSet & getUnmatchedAST() const noexcept {
    return mUnmatchedAST;
  }

  /// Releases allocated memory.
  void releaseMemory() override {
    mMatcher.clear();
    mUnmatchedAST.clear();
  }

private:
  MemoryMatcher mMatcher;
  MemoryASTSet mUnmatchedAST;
};
}
#endif//TSAR_MEMORY_MATCHER_H
