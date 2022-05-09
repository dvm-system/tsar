//===- Passes.h- Create and Initialize Transform Passes (Clang) -*- C++ -*-===//
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
// of TSAR passes which performs transformation of LLVM IR however depends on
// source-level information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MIXED_TRANSFORM_PASSES_H
#define TSAR_MIXED_TRANSFORM_PASSES_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class Pass;
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize IR-level transformation passes which depends on
/// source-level information.
void initializeMixedTransform(PassRegistry &Registry);

/// Initialize a pass to perform low-level (LLVM IR) instrumentation of program.
void initializeInstrumentationPassPass(PassRegistry &Registry);

/// Create a pass to perform low-level (LLVM IR) instrumentation of program.
ModulePass * createInstrumentationPass(llvm::StringRef InstrEntry = "",
  llvm::ArrayRef<std::string> StartFrom = {});

/// Initialize a pass which retrieves some debug information for a loop if
/// it is not presented in LLVM IR.
void initializeDILoopRetrieverPassPass(PassRegistry &Registry);

/// Create a pass which retrieves some debug information for a loop if
/// it is not presented in LLVM IR.
Pass * createDILoopRetrieverPass();

/// Initialize a pass which retrieves some debug information for global values
/// if it is not presented in LLVM IR.
void initializeDINodeRetrieverPassPass(PassRegistry &Registry);

/// Create a pass which retrieves some debug information for global values
/// if it is not presented in LLVM IR.
ModulePass * createDINodeRetrieverPass();

/// Initialize a pass which retrieves alias information for dummy arguments from
/// the source code.
void initializeFlangDummyAliasAnalysisPass(PassRegistry &Registry);

/// Create a pass which retrieves alias information for dummy arguments from
/// the source code.
FunctionPass *createFlangDummyAliasAnalysis();

/// Initialize a pass which retrieves metadata for Fortran
/// variables if it is not presented properly in LLVM IR.
void initializeFlangDIVariableRetrieverPassPass(PassRegistry &Registry);

/// Create a pass which retrieves metadata for Fortran
/// variables if it is not presented properly in LLVM IR.
ModulePass *createFlangDIVariableRetrieverPass();
}
#endif//TSAR_MIXED_TRANSFORM_PASSES_H
