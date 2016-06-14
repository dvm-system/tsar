//=== tsar_loop_matcher.h - High and Low Level Loop Matcher -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Classes and functions from this file match loops in a source high-level
// code and appropriate loops in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LOOP_MATCHER_H
#define TSAR_LOOP_MATCHER_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_bimap.h"
#include "tsar_pass.h"

namespace tsar {
/// This tag provides access to low-level representation of matched entities.
struct IR {};

/// This tag provides access to source-level representation of matched entities.
struct AST {};
}

namespace clang {
class ForStmt;
}

namespace llvm {
class Function;
class Loop;

/// This per-function pass maths different loops in a source high level code
/// and appropriated loops in low-level LLVM IR.
class LoopMatcherPass :
  public FunctionPass, private bcl::Uncopyable {
public:
  typedef tsar::Bimap<
    bcl::tagged<clang::ForStmt *, tsar::AST>,
    bcl::tagged<llvm::Loop *, tsar::IR>> LoopMatcher;

  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  LoopMatcherPass() : FunctionPass(ID) {
    initializeLoopMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Returns loop matcher for the last analyzed function.
  const LoopMatcher & getMatcher() const noexcept { return mMatcher; }

  /// Returns high-level declaration of the last analyzed function.
  clang::Decl * getFunctionDecl() const noexcept { return mFuncDecl; }

private:
  LoopMatcher mMatcher;
  clang::Decl * mFuncDecl;
};
}

#endif//TSAR_LOOP_MATCHER_H