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
#include "tsar_dbg_output.h"
#include "DefinedMemory.h"
#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "GlobalOptions.h"
#include "tsar_loop_matcher.h"
#include "MemoryAccessUtils.h"
#include "tsar_memory_matcher.h"
#include "tsar_query.h"
#include "SpanningTreeRelation.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "canonical-loop"

STATISTIC(NumCanonical, "Number of canonical for-loops");
STATISTIC(NumNonCanonical, "Number of non-canonical for-loops");

char CanonicalLoopPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

namespace {
/// This class visits and analyzes all matched for-loops in a source code.
class CanonicalLoopLabeler : public MatchFinder::MatchCallback {
public:
  /// Creates visitor.
  explicit CanonicalLoopLabeler(DFRegionInfo &DFRI,
      const LoopMatcherPass::LoopMatcher &LM, DefinedMemoryInfo &DefI,
      const MemoryMatchInfo::MemoryMatcher &MM, AliasTree &AT,
      TargetLibraryInfo &TLI, ScalarEvolution &SE, CanonicalLoopSet *CLI) :
      mRgnInfo(&DFRI), mLoopInfo(&LM), mDefInfo(&DefI), mMemoryMatcher(&MM),
      mAliasTree(&AT), mTLI(&TLI), mSE(&SE), mCanonicalLoopInfo(CLI),
      mSTR(&AT) {}

  /// \brief This function is called each time LoopMatcher finds appropriate
  /// loop.
  ///
  /// The same loop can be visited multiple times.
  virtual void run(const MatchFinder::MatchResult &Result) override {
    ++NumNonCanonical;
    auto *For = const_cast<ForStmt *>(
      Result.Nodes.getNodeAs<ForStmt>("forLoop"));
    if (!For)
      return;
    LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: process loop at ");
    LLVM_DEBUG(For->getLocStart().dump(Result.Context->getSourceManager()));
    LLVM_DEBUG(dbgs() << "\n");
    auto Match = mLoopInfo->find<AST>(For);
    if (Match == mLoopInfo->end()) {
      LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: unmatched loop found.\n");
      return;
    }
    auto *Region = cast<DFLoop>(mRgnInfo->getRegionFor(Match->get<IR>()));
    if (mCanonicalLoopInfo->find_as(Region) != mCanonicalLoopInfo->end()) {
      LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: loop is already checked.\n");
      --NumNonCanonical;
      return;
    }
    const clang::Expr *Init;
    if(auto *D = Result.Nodes.getNodeAs<clang::DeclStmt>("LoopInitDecl"))
      Init = cast<clang::VarDecl>(D->getSingleDecl())->getInit();
    else
      Init = Result.Nodes.
        getNodeAs<clang::BinaryOperator>("LoopInitAssignment")->getRHS();
    if (!Init) {
      LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: loop without initialization.\n");
      return;
    }
    auto *InitVar = Result.Nodes.getNodeAs<VarDecl>("InitVarName");
    assert(InitVar && "InitVar must not be null!");
    auto *UnIncVar = Result.Nodes.getNodeAs<VarDecl>("UnIncVarName");
    auto *BinIncVar = Result.Nodes.getNodeAs<VarDecl>("BinIncVarName");
    auto *AssignmentVar = Result.Nodes.getNodeAs<VarDecl>("AssignmentVarName");
    auto *FirstAssignmentVar =
      Result.Nodes.getNodeAs<VarDecl>("FirstAssignmentVarName");
    auto *SecondAssignmentVar =
      Result.Nodes.getNodeAs<VarDecl>("SecondAssignmentVarName");
    if (!(UnIncVar && sameVar(UnIncVar, InitVar) ||
          BinIncVar && sameVar(BinIncVar, InitVar) ||
          AssignmentVar && sameVar(AssignmentVar, InitVar) &&
            (FirstAssignmentVar && sameVar(InitVar, FirstAssignmentVar) ||
             SecondAssignmentVar && sameVar(InitVar, SecondAssignmentVar))))
      return;
    auto *FirstConditionVar =
      Result.Nodes.getNodeAs<VarDecl>("FirstConditionVarName");
    auto *SecondConditionVar =
      Result.Nodes.getNodeAs<VarDecl>("SecondConditionVarName");
    bool ReversedCond = false;
    // Note that at least one variable `FirstConditionVar` or
    // `SecondConditionVar` is equal to `nullptr`. For details, see
    // `makeLoopMatcher()` and `eachOf()` matcher.
    if (SecondConditionVar && sameVar(InitVar, SecondConditionVar))
      ReversedCond = true;
    else if (!(FirstConditionVar && sameVar(InitVar, FirstConditionVar)))
      return;
    auto *UnaryIncr =
      Result.Nodes.getNodeAs<clang::UnaryOperator>("UnaryIncr");
    auto *BinaryIncr =
      Result.Nodes.getNodeAs<clang::BinaryOperator>("BinaryIncr");
    auto *Condition =
      Result.Nodes.getNodeAs<clang::BinaryOperator>("LoopCondition");
    assert(Condition && "Condition must not be null!");
    bool CoherentCond =
      UnaryIncr && coherent(
        Init, UnaryIncr, Condition, ReversedCond, Result.Context) ||
      BinaryIncr && coherent(
        Init, BinaryIncr, Condition, ReversedCond, Result.Context);
    if (!CoherentCond) {
      LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: condition and increment is not consistent.\n");
      return;
    }
    LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: syntactically canonical loop found.\n");
    auto *LI = new CanonicalLoopInfo(Region, For);
    mCanonicalLoopInfo->insert(LI);
    checkLoop(Region, const_cast<VarDecl*>(InitVar->getCanonicalDecl()), LI);
    if (LI->isCanonical()) {
      LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: canonical loop found.\n");
      ++NumCanonical;
      --NumNonCanonical;
    }
  }

private:
  /// Checks whether two ValueDecl declares same variable or not
  bool sameVar(const ValueDecl *First, const ValueDecl *Second) {
    return First->getCanonicalDecl() == Second->getCanonicalDecl();
  }

