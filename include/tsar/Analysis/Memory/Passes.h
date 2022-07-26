//===- Passes.h - Create and Initialize Memory Analysis Passes  -*- C++ -*-===//
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
// of TSAR passes which are necessary for analysis of memory accesses.
// Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_ANALYSIS_PASSES_H
#define TSAR_MEMORY_ANALYSIS_PASSES_H

#include <functional>

namespace tsar {
class DIMemoryTrait;
}

namespace llvm {
class Pass;
class PassRegistry;
class FunctionPass;
class ImmutablePass;
class ModulePass;

/// Initialize all passes to perform analysis of memory accesses.
void initializeMemoryAnalysis(PassRegistry &Registry);

/// Initialize a pass to find defined locations for each data-flow region.
void initializeDefinedMemoryPassPass(PassRegistry &Registry);

/// Create a pass to find defined locations for each data-flow region.
FunctionPass * createDefinedMemoryPass();

/// Initialize a pass to find live locations for each data-flow region.
void initializeLiveMemoryPassPass(PassRegistry &Registry);

/// Create a pass to find live locations for each data-flow region.
FunctionPass * createLiveMemoryPass();

/// Initialize a pass to build hierarchy of accessed memory.
void initializeEstimateMemoryPassPass(PassRegistry &Registry);

/// Create a pass to build hierarchy of accessed memory.
FunctionPass * createEstimateMemoryPass();

/// Initialize a pass to build hierarchy of accessed memory.
void initializeDIEstimateMemoryPassPass(PassRegistry &Registry);

/// Create a pass to build hierarchy of accessed memory.
FunctionPass * createDIEstimateMemoryPass();

/// Initializes storage of debug-level memory environment.
void initializeDIMemoryEnvironmentStoragePass(PassRegistry &Registry);

/// Create storage of debug-level memory environment.
ImmutablePass * createDIMemoryEnvironmentStorage();

/// Initialize wrapper to access debug-level memory environment.
void initializeDIMemoryEnvironmentWrapperPass(PassRegistry &Registry);

/// Initialize a pass to display alias tree.
void initializeAliasTreeViewerPass(PassRegistry &Registry);

/// Create a pass to display alias tree.
FunctionPass * createAliasTreeViewerPass();

/// Initialize a pass to display alias tree (alias summary only).
void initializeAliasTreeOnlyViewerPass(PassRegistry &Registry);

/// Create a pass to display alias tree (alias summary only).
FunctionPass * createAliasTreeOnlyViewerPass();

/// Initialize a pass to display alias tree.
void initializeDIAliasTreeViewerPass(PassRegistry &Registry);

/// Create a pass to display alias tree.
FunctionPass * createDIAliasTreeViewerPass();

/// Initialize a pass to print alias tree to 'dot' file.
void initializeAliasTreePrinterPass(PassRegistry &Registry);

/// Create a pass to print alias tree to 'dot' file.
FunctionPass * createAliasTreePrinterPass();

/// Initialize a pass to print alias tree to 'dot' file (alias summary only).
void initializeAliasTreeOnlyPrinterPass(PassRegistry &Registry);

/// Create a pass to print alias tree to 'dot' file (alias summary only).
FunctionPass * createAliasTreeOnlyPrinterPass();

/// Initialize a pass to print alias tree to 'dot' file.
void initializeDIAliasTreePrinterPass(PassRegistry &Registry);

/// Create a pass to print alias tree to 'dot' file.
FunctionPass * createDIAliasTreePrinterPass();

/// Initialize storage of metadata-level pool of memory traits.
void initializeDIMemoryTraitPoolStoragePass(PassRegistry &Registry);

/// Create storage of metadata-level pool of memory traits.
ImmutablePass * createDIMemoryTraitPoolStorage();

/// Initialize wrapper to access metadata-level pool of memory traits.
void initializeDIMemoryTraitPoolWrapperPass(PassRegistry &Registry);

/// Initialize a pass to determine privatizable variables.
void initializePrivateRecognitionPassPass(PassRegistry &Registry);

/// Create a pass to determine privatizable variables.
FunctionPass * createPrivateRecognitionPass();

/// Initializes a pass to analyze private variables (at metadata level).
void initializeDIDependencyAnalysisPassPass(PassRegistry &Registry);

/// Create a pass to classify data dependency at metadata level.
///
/// This includes privatization, reduction and induction variable recognition
/// and flow/anti/output dependencies exploration.
FunctionPass * createDIDependencyAnalysisPass(bool IsInitialization = false);

/// Initialize a pass to process traits in a region.
void initializeProcessDIMemoryTraitPassPass(PassRegistry &Registry);

/// Create a pass to process traits in a region.
Pass * createProcessDIMemoryTraitPass(
  const std::function<void(tsar::DIMemoryTrait &)> &Lock);

/// Initialized a pass to look for not initialized memory locations.
void initializeNotInitializedMemoryAnalysisPass(PassRegistry &Registry);

/// Create a pass to look for not initialized memory locations.
FunctionPass * createNotInitializedMemoryAnalysis(PassRegistry &Registry);

/// Initialize a pass to delinearize array accesses.
void initializeDelinearizationPassPass(PassRegistry &Registry);

/// Create a pass to delinearize array accesses.
FunctionPass * createDelinearizationPass();

/// Initialize a pass to perform iterprocedural live memory analysis.
void initializeGlobalLiveMemoryPass(PassRegistry& Registry);

/// Create a pass to perform iterprocedural live memory analysis.
ModulePass * createGlobalLiveMemoryPass();

/// Initialize a pass to store results of interprocedural live memory analysis.
void initializeGlobalLiveMemoryStoragePass(PassRegistry &Registry);

/// Create a pass to store results of interprocedural live memory analysis.
ImmutablePass *createGlobalLiveMemoryStorage();

/// Initialize a pass to access results of interprocedural live memory analysis.
void initializeGlobalLiveMemoryWrapperPass(PassRegistry &Registry);

/// Initialize a pass to perform iterprocedural analysis of defined memory
/// locations.
void initializeGlobalDefinedMemoryPass(PassRegistry &Registry);

/// Create a pass to perform iterprocedural analysis of defined memory
/// locations.
ModulePass * createGlobalDefinedMemoryPass();

/// Initialize a pass to store results of interprocedural reaching definition
/// analysis.
void initializeGlobalDefinedMemoryStoragePass(PassRegistry &Registry);

/// Create a pass to store results of interprocedural reaching definition
/// analysis.
ImmutablePass *createGlobalDefinedMemoryStorage();

/// Initialize a pass to access results of interprocedural reaching definition
/// analysis.
void initializeGlobalDefinedMemoryWrapperPass(PassRegistry &Registry);

/// Create analysis server.
ModulePass *createDIMemoryAnalysisServer();

/// Initialize analysis server.
void initializeDIMemoryAnalysisServerPass(PassRegistry &Registry);

/// Initialize a pass to store list of array accesses.
void initializeDIArrayAccessStoragePass(PassRegistry &Registry);

/// Create a pass to store list of array accesses.
ImmutablePass *createDIArrayAccessStorage();

/// Initialize a pass to access information about array accesses in the program.
void initializeDIArrayAccessWrapperPass(PassRegistry &Registry);

/// Initialize a pass to collect array accesses.
void initializeDIArrayAccessCollectorPass(PassRegistry &Registry);

/// Create a pass to collect array accesses.
ModulePass *createDIArrayAccessCollector();

/// Initialize a pass to access alias results for allocas.
void initializeAllocasAAWrapperPassPass(PassRegistry &Regitsry);

/// Create a pass to access alias results for allocas.
ImmutablePass *createAllocasAAWrapperPass();

// Initialize a pass to store explicit accesses to global values in a function.
void initializeGlobalsAccessStoragePass(PassRegistry &Registry);

// Create a pass to store explicit accesses to global values in a function.
ImmutablePass *createGlobalsAccessStorage();

// Initialize a pass to collect explicit accesses to global values in a
// function.
void initializeGlobalsAccessCollectorPass(PassRegistry &Registry);

// Create a pass to collect explicit accesses to global values in a function.
ModulePass * createGlobalsAccessCollector();

// Initialize a pass to access list of explicit accesses to global
// values in a function.
void initializeGlobalsAccessWrapperPass(PassRegistry &Registry);
}
#endif//TSAR_MEMORY_ANALYSIS_PASSES_H
