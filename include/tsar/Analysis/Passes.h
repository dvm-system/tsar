//===- Passes.h - Create and Initialize Analysis Passes (Base) --*- C++ -*-===//
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
// of general TSAR analysis passes. Declarations of appropriate methods for an
// each new pass should be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_PASSES_H
#define TSAR_ANALYSIS_PASSES_H

namespace llvm {
class PassRegistry;
class PassInfo;
class FunctionPass;
class ModulePass;
class raw_ostream;

/// Initialize base analysis passes.
void initializeAnalysisBase(PassRegistry &Registry);

/// Create a pass to print internal state of the specified pass after the
/// last execution.
///
/// To use this function it is necessary to override
/// void `llvm::Pass::print(raw_ostream &O, const Module *M) const` method for
/// a function pass internal state of which must be printed.
FunctionPass * createFunctionPassPrinter(const PassInfo *PI, raw_ostream &OS);

/// Create a pass to print internal state of the specified pass after the
/// last execution.
///
/// To use this function it is necessary to override
/// void `llvm::Pass::print(raw_ostream &O, const Module *M) const` method for
/// a function pass internal state of which must be printed.
ModulePass * createModulePassPrinter(const PassInfo *PI, raw_ostream &OS);

/// Initialize a pass to build hierarchy of data-flow regions.
void initializeDFRegionInfoPassPass(PassRegistry &Registry);

/// Create a pass to build hierarchy of data-flow regions.
FunctionPass * createDFRegionInfoPass();


}
#endif//TSAR_ANALYSIS_PASSES_H
