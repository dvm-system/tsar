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
// of TSAR passes which is necessary for analysis of memory accesses.
// Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_ANALYSIS_PASSES_H
#define TSAR_MEMORY_ANALYSIS_PASSES_H

namespace llvm {
class PassRegistry;
class FunctionPass;

/// Initialize all passes to perfrom anlysis of memory accesses.
void initializeMemoryAnalysis(PassRegistry &Registry);

/// Initialize a pass to determine privatizable variables.
void initializePrivateRecognitionPassPass(PassRegistry &Registry);

/// Create a pass to determine privatizable variables.
FunctionPass * createPrivateRecognitionPass();

/// Initializes a pass to analyze private variables (at metadata level).
void initializeDIDependencyAnalysisPassPass(PassRegistry &Registry);

/// \brief Creates a pass to classify data dependency at metadata level.
///
/// This includes privatization, reduction and induction variable recognition
/// and flow/anti/output dependencies exploration.
FunctionPass * createDIDependencyAnalysisPass();
}
#endif//TSAR_MEMORY_ANALYSIS_PASSES_H
