//=== PerfectLoop.cpp - High Level Perfect Loop Analyzer --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements classes to identify perfect for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#include "PerfectLoop.h"
#include "tsar_transformation.h"
#include "tsar_loop_matcher.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <typeinfo>

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
      const LoopMatcherPass::LoopMatcher &LM, tsar::PerfectLoopInfo *PLI) : 
       mRgnInfo(&DFRI), mLoopInfo(&LM), mPerfectLoopInfo(PLI) {}

  /// Inserts appropriate pragma before a for-loop.
  /// Calls TraverseStmt() that walks through the body of cycle
  bool TraverseForStmt(ForStmt *For) {
    // Keeping values of external cycle if it exists
    int PrevNumberOfLoops = ++mNumberOfLoops;
    bool PrevIsThereOperators = mIsThereOperators;
    // Starting analysis
    mNumberOfLoops = 0;
    mIsThereOperators = false;
    // Here goes traverse
    auto Res = RecursiveASTVisitor::TraverseStmt(For->getBody());
    // Getting match AST <--> IR with ForStmt (AST)
    auto Match = mLoopInfo->find<AST>(static_cast<Stmt*>(For));
    // Getting DFNode* with IR
    tsar::DFNode* Region;
    if (Match != mLoopInfo->end())
      Region = mRgnInfo->getRegionFor(Match->get<IR>());
    else
      Region = nullptr;
    // Analyzing data
    if (((mNumberOfLoops == 1) && (!mIsThereOperators)) || (mNumberOfLoops == 0)) {
      ++NumPerfect;
      // Adding info about this loop
      if (Region)
        auto PLInfo = mPerfectLoopInfo->insert(Region);
    } else {
      ++NumImPerfect;
    }
    // Analysis ended
    // Return values of external cycle
    mNumberOfLoops = PrevNumberOfLoops;
    mIsThereOperators = PrevIsThereOperators;
    return true;
  }

  /// Actually visiting statements
  /// Called from TraverseStmt(), replaces VisitStmt()
  bool VisitStmt(Stmt *Statement) {
      if (!llvm::isa<CompoundStmt>(Statement))
        mIsThereOperators = true;
      return true;
  }

private:
  // This is number of loops,existence of none-cycle operators
  // inside the analyzed one's body
  int mNumberOfLoops;
  bool mIsThereOperators;
  // Information about passes
  DFRegionInfo *mRgnInfo;
  const LoopMatcherPass::LoopMatcher *mLoopInfo;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;
};
}

bool ClangPerfectLoopPass::runOnFunction(Function &F) {
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto &RgnInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &LoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  // Information about loops will be in mPerfect after the Traverse
  LoopVisitor Visitor(RgnInfo, LoopInfo, &mPerfectLoopInfo);
  Visitor.TraverseDecl(FuncDecl);
  return false;
}

void ClangPerfectLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangPerfectLoopPass() {
  return new ClangPerfectLoopPass();
}
