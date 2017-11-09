//=== CanonicalLoop.cpp - High Level Canonical Loop Analyzer ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to identify canonical for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#include "CanonicalLoop.h"
#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "tsar_loop_matcher.h"
#include "MemoryAccessUtils.h"
#include "tsar_memory_matcher.h"
#include "SpanningTreeRelation.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <typeinfo>

using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "canonical-loop"

STATISTIC(NumCanonical, "Number of canonical  for-loops");
STATISTIC(NumNonCanonical, "Number of non-canonical for-loops");

char CanonicalLoopPass::ID = 0;
INITIALIZE_PASS_BEGIN(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true)

namespace {
/// This class visits and analyzes all matched for-loops in a source code.
class CanonicalLoopLabeler : public MatchFinder::MatchCallback {
public:
  /// Creates visitor.
  explicit CanonicalLoopLabeler(Rewriter &R, DFRegionInfo &DFRI,
      const LoopMatcherPass::LoopMatcher &LM, DefinedMemoryInfo &DefI,
      const MemoryMatchInfo::MemoryMatcher &MM, AliasTree &AT,
      TargetLibraryInfo &TLI, tsar::CanonicalLoopInfo *CLI) : mRw(&R),
      mRgnInfo(&DFRI), mLoopInfo(&LM), mDefInfo(&DefI), mMemoryMatcher(&MM),
      mAliasTree(AT), mTLI(TLI), mCanonicalLoopInfo(CLI),
      mSTR(SpanningTreeRelation<const AliasTree *>(&AT)) {}

  /// This function is called each time LoopMatcher finds appropriate loop
  virtual void run(const MatchFinder::MatchResult &Result) {
    const ForStmt *FS = Result.Nodes.getNodeAs<clang::ForStmt>("forLoop");
    ASTContext *Context = Result.Context;
    // We do not want to convert header files!
    if (!FS||!Context->getSourceManager().isWrittenInMainFile(FS->getForLoc()))
      return;
    const VarDecl *UnIncVar = Result.Nodes.getNodeAs
        <VarDecl>("UnIncVarName");
    const VarDecl *BinIncVar = Result.Nodes.getNodeAs
        <VarDecl>("BinIncVarName");
    const VarDecl *InitVar = Result.Nodes.getNodeAs
        <VarDecl>("InitVarName");
    const VarDecl *AssignmentVar = Result.Nodes.getNodeAs
        <VarDecl>("AssignmentVarName"); 
    const VarDecl *FirstAssignmentVar = Result.Nodes.getNodeAs
        <VarDecl>("FirstAssignmentVarName"); 
    const VarDecl *SecondAssignmentVar = Result.Nodes.getNodeAs
        <VarDecl>("SecondAssignmentVarName");
    const VarDecl *FirstConditionVar = Result.Nodes.getNodeAs
        <VarDecl>("FirstConditionVarName"); 
    const VarDecl *SecondConditionVar = Result.Nodes.getNodeAs
        <VarDecl>("SecondConditionVarName");
    if (InitVar && ((UnIncVar && sameVar(UnIncVar, InitVar)) ||
        (BinIncVar && sameVar(BinIncVar, InitVar)) ||
        (AssignmentVar && sameVar(AssignmentVar, InitVar) &&
        ((FirstAssignmentVar && sameVar(InitVar, FirstAssignmentVar)) ||
        (SecondAssignmentVar && sameVar(InitVar, SecondAssignmentVar))))) &&
        ((FirstConditionVar && sameVar(InitVar, FirstConditionVar)) ||
        (SecondConditionVar && sameVar(InitVar, SecondConditionVar)))) {
      auto Match = mLoopInfo->find<AST>(const_cast<ForStmt*>(FS));
      tsar::DFNode* Region;
      if (Match != mLoopInfo->end())
        Region = mRgnInfo->getRegionFor(Match->get<IR>());
      else
        Region = nullptr;
      if (Region) {
        if (CheckLoop(Region,
            const_cast<VarDecl*>(InitVar->getCanonicalDecl()))) {
          auto PLInfo = mCanonicalLoopInfo->insert(Region);
          ++NumCanonical;
          return;
        }
      }
    }
    ++NumNonCanonical;
  }

private:
  /// Checks whether two ValueDecl declares same variable or not
  bool sameVar(const ValueDecl *First, const ValueDecl *Second) {
    return First->getCanonicalDecl() == Second->getCanonicalDecl();
  }

