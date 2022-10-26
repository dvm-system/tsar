//=== Passes.h - Create and Initialize Analysis Passes (Reader) -*- C++ -*-===//
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
// of TSAR passes which are necessary to load external analysis results.
// Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_READER_PASSES_H
#define TSAR_ANALYSIS_READER_PASSES_H

#include <llvm/ADT/StringRef.h>

namespace llvm {
class PassRegistry;
class FunctionPass;
class ModulePass;

/// Initialize all passes which is necessary to load external analysis results.
void initializeAnalysisReader(PassRegistry &Registry);

/// Create a reader of external analysis results.
///
/// Files with external results must be specified in GlobalOptions::AnalysisUse
/// option.
/// If `Filename` is empty `GlobalOptions::AnalysisUse` value is used.
FunctionPass * createAnalysisReader();

/// Initialize a reader of external analysis results.
void initializeAnalysisReaderPass(PassRegistry &Registry);

/// Create a writer of analysis results.
ModulePass * createAnalysisWriter();

/// Initialize a writer of analysis results.
void initializeAnalysisWriterPass(PassRegistry &Registry);

/// Initialize a pass to estimate region weights in a source code.
void initializeRegionWeightsEstimatorPass(PassRegistry &Registry);

/// Create a pass to estimate region weights in a source code.
ModulePass *createRegionWeightsEstimator();
}
#endif//TSAR_ANALYSIS_READER_PASSES_H