  /// Checks coherence of increment and condition.
  ///
  /// Only simple checks are performed. Static analysis can not perform this
  /// check in general case. However, under the conditions that loop conforms
  /// other canonical conditions this check is not essential. Otherwise,
  /// the loop never ends.
  /// \param [in] Init Initialization expression.
  /// \param [in] Incr Increment expression.
  /// \param [in] Condition Condition expression.
  /// \param [in] ReversedCond This flag specifies whether induction variable
  /// is presented at the right hand side.
  /// \return `false` if it is proved that increment and condition
  /// are conflicted.
  bool coherent(const clang::Expr *Init, const clang::UnaryOperator *Incr,
      const clang::BinaryOperator *Condition, bool ReversedCond,
      ASTContext *Ctx) {
    bool Increment = Incr->getOpcode() == UO_PostInc ||
      Incr->getOpcode() == UO_PreInc;
    bool LessCondition = Condition->getOpcode() == BO_LT ||
      Condition->getOpcode() == BO_LE;
    APSInt Start, End;
    if (Init->isIntegerConstantExpr(Start, *Ctx) &&
        (ReversedCond ? Condition->getLHS()->isIntegerConstantExpr(End, *Ctx) :
          Condition->getRHS()->isIntegerConstantExpr(End, *Ctx)))
      if (Increment && Start >= End || !Increment && Start <= End)
        return false;
    return Increment && LessCondition && !ReversedCond ||
      Increment && !LessCondition && ReversedCond ||
      !Increment && !LessCondition && !ReversedCond ||
      !Increment && LessCondition && ReversedCond;
  }

