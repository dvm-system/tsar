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
class PassRegistry;
class FunctionPass;
class ModulePass;
class StringRef;

/// Initialize all source-to-source transformation passes.
void initializeClangTransform(PassRegistry &Registry);

/// Initializes a pass to reformat sources after transformation using Clang.
void initializeClangFormatPassPass(PassRegistry& Registry);

/// Creates a pass to reformat sources after transformation using Clang.
llvm::ModulePass *createClangFormatPass();

/// Creates a pass to reformat sources after transformation using Clang.
llvm::ModulePass* createClangFormatPass(
  llvm::StringRef OutputSuffix, bool NoFormat);

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
}
/// Creates a pass to perform swapping of loops.
FunctionPass * createClangLoopSwapping();

/// Initializes a pass to perform swapping of loops.
void initializeClangLoopSwappingPass(PassRegistry &Registry);

}

#endif//TSAR_CLANG_TRANSFORM_PASSES_H
