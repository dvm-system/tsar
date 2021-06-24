//=== Passes.cpp - Create and Initialize Memory Analysis Passes -*- C++ -*-===//
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
// This contains functions to initialize passes which are necessary for
// analysis of memory accesses.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/Passes.h"

using namespace llvm;

void llvm::initializeMemoryAnalysis(PassRegistry &Registry) {
  initializeDefinedMemoryPassPass(Registry);
  initializeLiveMemoryPassPass(Registry);
  initializeEstimateMemoryPassPass(Registry);
  initializeDIEstimateMemoryPassPass(Registry);
  initializeAliasTreeViewerPass(Registry);
  initializeAliasTreeOnlyViewerPass(Registry);
  initializeDIAliasTreeViewerPass(Registry);
  initializeAliasTreePrinterPass(Registry);
  initializeAliasTreeOnlyPrinterPass(Registry);
  initializeDIAliasTreePrinterPass(Registry);
  initializePrivateRecognitionPassPass(Registry);
  initializeDIDependencyAnalysisPassPass(Registry);
  initializeProcessDIMemoryTraitPassPass(Registry);
  initializeNotInitializedMemoryAnalysisPass(Registry);
  initializeDelinearizationPassPass(Registry);
  initializeGlobalDefinedMemoryPass(Registry);
  initializeGlobalLiveMemoryPass(Registry);
  initializeDIArrayAccessWrapperPass(Registry);
  initializeAllocasAAWrapperPassPass(Registry);
  initializeGlobalsAccessWrapperPass(Registry);
}
