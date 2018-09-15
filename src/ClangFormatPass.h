//===--- ClangFormatPass.h - Source-level Reformat Pass (Clang) -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file declares a pass to reformat sources after transformations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_FORMAT_PASS_H
#define TSAR_CLANG_FORMAT_PASS_H

#include "tsar_pass.h"
#include <llvm/Pass.h>
#include <utility.h>

namespace llvm {
class Module;

/// \brief This pass tries to reformat sources which have been transformed by
/// previous passes.
///
/// clang::tooling can not apply replacements over rewritten sources, they can
/// be applied only over original non-modified sources. So, modifications
/// should be written to files by previous passes.
class ClangFormatPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  ClangFormatPass() : ModulePass(ID) {
    initializeClangFormatPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_CLANG_FORMAT_PASS_H