  bool CheckLoop(tsar::DFNode* Region, VarDecl *Var) {
    auto MemMatch = mMemoryMatcher->find<AST>(Var);
    if (MemMatch == mMemoryMatcher->end()) {
      return false;
    }
    auto alloca = MemMatch->get<IR>();
    auto type = (llvm::dyn_cast<llvm::PointerType>(alloca->getType()))->getElementType();
    if (!type->isSized()) {
      return false;
    }
    llvm::MemoryLocation MemLoc(alloca, 1);
    if (tsar::DFLoop* DFFor = llvm::dyn_cast<DFLoop>(Region)) {
      for (auto I = DFFor->node_begin(); I != DFFor->node_end(); ++I) {
        auto Match = mDefInfo->find(*I);
        if (Match != mDefInfo->end()) {
          auto &DUS = Match->get<DefUseSet>();
          if ((DUS->hasDef(MemLoc)) ||  (DUS->hasMayDef(MemLoc))) {
            if (tsar::DFBlock* DFB = llvm::dyn_cast<DFBlock>(*I)) {
              if (DFFor->getLoop()->isLoopLatch(DFB->getBlock())) {
                auto EMI = mAliasTree.find(MemLoc);
                assert(EMI && "Estimate memory location must not be null!");
                auto ANI = EMI->getAliasNode(mAliasTree);
                assert(ANI && "Alias node must not be null!");
                auto &AT = mAliasTree;
                auto &STR = mSTR;
                bool unreachable = true;
                bool *res = &unreachable;
                for_each_memory(*(DFB->getBlock()), mTLI,
                  [&AT, &STR, &ANI, &res] (Instruction &I, MemoryLocation &&Loc,
                      unsigned Idx, AccessInfo R, AccessInfo W) {
                    auto EM = AT.find(Loc);
                    assert(EM && "Estimate memory location must not be null!");
                    auto AN = EM->getAliasNode(AT);
                    assert(AN && "Alias node must not be null!");
                    if (!(STR.isEqual(ANI, AN) || STR.isUnreachable(ANI, AN)))
                      *res = false;
                  },
                  [&AT, &STR, &ANI, &res](Instruction &I, AccessInfo,
                      AccessInfo) {
                    auto AN = AT.findUnknown(I);
                    if (!AN)
                      return;
                    if (!(STR.isEqual(ANI, AN) || STR.isUnreachable(ANI, AN)))
                      *res = false;
                  }
                );
                if (unreachable)
                  return true;
              }
            }
          }
        }
      }
    }
    return false;
  }
  
  Rewriter* mRw;
  DFRegionInfo *mRgnInfo;
  const LoopMatcherPass::LoopMatcher *mLoopInfo;
  DefinedMemoryInfo *mDefInfo;
  const MemoryMatchInfo::MemoryMatcher *mMemoryMatcher;
  tsar::AliasTree &mAliasTree;
  TargetLibraryInfo &mTLI;
  tsar::CanonicalLoopInfo *mCanonicalLoopInfo;
  SpanningTreeRelation<const AliasTree *> mSTR;
};

/// returns LoopMatcher that matches loops that can be canonical
StatementMatcher makeLoopMatcher() {
  return forStmt(
      hasLoopInit(eachOf(
        declStmt(hasSingleDecl(
          varDecl(hasInitializer(integerLiteral()))
          .bind("InitVarName"))),
        binaryOperator(
          hasOperatorName("="),
          hasLHS(declRefExpr(to(
            varDecl(hasType(isInteger()))
            .bind("InitVarName"))))))),
      hasIncrement(eachOf(
        unaryOperator(
          eachOf(
            hasOperatorName("++"),
            hasOperatorName("--")),
          hasUnaryOperand(declRefExpr(to(
            varDecl(hasType(isInteger()))
            .bind("UnIncVarName"))))),
        binaryOperator(
          eachOf(
            hasOperatorName("+="),
            hasOperatorName("-=")),
          hasLHS(declRefExpr(to(
            varDecl(hasType(isInteger()))
            .bind("BinIncVarName"))))),
        binaryOperator(
          hasOperatorName("="),
          hasLHS(declRefExpr(to(
            varDecl(hasType(isInteger()))
            .bind("AssignmentVarName")))),
          hasRHS(eachOf(
            binaryOperator(
              hasOperatorName("+"),
              eachOf(
                hasLHS(implicitCastExpr(
                  hasImplicitDestinationType(isInteger()),
                  hasSourceExpression(declRefExpr(to(
                    varDecl(hasType(isInteger()))
                    .bind("FirstAssignmentVarName")))))),
                hasRHS(implicitCastExpr(
                  hasImplicitDestinationType(isInteger()),
                  hasSourceExpression(declRefExpr(to(
                    varDecl(hasType(isInteger()))
                    .bind("SecondAssignmentVarName")))))))),
            binaryOperator(
              hasOperatorName("-"),
              hasLHS(implicitCastExpr(
                hasImplicitDestinationType(isInteger()),
                hasSourceExpression(declRefExpr(to(
                  varDecl(hasType(isInteger()))
                  .bind("FirstAssignmentVarName")))))))))))),
      hasCondition(binaryOperator(
        eachOf(
          hasOperatorName("<"),
          hasOperatorName("<="),
          hasOperatorName("="),
          hasOperatorName(">="),
          hasOperatorName(">")),
        eachOf(
          hasLHS(implicitCastExpr(
            hasImplicitDestinationType(isInteger()),
            hasSourceExpression(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("FirstConditionVarName")))))),
          hasRHS(implicitCastExpr(
            hasImplicitDestinationType(isInteger()),
            hasSourceExpression(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("SecondConditionVarName"))))))))))
  .bind("forLoop");
}
}

bool CanonicalLoopPass::runOnFunction(Function &F) {
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto &RgnInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &LoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  auto &DefInfo = getAnalysis<DefinedMemoryPass>().getDefInfo();
  auto &MemInfo =
    getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher;
  auto &ATree = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  StatementMatcher LoopMatcher = makeLoopMatcher();
  MatchFinder Finder;
  CanonicalLoopLabeler Labeler(Rwriter, RgnInfo, LoopInfo, DefInfo, MemInfo,
      ATree, TLI, &mCanonicalLoopInfo);
  Finder.addMatcher(LoopMatcher, &Labeler);
  Finder.matchAST(FuncDecl->getASTContext());
  return false;
}

void CanonicalLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createCanonicalLoopPass() {
  return new CanonicalLoopPass();
}