  /// Checks coherence of increment and condition.
  ///
  /// Only simple checks are performed. Static analysis can not perform this
  /// check in general case. However, under the conditions that loop conforms
  /// other canonical conditions this check is not essential. Otherwise,
  /// the loop never ends.
  /// \param [in] Init Initialization expression.
  /// \param [in] Incr Increment expression.
  /// \param [in] Condition Condition expression.
  /// \param [in] ReversedCond This flag specifies whether induction variable
  /// is presented at the right hand side.
  /// \return `false` if it is proved that increment and condition
  /// are conflicted.
  bool coherent(const clang::Expr *Init, const clang::BinaryOperator *Incr,
      const clang::BinaryOperator *Condition, bool ReversedCond,
      ASTContext *Ctx) {
    APSInt Step;
    // If step is not constant we can not prove anything.
    if (!Incr->getRHS()->isIntegerConstantExpr(Step, *Ctx) &&
        !Incr->getLHS()->isIntegerConstantExpr(Step, *Ctx))
      return true;
    if (Step.isNonNegative() && !Step.isStrictlyPositive())
      return false;
    bool Increment =
      Step.isStrictlyPositive() &&
        (Incr->getOpcode() == BO_Add || Incr->getOpcode() == BO_AddAssign) ||
      Step.isNegative() &&
        (Incr->getOpcode() == BO_Sub || Incr->getOpcode() == BO_SubAssign);
    bool LessCondition = Condition->getOpcode() == BO_LT ||
      Condition->getOpcode() == BO_LE;
    APSInt Start, End;
    if (Init->isIntegerConstantExpr(Start, *Ctx) &&
        (ReversedCond ? Condition->getLHS()->isIntegerConstantExpr(End, *Ctx) :
          Condition->getRHS()->isIntegerConstantExpr(End, *Ctx)))
      if (Increment && Start >= End || !Increment && Start <= End)
        return false;
    return Increment && LessCondition && !ReversedCond ||
      Increment && !LessCondition && ReversedCond ||
      !Increment && !LessCondition && !ReversedCond ||
      !Increment && LessCondition && ReversedCond;
  }

  /// Finds a last instruction in a specified block which writes memory
  /// from a specified node.
  Instruction* findLastWrite(AliasEstimateNode *ANI, BasicBlock *BB) {
    for (auto I = BB->rbegin(), E = BB->rend(); I != E; ++I) {
      bool RequiredInstruction = false;
      for_each_memory(*I, *mTLI,
        [this, &ANI, &RequiredInstruction] (Instruction &I,
            MemoryLocation &&Loc, unsigned Idx, AccessInfo, AccessInfo W) {
          auto EM = mAliasTree->find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto AN = EM->getAliasNode(*mAliasTree);
          assert(AN && "Alias node must not be null!");
          RequiredInstruction |=
            !mSTR.isUnreachable(ANI, AN) && W != AccessInfo::No;
        },
        [this, &ANI, &RequiredInstruction] (Instruction &I, AccessInfo,
            AccessInfo W) {
          auto AN = mAliasTree->findUnknown(I);
          assert(AN && "Unknown memory must not be null!");
          RequiredInstruction |=
            !mSTR.isUnreachable(ANI, AN) && W != AccessInfo::No;
        }
      );
      if (RequiredInstruction)
        return &(*I);
    }
    return nullptr;
  }

  /// Returns the last comparison in a specified basic block.
  Instruction* findLastCmp(BasicBlock *BB) {
    Instruction *CondInstruction = nullptr;
    auto I = BB->rbegin();
    auto InstrEnd = BB->rend();
    while ((I != InstrEnd) && (!(CondInstruction))) {
      if (isa<CmpInst>(*I))
        CondInstruction = &(*I);
      ++I;
    }
    return CondInstruction;
  }

  /// Checks that possible call from here does not write to memory.
  bool onlyReadsMemory(Instruction &Inst) {
    ImmutableCallSite CS(&Inst);
    return CS && mAliasTree->getAliasAnalysis().onlyReadsMemory(CS);
  }

