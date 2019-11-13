//===- Passes.h - Create and Initialize Parallelization Passes --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// of TSAR analysis passes which determine opportunities of program
// parallelization. Declarations of appropriate methods for an each new pass
// should be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PARALLEL_ANALYSIS_PASSES_H
#define TSAR_PARALLEL_ANALYSIS_PASSES_H

namespace llvm {
class PassRegistry;
class FunctionPass;

/// Initialize analysis passes which determine opportunities of program
/// parallelization.
void initializeParallelizationAnalysis(PassRegistry &Registry);

/// Initialize a pass to determine loops which could be executed
/// in a parallel way.
void initializeParallelLoopPassPass(PassRegistry &Registry);

/// Initialize a pass to determine loops which could be executed
/// in a parallel way.
FunctionPass *createParallelLoopPass();
}
#endif//TSAR_PARALLEL_ANALYSIS_PASSES_H
