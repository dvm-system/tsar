//===- Passes.h ------- Create and Initialize APC Passes --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// It contains declarations of functions that initialize and create an instances
// of TSAR passes which are necessary for program parallelization. Declarations
// of appropriate methods for an each new pass should be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_APC_PASSES_H
#define TSAR_APC_PASSES_H

namespace llvm {
class Pass;
class ImmutablePass;
class FunctionPass;
class ModulePass;
class PassRegistry;

/// Initializes all APC passes.
void initializeAPC(PassRegistry &Registry);

/// Initializes wrapper to access auto-parallelization context.
void initializeAPCContextWrapperPass(PassRegistry &Registry);

/// Initializes a storage for auto-parallelization context.
void initializeAPCContextStoragePass(PassRegistry &Registry);

/// Creates a storage for auto-parallelization context.
ImmutablePass * createAPCContextStorage();

/// Initializes a pass to collect general information about loops.
void initializeAPCLoopInfoBasePassPass(PassRegistry &Registry);

/// Creates a pass to collect general information about loops.
FunctionPass * createAPCLoopInfoBasePass();

/// Initialize a pass to collect arrays.
void initializeAPCArrayInfoPassPass(PassRegistry &Registry);

/// Create a pass to collect arrays.
FunctionPass * createAPCArrayInfoPass();

/// Initialize a pass to collect functions.
void initializeAPCFunctionInfoPassPass(PassRegistry &Registry);

/// Create a pass to collect function.
ModulePass * createAPCFunctionInfoPass();

/// Initialize a core pass to perform program parallelization.
void initializeAPCParallelizationPassPass(PassRegistry &Registry);

/// Create a core pass to perform program parallelization.
ModulePass * createAPCParallelizationPass();

/// Create a pass to insert DVMH-directives into a source program.
ModulePass * createAPCClangDVMHWriter();

/// Initialize a pass to insert DVMH-directives into a source program.
void initializeAPCClangDVMHWriterPass(PassRegistry &Registry);

/// Create a pass to determine limitations for data distribution.
FunctionPass *createAPCDistrLimitsChecker();

/// Initialize a pass  to determine limitations for data distribution.
void initializeAPCDistrLimitsCheckerPass(PassRegistry &Registry);

/// Create a pass to determine limitations for data distribution.
ModulePass *createAPCDistrLimitsIPOChecker();

/// Initialize a pass  to determine limitations for data distribution.
void initializeAPCDistrLimitsIPOCheckerPass(PassRegistry &Registry);

/// Create a pass to collect user directives.
ModulePass *createAPCClangDirectivesCollector();

/// Initialize a pass to collect user directives.
void initializeAPCClangDirectivesCollectorPass(PassRegistry &Registry);

/// Initialize a pass to print diagnostic messages.
void initializeAPCClangDiagnosticPrinterPass(PassRegistry &Registry);

/// Create a pass to print diagnostic messages.
ModulePass *createAPCClangDiagnosticPrinter();
}

#endif//TSAR_APC_PASSES_H
