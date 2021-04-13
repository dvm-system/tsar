//===---------------------------------------------------------------------===//
//
// This file defines a pass to eliminate unreachable calls in a source code.
//
//===---------------------------------------------------------------------===//

#include "tsar/Transform/Clang/UnreachableCallsElimination.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/UnreachableCountedRegions.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LegacyPassManager.h>
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
#define DEBUG_TYPE "clang-unreachable-calls"

class ClangCopyPropagationInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &PM) const override {
    PM.add(createClangUnreachableCountedRegions());
  }
};

char ClangUnreachableCallsElimination::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangUnreachableCallsElimination, "clang-unreachable-calls",
  "Unreachable Calls Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangCopyPropagationInfo);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangUnreachableCountedRegions)
INITIALIZE_PASS_IN_GROUP_END(ClangUnreachableCallsElimination, "clang-unreachable-calls",
  "Unreachable Calls Elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {
class CallExprVisitor : public RecursiveASTVisitor<CallExprVisitor> {
public:
  explicit CallExprVisitor(clang::Rewriter &Rewriter,
      const std::vector<llvm::coverage::CountedRegion> &Unreachable)
    : mRewriter(&Rewriter),
    mUnreachable(Unreachable),
    mSourceManager(Rewriter.getSourceMgr()) {}

  inline clang::SourceLocation
  shiftTokenIfSemi(clang::SourceLocation Loc) {
  Token SemiTok;
  return (!getRawTokenAfter(Loc, mSourceManager, mRewriter->getLangOpts(), SemiTok)
      && SemiTok.is(tok::semi))
      ? SemiTok.getLocation()
      : Loc;
  }

  bool TraverseCallExpr(CallExpr *CE) {
    if (!CE) {
      return true;
    }

    auto Res = RecursiveASTVisitor::TraverseCallExpr(CE);

    clang::SourceLocation BeginLoc = CE->getBeginLoc();
    clang::SourceLocation EndLoc = CE->getEndLoc();

    unsigned LineStart = mSourceManager.getSpellingLineNumber(BeginLoc);
    unsigned ColumnStart = mSourceManager.getSpellingColumnNumber(BeginLoc);
    unsigned LineEnd = mSourceManager.getSpellingLineNumber(EndLoc);
    unsigned ColumnEnd = mSourceManager.getSpellingColumnNumber(EndLoc);

    std::cout << "CallExpr SourceLocation:" << std::endl;
    std::cout << "\tstart: " << LineStart << ":" << ColumnStart
        << ", end: " << LineEnd << ":" << ColumnEnd << std::endl;

    for (const auto &CR : mUnreachable) {
      if ((CR.LineStart < LineStart || (CR.LineStart == LineStart && CR.ColumnStart <= ColumnStart))
          && (CR.LineEnd > LineEnd || (CR.LineEnd == LineEnd && CR.ColumnEnd >= ColumnEnd))) {
        EndLoc = shiftTokenIfSemi(EndLoc);
        clang::SourceRange SourceRangeToRemove{ BeginLoc, EndLoc };
        mRewriter->RemoveText(SourceRangeToRemove);
        break;
      }
    }

    return Res;
  }

  // bool TraverseStmt(Stmt *S) {
  //   if (!S)
  //     return true;
  //   auto Res = RecursiveASTVisitor::TraverseStmt(S);
  //   return Res;
  // }

private:
  clang::Rewriter *mRewriter;
  clang::SourceManager &mSourceManager;
  const std::vector<llvm::coverage::CountedRegion> &mUnreachable;
};
}


ClangUnreachableCallsElimination::ClangUnreachableCallsElimination() : FunctionPass(ID) {
  initializeClangUnreachableCallsEliminationPass(*PassRegistry::getPassRegistry());
}

bool ClangUnreachableCallsElimination::runOnFunction(Function &F) {
  errs() << "Run on function ";
  errs().write_escaped(F.getName()) << '\n';

  auto *M = F.getParent();

  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  auto *TfmCtx{TfmInfo ? TfmInfo->getContext(*M) : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M->getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }

  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;

  const auto &Unreachable = getAnalysis<ClangUnreachableCountedRegions>().getUnreachable();
  CallExprVisitor Visitor(TfmCtx->getRewriter(), Unreachable);
  Visitor.TraverseDecl(FuncDecl);

  return false;
}

void ClangUnreachableCallsElimination::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<ClangUnreachableCountedRegions>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangUnreachableCallsElimination() {
  return new ClangUnreachableCallsElimination();
}