  /// \brief Checks whether operands (except inductive variable) of a specified
  /// instruction are invariant for a specified loop.
  ///
  /// \return
  /// - `true` on success,
  /// - number of reads from induction variable memory (on success),
  /// - number of writes to induction variable memory (on success),
  /// - operand at the beginning of def-use chain that does not access
  /// induction variable or `nullptr`,
  /// - instruction accesses this operand or `nullptr`.
  std::tuple<bool, unsigned, unsigned, Value *, Value *> isLoopInvariantMemory(
      DFLoop &DFL, DefUseSet &DUS, EstimateMemory &EMI, Instruction &Inst) {
    bool Result = true;
    unsigned InductUseNum = 0, InductDefNum = 0;
    SmallSet<unsigned, 2> InductIdx;
    for_each_memory(Inst, *mTLI,
      [this, &EMI, &DUS, &Result, &InductUseNum, &InductDefNum, &InductIdx] (
          Instruction &, MemoryLocation &&Loc, unsigned Idx,
          AccessInfo R, AccessInfo W) {
        if (!Result)
          return;
        auto EM = mAliasTree->find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        if (!mSTR.isUnreachable(
             EM->getAliasNode(*mAliasTree), EMI.getAliasNode(*mAliasTree))) {
          if (R != AccessInfo::No || W != AccessInfo::No)
            InductIdx.insert(Idx);
          if (R != AccessInfo::No)
            ++InductUseNum;
          if (W != AccessInfo::No)
            ++InductDefNum;
          return;
        }
        Result &= !(DUS.hasDef(Loc) || DUS.hasMayDef(Loc));
      },
      [this, &DFL, &DUS, &Result](Instruction &I, AccessInfo, AccessInfo) {
        if (!Result)
          return;
        if (!onlyReadsMemory(I)) {
          Result = false;
          return;
        }
        auto AN = mAliasTree->findUnknown(I);
        for (auto &Loc : DUS.getExplicitAccesses()) {
          if (!DUS.hasDef(Loc) && !DUS.hasMayDef(Loc))
            continue;
          auto EM = mAliasTree->find(Loc);
          assert(EM && "Memory location must be presented in alias tree!");
          if (!mSTR.isUnreachable(EM->getAliasNode(*mAliasTree), AN)) {
            Result = false;
            return;
          }
        }
        for (auto *Loc : DUS.getExplicitUnknowns()) {
          if (onlyReadsMemory(*Loc))
            continue;
          auto UN = mAliasTree->findUnknown(*Loc);
          assert(UN &&
            "Unknown memory location must be presented in alias tree!");
          if (!mSTR.isUnreachable(UN, AN)) {
            Result = false;
            return;
          }
        }
      });
    std::tuple<bool, unsigned, unsigned, Value *, Value *> Tuple(
      Result, InductUseNum, InductDefNum, nullptr, nullptr);
    if (!std::get<0>(Tuple))
      return Tuple;
    bool OpNotAccessInduct = true;
    for (auto &Op : Inst.operands())
      if (auto I = dyn_cast<Instruction>(Op)) {
        auto T = isLoopInvariantMemory(DFL, DUS, EMI, *I);
        std::get<0>(Tuple) &= std::get<0>(T);
        if (!std::get<0>(Tuple))
          return Tuple;
        std::get<1>(Tuple) += std::get<1>(T);
        std::get<2>(Tuple) += std::get<2>(T);
        OpNotAccessInduct &= (std::get<1>(T) == 0 && std::get<2>(T) == 0);
        if (std::get<3>(T) != Op && std::get<3>(T)) {
          std::get<3>(Tuple) = std::get<3>(T);
          std::get<4>(Tuple) = std::get<4>(T);
        }
        if (std::get<3>(T) == Op && !InductIdx.count(Op.getOperandNo())) {
          std::get<3>(Tuple) = std::get<3>(T);
          std::get<4>(Tuple) = &Inst;
        }
      } else if (isa<ConstantData>(Op)) {
        std::get<3>(Tuple) = Op;
        std::get<4>(Tuple) = &Inst;
      } else {
        std::get<0>(Tuple) = false;
        return Tuple;
      }
   if (OpNotAccessInduct && InductUseNum == 0 && InductDefNum == 0)
     std::get<3>(Tuple) = &Inst;
    return Tuple;
  }

