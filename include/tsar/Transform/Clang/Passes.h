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
// of TSAR passes which is necessary for source-to-source transformation of C
// programs. Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_TRANSFORM_PASSES_H
#define TSAR_CLANG_TRANSFORM_PASSES_H

namespace llvm {
class ImmutablePass;
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize all source-to-source transformation passes.
void initializeClangTransform(PassRegistry &Registry);

/// Initialize a pass to replace the occurrences of variables
/// with direct assignments.
void initializeClangExprPropagationPass(PassRegistry &Registry);

/// Create a pass to replace the occurrences of variables
/// with direct assignments.
FunctionPass *createClangExprPropagation();

/// Initializes a pass to perform source-level inline expansion using Clang.
void initializeClangInlinerPassPass(PassRegistry& Registry);

/// Creates a pass to perform source-level inline expansion using Clang.
llvm::ModulePass *createClangInlinerPass();

/// Creates a pass to perform source-level object renaming.
llvm::ModulePass * createClangRenameLocalPass();

/// Initializes a pass to perform source-level object renaming.
void initializeClangRenameLocalPassPass(PassRegistry &Registry);

/// Creates a pass to perform elimination of dead declarations.
FunctionPass * createClangDeadDeclsElimination();

/// Initializes a pass to perform elimination of dead declarations.
void initializeClangDeadDeclsEliminationPass(PassRegistry &Registry);

/// Initialize a pass to perform OpenMP-based parallelization.
void initializeClangOpenMPParallelizationPass(PassRegistry &Registry);

/// Create a pass to perform OpenMP-based parallelization.
ModulePass* createClangOpenMPParallelization();

/// Initialize a storage for DVMH-base parallelization.
void initializeDVMHParallelizationContextPass(PassRegistry &Registry);

/// Create a storage for DVMH-base parallelization.
ImmutablePass *createDVMHParallelizationContext();

/// Initialize a pass to perform IPO of data transfer between CPU and GPU
/// memories.
void initializeDVMHDataTransferIPOPassPass(PassRegistry &Registry);

/// Create a pass to perform IPO of data transfer between CPU and GPU
/// memories.
ModulePass *createDVMHDataTransferIPOPass();

/// Initialize a pass to unparse DVMH-based parallelization.
void initializeClangDVMHWriterPass(PassRegistry &Registry);

/// Create a pass to unparse DVMH-based parallelization.
ModulePass *createClangDVMHWriter();

/// Initialize a pass to perform DVMH-based parallelization for shared memory.
void initializeClangDVMHSMParallelizationPass(PassRegistry &Registry);

/// Create a pass to perform DVMH-based parallelization for shared memory.
ModulePass* createClangDVMHSMParallelization();

/// Create pass to perform replacement of access to structure fields
/// with separate variables.
ModulePass * createClangStructureReplacementPass();

/// Initialize a pass to perform replacement of access to structure fields
/// with separate variables.
void initializeClangStructureReplacementPassPass(PassRegistry &Registry);

/// Create a pass to intergchange loops in a loop nest.
FunctionPass * createClangLoopInterchange();

/// Initialize a pass to intergchange loops in a loop nest.
void initializeClangLoopInterchangePass(PassRegistry &Registry);

/// Initialize a pass to reverse loop.
void initializeClangLoopReversePass(PassRegistry &Registry);

/// Create a pass to reverse loop.
ModulePass *createClangLoopReverse();

/// Initialize a pass to eliminate unreachable code.
void initializeClangUnreachableCodeEliminationPassPass(PassRegistry &Registry);

/// Create a pass to eliminate unreachable code.
FunctionPass *createClangUnreachableCodeEliminationPass();

}
#endif//TSAR_CLANG_TRANSFORM_PASSES_H
