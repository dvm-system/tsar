//===---------------------------------------------------------------------===//
//
// This file defines a pass to obtain
// unreachable CountedRegions from the given file.
//
//===---------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/UnreachableCountedRegions.h"
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
#define DEBUG_TYPE "clang-unreachable-counted-regions"

char ClangUnreachableCountedRegions::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangUnreachableCountedRegions, "clang-unreachable-counted-regions",
  "Unreachable Counted Regions (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangUnreachableCountedRegions, "clang-unreachable-counted-regions",
  "Unreachable Counted Regions (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

ClangUnreachableCountedRegions::ClangUnreachableCountedRegions() : ImmutablePass(ID) {
  initializeClangUnreachableCountedRegionsPass(*PassRegistry::getPassRegistry());
}

// obtains & stores unreachable CountedRegions
void ClangUnreachableCountedRegions::initializePass() {
  std::string ObjectFilename{ "/home/alex000/Documents/Sapfor/working/main" };
  std::string ProfileFilename{ "/home/alex000/Documents/Sapfor/working/main.profdata" };

  StringRef ObjectFilenameRef{ ObjectFilename };
  ArrayRef<StringRef> ObjectFilenameRefs{ ObjectFilenameRef };
  StringRef ProfileFilenameRef{ ProfileFilename };

  auto CoverageOrErr = llvm::coverage::CoverageMapping::load(ObjectFilenameRefs, ProfileFilenameRef);
  if (!CoverageOrErr) {
    errs() << "COVERAGE ERROR!!!\n";
    return;
  }

  std::cout << "Coverage loading done" << std::endl;

  const auto &Coverage = CoverageOrErr->get();  // std::unique_ptr<CoverageMapping> &Coverage
  
  for (auto &FRI : Coverage->getCoveredFunctions()) {  // FunctionRecordIterator &FRI
    for (auto CR : FRI.CountedRegions) {  // std::vector<CountedRegion> CountedRegions
      if (CR.ExecutionCount == 0) {
        Unreachable.push_back(CR);
      }
    }
  }

  std::cout << "Unreachable Counted Regions:" << std::endl;
  for (const auto &CR : Unreachable) {
    std::cout << "\tstart: " << CR.LineStart << ":" << CR.ColumnStart
        << ", end: " << CR.LineEnd << ":" << CR.ColumnEnd << std::endl;
  }
  std::cout << std::endl;
}

const std::vector<llvm::coverage::CountedRegion> &
ClangUnreachableCountedRegions::getUnreachable() const noexcept {
	return Unreachable;
}

void ClangUnreachableCountedRegions::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

ImmutablePass *llvm::createClangUnreachableCountedRegions() {
  return new ClangUnreachableCountedRegions();
}
