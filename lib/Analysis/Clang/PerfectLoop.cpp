//=== PerfectLoop.cpp - High Level Perfect Loop Analyzer --------*- C++ -*-===//
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
// This file implements classes to identify perfect for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Tags.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/PostOrderIterator.h>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "perfect-loop"

STATISTIC(NumPerfect, "Number of perfectly nested for-loops");
STATISTIC(NumImPerfect, "Number of imperfectly nested for-loops");

char ClangPerfectLoopPass::ID = 0;
INITIALIZE_PASS_BEGIN(ClangPerfectLoopPass, "perfect-loop",
  "Perfectly Nested Loop Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_END(ClangPerfectLoopPass, "perfect-loop",
  "Perfectly Nested Loop Analysis", true, true)

namespace {
/// This visits and analyzes all for-loops in a source code.
class LoopVisitor : public RecursiveASTVisitor<LoopVisitor> {
public:
  /// Creates visitor.
  explicit LoopVisitor(DFRegionInfo &DFRI,
      const LoopMatcherPass::LoopMatcher &LM, tsar::PerfectLoopInfo &PLI) :
    mRgnInfo(&DFRI), mLoopInfo(&LM), mPerfectLoopInfo(&PLI) {}

  /// \brief Overridden traversing of for-loops.
  ///
  /// For a specified loop a number of inner loops are calculated. This function
  /// also checks whether some statements are placed between heads or tails of
  /// loops are calculated.
  /// \attention Only statements comprises body of a loop is going
  /// to be visited.
  bool TraverseForStmt(ForStmt *For) {
    // Keep values of external loop if it exists.
    int PrevNumberOfLoops = ++mNumberOfLoops;
    bool PrevIsThereOperators = mIsThereStmt;
    mNumberOfLoops = 0;
    mIsThereStmt = false;
    auto Res = RecursiveASTVisitor::TraverseStmt(For->getBody());
    if (mNumberOfLoops == 1 && !mIsThereStmt || mNumberOfLoops == 0) {
      ++NumPerfect;
      auto Match = mLoopInfo->find<AST>(For);
      if (Match != mLoopInfo->end()) {
        auto Region = mRgnInfo->getRegionFor(Match->get<IR>());
        auto PLInfo = mPerfectLoopInfo->insert(Region);
      }
    } else {
      ++NumImPerfect;
    }
    // Return values of external loop.
    mNumberOfLoops = PrevNumberOfLoops;
    mIsThereStmt = PrevIsThereOperators;
    return true;
  }

  /// Checks that a specified statement makes a currently evaluated loop
  /// imperfect.
  bool VisitStmt(Stmt *S) {
    if (!isa<CompoundStmt>(S))
      mIsThereStmt = true;
    return true;
  }

private:
  /// This is number of loops,existence of none-cycle operators
  /// inside the analyzed one's body
  /// Number of inner loops for a current loop.
  int mNumberOfLoops;

  /// Existence of none-loop statement inside the analyzed one's body.
  bool mIsThereStmt;

  DFRegionInfo *mRgnInfo;
  const LoopMatcherPass::LoopMatcher *mLoopInfo;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;
};
}

bool ClangPerfectLoopPass::runOnFunction(Function &F) {
  releaseMemory();
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto &RgnInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &LoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  LoopVisitor Visitor(RgnInfo, LoopInfo, mPerfectLoopInfo);
  Visitor.TraverseDecl(FuncDecl);
  return false;
}

void ClangPerfectLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequiredTransitive<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangPerfectLoopPass() {
  return new ClangPerfectLoopPass();
}
