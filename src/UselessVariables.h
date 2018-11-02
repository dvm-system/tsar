//=== UselessVariables.h --- High Level Variables Analyzer -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===---------------------------------------------------------------------===//
//
// This file defines classes to find declarations of uselsess variables
// in a source code.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_USELESS_VARIABLES_H
#define TSAR_CLANG_USELESS_VARIABLES_H

#include "tsar_pass.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>





namespace llvm {
/// \brief This per-function pass determines perfect for-loops in a source code.
///
/// A for-loop is treated as perfect if it has no internal for-loops or if it
/// has only one internal for-loop and there are no other statements between
/// loop bounds.
class ClangUselessVariablesPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  ClangUselessVariablesPass() : FunctionPass(ID) {
    initializeClangUselessVariablesPassPass(*PassRegistry::getPassRegistry());
  }

  // Determines perfect loops in a specified functions.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

};
}
#endif// TSAR_CLANG_USELESS_VARIABLES_H
