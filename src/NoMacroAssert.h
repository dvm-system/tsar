//===--- NoMacroAssert.h --- No Macro Assert (Clang) ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines a pass which checks absence of a macro in a specified
// source range marked with `#pragma spf assert nomacro`. Note, that all
// preprocessor directives (except #pragma) are also treated as a macros.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_ASSERT_NO_MACRO_H
#define TSAR_CLANG_ASSERT_NO_MACRO_H

#include "tsar_pass.h"
#include <llvm/Pass.h>
#include <utility.h>

namespace llvm {
/// Checks absence of a macro in source ranges which are marked with
/// `assert nomacro` directive.
class ClangNoMacroAssert : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  ClangNoMacroAssert(bool *IsInvalid = nullptr) :
      FunctionPass(ID), mIsInvalid(IsInvalid) {
    initializeClangNoMacroAssertPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
private:
  bool *mIsInvalid = nullptr;
};
}

#endif//TSAR_CLANG_ASSERT_NO_MACRO_H
