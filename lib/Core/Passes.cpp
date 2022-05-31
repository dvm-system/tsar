//===------ Passes.cpp ---- Initialize TSAR Passes --------------*- C++ -*-===//
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
// This contains functions to initialize passes that are implemented in TSAR.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/Passes.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Core/tsar-config.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Transform/AST/Passes.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/Passes.h"
#include "tsar/Transform/Mixed/Passes.h"
#ifdef FLANG_FOUND
# include "tsar/Analysis/Flang/Passes.h"
# include "tsar/Transform/Flang/Passes.h"
#endif
#ifdef APC_FOUND
# include "tsar/APC/Passes.h"
#endif

using namespace llvm;

void llvm::initializeTSAR(PassRegistry &Registry) {
  initializeGlobalOptionsImmutableWrapperPass(Registry);
  initializeTransformationEnginePassPass(Registry);
  initializeAnalysisBase(Registry);
  initializeMemoryAnalysis(Registry);
  initializeAnalysisReader(Registry);
  initializeParallelizationAnalysis(Registry);
  initializeClangAnalysis(Registry);
  initializeIRTransform(Registry);
  initializeMixedTransform(Registry);
  initializeASTTransform(Registry);
  initializeClangTransform(Registry);
#ifdef FLANG_FOUND
  initializeFlangAnalysis(Registry);
  initializeFlangTransform(Registry);
#endif
#ifdef APC_FOUND
  initializeAPC(Registry);
#endif
}

void initializeTSAR(LLVMPassRegistryRef R) {
  initializeTSAR(*unwrap(R));
}
