//===---- tsar_pass.h --------- Create TSAR Passes --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions that is necessary to combine TSAR and LLVM.
// It contains declarations definitions of functions that initialize and
// create an instances of TSAR passes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PASS_H
#define TSAR_PASS_H

namespace llvm {
class PassRegistry;
class FunctionPass;
class ModulePass;
class ImmutablePass;

/// Initialize all passes developed for TSAR project
void initializeTSAR(PassRegistry &Registry);

/// Initialize a pass to analyze private variables.
void initializePrivateRecognitionPassPass(PassRegistry &Registry);

/// Creates a pass to analyze private variables.
FunctionPass * createPrivateRecognitionPass();

/// Initialize a pass to access source level transformation enginer.
void initializeTransformationEnginePassPass(PassRegistry &Registry);

/// Creates a pass to make more precise analysis of for-loops in C sources.
FunctionPass * createPrivateCClassifierPass();

/// Initialize a pass to make more precise analysis of for-loops in C sources.
void initializePrivateCClassifierPassPass(PassRegistry &Registry);

/// Creates a pass to access source level transformation enginer.
ImmutablePass * createTransformationEnginePass();

/// Initialize a pass to perform low-level (LLVM IR) instrumentation of program.
void initializeInstrumentationPassPass(PassRegistry &Registry);

/// Creates a pass to perform low-level (LLVM IR) instrumentation of program.
FunctionPass * createInstrumentationPass();
}

#endif//TSAR_PASS_H
