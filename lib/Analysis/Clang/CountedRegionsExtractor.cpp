//=== CountedRegionsExtractor.cpp - Extracting Of CountedRegions From Code Coverage (Clang) --*- C++ -*===//
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
//===---------------------------------------------------------------------===//
//
// This file defines a pass to extract & store 
// all the CountedRegions along with their ExecutionCounts
// from a given code coverage file.
//
//===---------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/CountedRegionsExtractor.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>

#include <iostream>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-counted-regions-extractor"

char ClangCountedRegionsExtractor::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangCountedRegionsExtractor, "clang-counted-regions-extractor",
  "Extracting Of CountedRegions From Code Coverage (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangCountedRegionsExtractor, "clang-counted-regions-extractor",
  "Extracting Of CountedRegions From Code Coverage (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

ClangCountedRegionsExtractor::ClangCountedRegionsExtractor() : ImmutablePass(ID) {
  initializeClangCountedRegionsExtractorPass(*PassRegistry::getPassRegistry());
}

// Extracts & stores CountedRegions from code coverage
void ClangCountedRegionsExtractor::initializePass() {
  std::string ObjectFilename{ "/home/alex000/Documents/sapfor-total/working/main" };
  std::string ProfileFilename{ "/home/alex000/Documents/sapfor-total/working/main.profdata" };

  StringRef ObjectFilenameRef{ ObjectFilename };
  ArrayRef<StringRef> ObjectFilenameRefs{ ObjectFilenameRef };
  StringRef ProfileFilenameRef{ ProfileFilename };

  auto CoverageOrErr = coverage::CoverageMapping::load(ObjectFilenameRefs, ProfileFilenameRef);
  if (!CoverageOrErr) {
    errs() << "COVERAGE ERROR\n";
    return;
  }

  LLVM_DEBUG(llvm:dbgs() << "Coverage loading done\n";);

  const auto &Coverage = CoverageOrErr->get();
  
  for (auto &FRI : Coverage->getCoveredFunctions()) {
    for (auto CR : FRI.CountedRegions) {
        CountedRegions.push_back(CR);
    }
  }

  LLVM_DEBUG(llvm:dbgs() << "Stored CountedRegions:\n";);
  for (const auto &CR : CountedRegions) {
    LLVM_DEBUG(
      llvm:dbgs() << "\tstart: " << CR.LineStart << ":" << CR.ColumnStart
      << ", end: " << CR.LineEnd << ":" << CR.ColumnEnd
      << ", count: " << CR.ExecutionCount << "\n";
    );
  }
  LLVM_DEBUG(llvm:dbgs() << "\n";);
}

const std::vector<coverage::CountedRegion> &
ClangCountedRegionsExtractor::getCountedRegions() const noexcept {
	return CountedRegions;
}

void ClangCountedRegionsExtractor::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

ImmutablePass *llvm::createClangCountedRegionsExtractor() {
  return new ClangCountedRegionsExtractor();
}
