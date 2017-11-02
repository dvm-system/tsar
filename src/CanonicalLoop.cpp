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
#include "tsar_loop_matcher.h"
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
INITIALIZE_PASS_END(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true)

namespace {
/// This class visits and analyzes all matched for-loops in a source code.
class CanonicalLoopLabeler : public MatchFinder::MatchCallback {
public:
  /// Creates visitor.
  explicit CanonicalLoopLabeler(Rewriter &R, DFRegionInfo &DFRI,
      const LoopMatcherPass::LoopMatcher &LM, DefinedMemoryInfo &DefI,
      const MemoryMatchInfo::MemoryMatcher &MM,
      const MemoryMatchInfo::MemoryASTSet &MS, llvm::Module *M,
      tsar::CanonicalLoopInfo *CLI) : mRw(&R), mRgnInfo(&DFRI), mLoopInfo(&LM),
      mDefInfo(&DefI), mMemoryMatcher(&MM), mMemoryUnmatched(&MS), mModule(M),
      mCanonicalLoopInfo(CLI) {}

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
        auto PLInfo = mCanonicalLoopInfo->insert(Region);
        ++NumCanonical;
      } else {
        ++NumNonCanonical;
      }
    } else {
      ++NumNonCanonical;
    }
  }

private:
  /// Checks whether two ValueDecl declares same variable or not
  bool sameVar(const ValueDecl *First, const ValueDecl *Second) {
    return First->getCanonicalDecl() == Second->getCanonicalDecl();
  }
  
  DFRegionInfo *mRgnInfo;
  const LoopMatcherPass::LoopMatcher *mLoopInfo;
  DefinedMemoryInfo *mDefInfo;
  const MemoryMatchInfo::MemoryMatcher *mMemoryMatcher;
  const MemoryMatchInfo::MemoryASTSet *mMemoryUnmatched;
  llvm::Module *mModule;
  tsar::CanonicalLoopInfo *mCanonicalLoopInfo;
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
  auto &MemUnmatched =
    getAnalysis<MemoryMatcherImmutableWrapper>()->UnmatchedAST;
  StatementMatcher LoopMatcher = makeLoopMatcher();
  MatchFinder Finder;
  CanonicalLoopLabeler Labeler(RgnInfo, LoopInfo, &mCanonicalLoopInfo);
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
  AU.setPreservesAll();
}

FunctionPass *llvm::createCanonicalLoopPass() {
  return new CanonicalLoopPass();
}
