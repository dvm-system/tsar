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
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class Module;

/// This pass tries to reformat sources which have been transformed by
/// previous passes.
class ClangFormatPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  /// \brief Creates pass which adds a specified suffix to transformed sources
  /// performs formatting if NoFormat is not set (default).
  ///
  /// Note, if suffix is specified it will be added before the file extension.
  /// If suffix is empty, the original will be stored to
  /// <filenmae>.<extension>.orig and then it will be overwritten.
  ClangFormatPass(llvm::StringRef OutputSuffix ="", bool NoFormat = false) :
    ModulePass(ID), mOutputSuffix(OutputSuffix), mNoFormat(NoFormat) {
    initializeClangFormatPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
private:
  std::string mOutputSuffix;
  bool mNoFormat;
};
}
#endif//TSAR_CLANG_FORMAT_PASS_H
