//===- tsar_instrumentation.h - TSAR Instrumentation Engine -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file defines LLVM IR level instrumentation engine.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_INSTRUMENTATION_ENGINE_H
#define TSAR_INSTRUMENTATION_ENGINE_H

#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_pass.h"

namespace tsar {
// Implement hear all functionalyti that is not related to LLVM directly.

}

namespace llvm {
class Function;

/// This per-module pass performs instrumentation of LLVM IR.
class InstrumentationPass :
  public ModulePass, bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  InstrumentationPass() : ModulePass(ID) {
    initializeInstrumentationPassPass(*PassRegistry::getPassRegistry());
  }

  /// Implements of the per-module instrumentation pass.
  bool runOnModule(Module &M) override;

  /// \brief Releases allocated memory when it is no longer needed.
  void releaseMemory() override;

  /// \brief Set analysis information that is necessary to run this pass.
  ///
  /// If a pass specifies that it uses a particular analysis result to this
  /// function, it can then use the getAnalysis<AnalysisType>() function, below.
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

#endif//TSAR_INSTRUMENTATION_H
