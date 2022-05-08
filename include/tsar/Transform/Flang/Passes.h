//===- Passes.h- Create and Initialize Transform Passes (Flang) -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// of TSAR passes which is necessary for source-to-source transformation of
// Fortran programs. Declarations of appropriate methods for an each new pass
// should be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_TRANSFORM_PASSES_H
#define TSAR_FLANG_TRANSFORM_PASSES_H

namespace llvm {
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize all source-to-source transformation passes.
void initializeFlangTransform(PassRegistry &Registry);

/// Initialize a pass to replace constants with variables with values equal
/// to the replaced constants.
void initializeFlangConstantReplacementPassPass(PassRegistry &Registry);

/// Create a pass to replace constants with variables with values equal
/// to the replaced constants.
FunctionPass *createFlangConstantReplacementPass();

/// Initialize a pass to register all varaiables in a program unit.
void initializeFlangVariableRegistrationPassPass(PassRegistry &Registry);

/// Create a pass to register all varaiables in a program unit.
FunctionPass *createFlangVariableRegistrationPass();
} // namespace llvm

#endif//TSAR_FLANG_TRANSFORM_PASSES_H
