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

/// Initializes all passes developed for TSAR project
void initializeTSAR(PassRegistry &Registry);

/// Initializes a pass to builde hierarchy of data-flow regions.
void initializeDFRegionInfoPassPass(PassRegistry &Registry);

/// Creates a pass to builde hierarchy of data-flow regions.
FunctionPass * createDFRegionInfoPass();

/// Initializes a pass to find must defined locations for each data-flow region.
void initializeDefinedMemoryPassPass(PassRegistry &Registry);

/// Creates a pass to find must defined locations for each data-flow region.
FunctionPass * createDefinedMemoryPass();

/// Initializes a pass to find live locations for each data-flow region.
void initializeLiveMemoryPassPass(PassRegistry &Registry);

/// Creates a pass to find live locations for each data-flow region.
FunctionPass * createLiveMemoryPass();

/// Initializes a pass to analyze private variables.
void initializePrivateRecognitionPassPass(PassRegistry &Registry);

/// Creates a pass to analyze private variables.
FunctionPass * createPrivateRecognitionPass();

/// Initializes a pass to fetch private variables before they will be promoted
/// to registers or removed.
void initializeFetchPromotePrivatePassPass(PassRegistry &Registry);

/// Creates a pass to fetch private variables before they will be promoted
/// to registers or removed.
FunctionPass * createFetchPromotePrivatePass();

/// Initializes a pass to access source level transformation enginer.
void initializeTransformationEnginePassPass(PassRegistry &Registry);

/// Creates a pass to make more precise analysis of for-loops in C sources.
FunctionPass * createPrivateCClassifierPass();

/// Initializes a pass to make more precise analysis of for-loops in C sources.
void initializePrivateCClassifierPassPass(PassRegistry &Registry);

/// Creates a pass to access source level transformation enginer.
ImmutablePass * createTransformationEnginePass();

/// Initializes a pass to perform low-level (LLVM IR) instrumentation of program.
void initializeInstrumentationPassPass(PassRegistry &Registry);

/// Creates a pass to perform low-level (LLVM IR) instrumentation of program.
FunctionPass * createInstrumentationPass();

/// Initialize a pass to match high-level and low-level loops.
void initializeLoopMatcherPassPass(PassRegistry &Registry);

/// Creates a pass to match high-level and low-level loops.
FunctionPass * createLoopMatcherPass();

/// Initialize a pass to match variables and allocas (or global variables).
void initializeMemoryMatcherPassPass(PassRegistry &Registry);

/// Creates a pass to match variables and allocas (or global variables).
ModulePass * createMemoryMatcherPass();

/// Creates a pass to print internal state of the specified pass after the
/// last execution.
FunctionPass * createFunctionPassPrinter(const PassInfo *PI, raw_ostream &OS);

/// Initializes a pass to print results of a test.
void initializeTestPrinterPassPass(PassRegistry &Registry);

/// Creates a pass to print results of a test.
ModulePass * createTestPrinterPass();
}

#endif//TSAR_PASS_H
