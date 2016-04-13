//===---- tsar_pass.h --------- Create TSAR Passes --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions that is necessary to combine TSAR and LLVM.
// It contains declarations definitions of functions that create an instances
// of TSAR passes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PASS_H
#define TSAR_PASS_H

namespace llvm {
class FunctionPass;
class ModulePass;

/// Creates a pass to analyze private variables.
FunctionPass * createPrivateRecognitionPass();

/// Creates a pass to make more precise analysis of for-loops in C sources.
FunctionPass * createPrivateCClassifierPass();

/// Creates a pass to initialize source code rewriter.
ModulePass * createRewriterInitializationPass();
}

#endif//TSAR_PASS_H
