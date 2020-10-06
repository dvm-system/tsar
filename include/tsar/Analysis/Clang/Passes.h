//===- Passes.h - Create and Initialize Analysis Passes (Clang) -*- C++ -*-===//
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
// of TSAR passes which are necessary for source-based analysis of C programs.
// Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_ANALYSIS_PASSES_H
#define TSAR_CLANG_ANALYSIS_PASSES_H

namespace llvm {
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize all passes to perform source-base analysis of C programs.
void initializeClangAnalysis(PassRegistry &Registry);

/// Initialize a pass which collects information about source-level globals.
void initializeClangGlobalInfoPassPass(PassRegistry &Registry);

/// Create a pass which collects information about source-level globals.
llvm::ModulePass * createClangGlobalInfoPass();

/// Initialize a pass to match variable in a source high-level code
/// and appropriate metadata-level representations of variables.
void initializeClangDIMemoryMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match variable in a source high-level code
/// and appropriate metadata-level representations of variables.
FunctionPass *createDIMemoryMatcherPass();

/// Initialize pass to check absence of a macro in source ranges which
/// are marked with `assert nomacro` directive.
void initializeClangNoMacroAssertPass(PassRegistry& Registry);

/// Initialize pass to check absence of a macro in source ranges which
/// are marked with `assert nomacro` directive.
llvm::FunctionPass * createClangNoMacroAssert(bool *IsInvalid = nullptr);

/// Initialize a pass to match variables and allocas (or global variables).
void initializeMemoryMatcherPassPass(PassRegistry &Registry);

/// Initialize a pass to match variables and allocas (or global variables).
void initializeMemoryMatcherImmutableStoragePass(PassRegistry &Registry);

/// Initialize a pass to match variables and allocas (or global variables).
void initializeMemoryMatcherImmutableWrapperPass(PassRegistry &Registry);

/// Create a pass to match variables and allocas (or global variables).
ModulePass * createMemoryMatcherPass();

/// Initialize a pass to match only global variable in a source high-level code
/// and appropriate metadata-level representations of variables.
void initializeClangDIGlobalMemoryMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match only global variable in a source high-level code
/// and appropriate metadata-level representations of variables.
ModulePass *createDIGlobalMemoryMatcherPass();

/// Initialize a pass to match high-level and low-level expressions.
void initializeClangExprMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match high-level and low-level expressions.
FunctionPass * createClangExprMatcherPass();

/// Initialize a pass to match high-level and low-level loops.
void initializeLoopMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match high-level and low-level loops.
FunctionPass * createLoopMatcherPass();

/// Initialize a pass to determine perfect for-loops in a source code.
void initializeClangPerfectLoopPassPass(PassRegistry &Registry);

/// Create a pass to determine perfect for-loops in a source code.
FunctionPass * createClangPerfectLoopPass();

/// Initialize a pass to determine canonical for-loops in a source code.
void initializeCanonicalLoopPassPass(PassRegistry &Registry);

/// Create a pass to determine canonical for-loops in a source code.
FunctionPass * createCanonicalLoopPass();

/// Initialize an AST-level pass which collects control-flow traits
/// for function and its loops.
void initializeClangCFTraitsPassPass(PassRegistry &Registry);

/// Create an AST-level pass which collects control-flow traits
/// for function and its loops.
FunctionPass * createClangCFTraitsPass();

/// Initialize a pass to collect '#pragma spf region' directives.
void initializeClangRegionCollectorPass(PassRegistry &Registry);

/// Create a pass to collect '#pragma spf region' directives.
ModulePass * createClangRegionCollector();

/// Initialize a pass to build file hierarchy.
void initializeClangIncludeTreePassPass(PassRegistry &Registry);

/// Create a pass to build file hierarchy.
ModulePass *createClangIncludeTreePass();

/// Initialize a pass to print source file tree to 'dot' file.
void initializeClangIncludeTreePrinterPass(PassRegistry &Registry);

/// Create a pass to print source file tree to 'dot' file.
ModulePass *createClangIncludeTreePrinter();

/// Initialize a pass to print source file tree to 'dot' file (filenames
/// instead of full paths).
void initializeClangIncludeTreeOnlyPrinterPass(PassRegistry &Registry);

/// Create a pass to print source file tree to 'dot' file (filenames
/// instead of full paths).
ModulePass *createClangIncludeTreeOnlyPrinter();

/// Initialize a pass to display source file tree.
void initializeClangIncludeTreeViewerPass(PassRegistry &Registry);

/// Create a pass to display source file tree.
ModulePass *createClangIncludeTreeViewer();

/// Initialize a pass to display source file tree (filenames instead of
/// full paths).
void initializeClangIncludeTreeOnlyViewerPass(PassRegistry &Registry);

/// Create a pass to display source file tree (filenames instead of
/// full paths).
ModulePass *createClangIncludeTreeOnlyViewer();
}
#endif//TSAR_CLANG_ANALYSIS_PASSES_H
