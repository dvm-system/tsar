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
// of TSAR passes which performs transformation of LLVM IR. Declarations of
// appropriate methods for an each new pass should be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_IR_TRANSFORM_PASSES_H
#define TSAR_IR_TRANSFORM_PASSES_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class Pass;
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize all IR-level transformation passes.
void initializeIRTransform(PassRegistry &Registry);

/// Initialize a pass which eliminates dead stores which accesses values without
/// attached metadata.
void initializeNoMetadataDSEPassPass(PassRegistry &Registry);

/// Create a pass which eliminates dead stores which accesses values without
/// attached metadata.
FunctionPass * createNoMetadataDSEPass();

/// Initialize a pass which deduce function attributes in PO.
void initializePOFunctionAttrsAnalysisPass(PassRegistry &Registry);

/// Create a pass which deduce function attributes in PO.
Pass * createPOFunctionAttrsAnalysis();

/// Initialize a pass which deduce function attributes in RPO.
void initializeRPOFunctionAttrsAnalysisPass(PassRegistry &Registry);

/// Create a pass which deduce function attributes in RPO.
ModulePass * createRPOFunctionAttrsAnalysis();

/// Initialize a pass which deduce loop attributes.
void initializeLoopAttributesDeductionPassPass(PassRegistry &Registry);

/// Create a pass which deduce loop attributes.
FunctionPass * createLoopAttributesDeductionPass();

/// Initialize a pass which extract each call instruction
///(except debug instructions) into its own new basic block.
void initializeCallExtractorPassPass(PassRegistry& Registry);

/// Create a pass which extract each call instruction
/// (except debug instructions) into its own new basic block.
FunctionPass* createCallExtractorPass();

/// Initialize a pass which deduces function memory attributes.
void initializeFunctionMemoryAttrsAnalysisPass(PassRegistry &Registry);

/// Create a pass which deduces function memory attributes.
FunctionPass *createFunctionMemoryAttrsAnalysis();

/// Initialize a pass which marks functions which should be inlined to improve
/// data-dependence analysis.
void initializeDependenceInlinerAttributerPass(PassRegistry &Registry);

/// Create a pass which marks functions which should be inlined to improve
/// data-dependence analysis.
ModulePass *createDependenceInlinerAttributer();

/// Initialize an inliner pass which handle functions which are necessary for
/// analysis.
void initializeDependenceInlinerPassPass(PassRegistry &Registry);

/// Create an inliner pass which handle functions which are necessary for
/// analysis.
Pass *createDependenceInlinerPass(bool InsertLifetime = true);

/// Initialize a pass calculating preserved parameters.
void initializeNoCaptureAnalysisPass(PassRegistry &Registry);

/// Create a pass calculating preserved parameters.
Pass * createNoCaptureAnalysisPass();

/// Initialize a pass which attempts to promote pointer values to registers.
void initializePointerScalarizerPassPass(PassRegistry &Registry);

/// Create a pass which attempts to promote pointer values to registers.
FunctionPass *createPointerScalarizerPass();
}
#endif//TSAR_IR_TRANSFORM_PASSES_H
