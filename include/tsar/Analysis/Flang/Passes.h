//===- Passes.h - Create and Initialize Analysis Passes (Flang) -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// of TSAR passes which are necessary for source-based analysis of Fortran
// programs.
// Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_ANALYSIS_PASSES_H
#define TSAR_FLANG_ANALYSIS_PASSES_H

namespace llvm {
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize all passes to perform source-base analysis of Fortran programs.
void initializeFlangAnalysis(PassRegistry &Registry);

/// Initialize a pass to match high-level and low-level expressions.
void initializeFlangExprMatcherPassPass(PassRegistry &Registry);

/// Create a pass to match high-level and low-level expressions.
FunctionPass * createFlangExprMatcherPass();

}
#endif//TSAR_FLANG_ANALYSIS_PASSES_H
