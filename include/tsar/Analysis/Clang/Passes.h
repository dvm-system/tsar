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
// of TSAR passes which is necessary for source-based analysis of C programs.
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

/// Initialize all passes to perfrom source-base analysis of C programs.
void initializeClangAnalysis(PassRegistry &Registry);

/// Initialize a pass to match variable in a source high-level code
/// and appropriate metadata-level representations of variables.
void initializeClangDIMemoryMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match variable in a source high-level code
/// and appropriate metadata-level representations of variables.
FunctionPass *createDIMemoryMatcherPass();

/// Initializes pass to check absence of a macro in source ranges which
/// are marked with `assert nomacro` directive.
void initializeClangNoMacroAssertPass(PassRegistry& Registry);

/// Initializes pass to check absence of a macro in source ranges which
/// are marked with `assert nomacro` directive.
llvm::FunctionPass * createClangNoMacroAssert(bool *IsInvalid = nullptr);

/// Initialize a pass to match only global variable in a source high-level code
/// and appropriate metadata-level representations of variables.
void initializeClangDIGlobalMemoryMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match only global variable in a source high-level code
/// and appropriate metadata-level representations of variables.
ModulePass *createDIGlobalMemoryMatcherPass();
}
#endif//TSAR_CLANG_ANALYSIS_PASSES_H