  /// \brief Checks that a specified loop is represented in canonical form.
  ///
  /// \post The`LInfo` parameter will be updated. Start, end, step will be set
  /// is possible, and loop will be marked as canonical on success.
  void checkLoop(tsar::DFLoop* Region, VarDecl *Var, CanonicalLoopInfo *LInfo) {
    auto MemMatch = mMemoryMatcher->find<AST>(Var);
    if (MemMatch == mMemoryMatcher->end())
      return;
    auto AI= MemMatch->get<IR>();
    if (!AI || !AI->getType() || !AI->getType()->isPointerTy())
      return;
    LInfo->setInduction(AI);
    LLVM_DEBUG(
      dbgs() << "[CANONICAL LOOP]: induction variable is";
      TSAR_LLVM_DUMP(AI->dump()));
    llvm::MemoryLocation MemLoc(AI, 1);
    auto EMI = mAliasTree->find(MemLoc);
    assert(EMI && "Estimate memory location must not be null!");
    auto ANI = EMI->getAliasNode(*mAliasTree);
    assert(ANI && "Alias node must not be null!");
    auto *L = Region->getLoop();
    assert(L && "Loop must not be null!");
    Instruction *Init = findLastWrite(ANI, L->getLoopPreheader());
    assert(Init && "Init instruction should not be nullptr!");
    Instruction *Condition = findLastCmp(L->getHeader());
    assert(Condition && "Condition instruction should not be nullptr!");
    auto *LatchBB = L->getLoopLatch();
    Instruction *Increment = nullptr;
    for (auto *BB: L->blocks()) {
      auto DFN = mRgnInfo->getRegionFor(BB);
      assert(DFN && "DFNode must not be null!");
      auto Match = mDefInfo->find(DFN);
      assert(Match != mDefInfo->end() && Match->get<ReachSet>() &&
          Match->get<DefUseSet>() && "Data-flow value must be specified!");
      auto &DUS = Match->get<DefUseSet>();
      if (!DUS->hasDef(MemLoc) && !DUS->hasMayDef(MemLoc))
        continue;
      auto *DFB = llvm::dyn_cast<DFBlock>(DFN);
      if (!DFB)
        continue;
      // Let us check that there is only a single node which contains definition
      // of induction variable. This node must be a single latch. In case of
      // multiple latches `LatchBB` will be `nullptr`.
      if (LatchBB != DFB->getBlock())
        return;
      int NumOfWrites = 0;
      for_each_memory(*DFB->getBlock(), *mTLI,
        [this, ANI, &NumOfWrites, &Increment] (Instruction &I,
            MemoryLocation &&Loc, unsigned Idx, AccessInfo, AccessInfo W) {
          auto EM = mAliasTree->find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto AN = EM->getAliasNode(*mAliasTree);
          assert(AN && "Alias node must not be null!");
          if (!mSTR.isUnreachable(ANI, AN) && W != AccessInfo::No) {
            ++NumOfWrites;
            Increment = &I;
          }
        },
        [this, &ANI, &NumOfWrites] (Instruction &I, AccessInfo,
            AccessInfo W) {
          auto AN = mAliasTree->findUnknown(I);
          assert(AN && "Alias node must not be null!");
          if (!mSTR.isUnreachable(ANI, AN) && W != AccessInfo::No)
            ++NumOfWrites;
        }
      );
      if (!Increment && NumOfWrites != 1)
        return;
    }
    auto LoopDefItr = mDefInfo->find(Region);
    assert(LoopDefItr != mDefInfo->end() && LoopDefItr->get<DefUseSet>() &&
     "Data-flow value must be specified!");
    auto &LoopDUS = LoopDefItr->get<DefUseSet>();
    bool Result;
    unsigned InductUseNum, InductDefNum;
    Value *Expr;
    Value *Inst;
    std::tie(Result, InductUseNum, InductDefNum, Expr, Inst) =
      isLoopInvariantMemory(*Region, *LoopDUS, *EMI, *Init);
    if (!Result || InductDefNum != 1 || InductUseNum > 0)
      return;
    LInfo->setStart(Expr);
    LLVM_DEBUG(
      if (Expr) {
        dbgs() << "[CANONICAL LOOP]: lower bound of induction variable is ";
        TSAR_LLVM_DUMP(Expr->dump());
      });
    std::tie(Result, InductUseNum, InductDefNum, Expr, Inst) =
      isLoopInvariantMemory(*Region, *LoopDUS, *EMI, *Increment);
    if (!Result || InductDefNum != 1 || InductUseNum > 1)
      return;
    assert((!Expr || Inst && isa<llvm::BinaryOperator>(Inst) &&
      (cast<llvm::BinaryOperator>(Inst)->getOpcode() ==
        llvm::BinaryOperator::Add ||
      cast<llvm::BinaryOperator>(Inst)->getOpcode() ==
        llvm::BinaryOperator::Sub)) && "Unknown step value!");
    if (Expr && isa<llvm::BinaryOperator>(Inst))
      if (cast<llvm::BinaryOperator>(Inst)->getOpcode() ==
          llvm::BinaryOperator::Sub)
        LInfo->setStep(mSE->getNegativeSCEV(mSE->getSCEV(Expr)));
      else
        LInfo->setStep(mSE->getSCEV(Expr));
    LLVM_DEBUG(
      if (LInfo->getStep()) {
        dbgs() << "[CANONICAL LOOP]: step of induction variable is ";
        TSAR_LLVM_DUMP(LInfo->getStep()->dump());
      });
    std::tie(Result, InductUseNum, InductDefNum, Expr, Inst) =
      isLoopInvariantMemory(*Region, *LoopDUS, *EMI, *Condition);
    if (!Result || InductDefNum != 0 || InductUseNum > 1)
      return;
    LInfo->setEnd(Expr);
    LLVM_DEBUG(
      if (Expr) {
        dbgs() << "[CANONICAL LOOP]: upper bound of induction variable is ";
        TSAR_LLVM_DUMP(Expr->dump());
      });
    LInfo->markAsCanonical();
  }

  DFRegionInfo *mRgnInfo;
  const LoopMatcherPass::LoopMatcher *mLoopInfo;
  DefinedMemoryInfo *mDefInfo;
  const MemoryMatchInfo::MemoryMatcher *mMemoryMatcher;
  tsar::AliasTree *mAliasTree;
  TargetLibraryInfo *mTLI;
  ScalarEvolution *mSE;
  CanonicalLoopSet *mCanonicalLoopInfo;
  SpanningTreeRelation<const AliasTree *> mSTR;
};

/// Returns LoopMatcher that matches loops that can be canonical.
DeclarationMatcher makeLoopMatcher() {
  return functionDecl(forEachDescendant(
      forStmt(
        hasLoopInit(eachOf(
          declStmt(hasSingleDecl(
            varDecl(hasType(isInteger()))
            .bind("InitVarName")))
          .bind("LoopInitDecl"),
          binaryOperator(
            hasOperatorName("="),
            hasLHS(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("InitVarName")))))
          .bind("LoopInitAssignment"))),
        hasIncrement(eachOf(
          unaryOperator(
            eachOf(
              hasOperatorName("++"),
              hasOperatorName("--")),
            hasUnaryOperand(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("UnIncVarName"))))).bind("UnaryIncr"),
          binaryOperator(
            eachOf(
              hasOperatorName("+="),
              hasOperatorName("-=")),
            hasLHS(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("BinIncVarName"))))).bind("BinaryIncr"),
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
                      .bind("SecondAssignmentVarName")))))))).bind("BinaryIncr"),
              binaryOperator(
                hasOperatorName("-"),
                hasLHS(implicitCastExpr(
                  hasImplicitDestinationType(isInteger()),
                  hasSourceExpression(declRefExpr(to(
                    varDecl(hasType(isInteger()))
                    .bind("FirstAssignmentVarName"))))))).bind("BinaryIncr"))))
            .bind("LoopIncrAssignment"))),
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
                .bind("SecondConditionVarName")))))))).bind("LoopCondition")))
      .bind("forLoop")));
}
}

bool CanonicalLoopPass::runOnFunction(Function &F) {
  releaseMemory();
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
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  DeclarationMatcher LoopMatcher = makeLoopMatcher();
  CanonicalLoopLabeler Labeler(RgnInfo, LoopInfo, DefInfo, MemInfo, ATree,
      TLI, SE, &mCanonicalLoopInfo);
  auto &Context = FuncDecl->getASTContext();
  auto Nodes = match<DeclarationMatcher, Decl>(LoopMatcher, *FuncDecl, Context);
  while (!Nodes.empty()) {
    MatchFinder::MatchResult Result(Nodes.back(), &Context);
    Labeler.run(Result);
    Nodes.pop_back();
  }
  return false;
}

void CanonicalLoopPass::print(raw_ostream &OS, const llvm::Module *M) const {
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  for (auto *Info : mCanonicalLoopInfo) {
    auto *DFL = Info->getLoop();
    OS << "loop at ";
    tsar::print(OS, DFL->getLoop()->getStartLoc(),
                GlobalOpts.PrintFilenameOnly);
    OS << " is " << (Info->isCanonical() ? "semantically" : "syntactically")
       << " canonical\n";
  }
}

void CanonicalLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createCanonicalLoopPass() {
  return new CanonicalLoopPass();
}
