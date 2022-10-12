//=== CanonicalLoop.cpp - High Level Canonical Loop Analyzer ----*- C++ -*-===//
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
// This file defines classes to identify canonical for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/Utils.h"
#include "tsar/Support/Tags.h"
#include "tsar/Unparse/Utils.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
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
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
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
  CanonicalLoopLabeler(FunctionPass &P, Function &F, CanonicalLoopSet &CLI)
    : mCanonicalLoopInfo(&CLI) {
      mRgnInfo = &P.getAnalysis<DFRegionInfoPass>().getRegionInfo();
      mLoopInfo = &P.getAnalysis<LoopMatcherPass>().getMatcher();
      mMemoryMatcher = &P.getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher;
      mAliasTree = &P.getAnalysis<EstimateMemoryPass>().getAliasTree();
      mTLI = &P.getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
      mSE = &P.getAnalysis<ScalarEvolutionWrapperPass>().getSE();
      mDT = &P.getAnalysis<DominatorTreeWrapperPass>().getDomTree();
      auto &DIEMPass = P.getAnalysis<DIEstimateMemoryPass>();
      if (DIEMPass.isConstructed())
        mDIMInfo = DIMemoryClientServerInfo(DIEMPass.getAliasTree(), P, F);
  }

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
    LLVM_DEBUG(
        dbgs() << "[CANONICAL LOOP]: process loop at ";
        For->getBeginLoc().print(dbgs(), Result.Context->getSourceManager());
        dbgs() << "\n");
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
    if (auto Start{Init->getIntegerConstantExpr(*Ctx)})
      if (auto End{ReversedCond
                       ? Condition->getLHS()->getIntegerConstantExpr(*Ctx)
                       : Condition->getRHS()->getIntegerConstantExpr(*Ctx)})
        if (Increment && *Start >= *End || !Increment && *Start <= *End)
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
    Optional<APSInt> Step;
    // If step is not constant we can not prove anything.
    if (!(Step = Incr->getRHS()->getIntegerConstantExpr(*Ctx)))
      if (!(Step = Incr->getLHS()->getIntegerConstantExpr(*Ctx)))
        return true;
    if (Step->isNonNegative() && !Step->isStrictlyPositive())
      return false;
    bool Increment =
      Step->isStrictlyPositive() &&
        (Incr->getOpcode() == BO_Add || Incr->getOpcode() == BO_AddAssign) ||
      Step->isNegative() &&
        (Incr->getOpcode() == BO_Sub || Incr->getOpcode() == BO_SubAssign);
    bool LessCondition = Condition->getOpcode() == BO_LT ||
      Condition->getOpcode() == BO_LE;
    if (auto Start{Init->getIntegerConstantExpr(*Ctx)})
      if (auto End{ReversedCond
                       ? Condition->getLHS()->getIntegerConstantExpr(*Ctx)
                       : Condition->getRHS()->getIntegerConstantExpr(*Ctx)})
        if (Increment && *Start >= *End || !Increment && *Start <= *End)
          return false;
    return Increment && LessCondition && !ReversedCond ||
      Increment && !LessCondition && ReversedCond ||
      !Increment && !LessCondition && !ReversedCond ||
      !Increment && LessCondition && ReversedCond;
  }

  /// Finds a last instruction in a specified block which writes memory
  /// from a specified node.
  Instruction* findLastWrite(DIMemory &DIMI, BasicBlock *BB,
      const SpanningTreeRelation<const DIAliasTree *> &STR) {
    assert(mDIMInfo && mDIMInfo->isValid() &&
      "Client-to-server mapping must be available!");
    auto *DINI = DIMI.getAliasNode();
    for (auto I = BB->rbegin(), E = BB->rend(); I != E; ++I) {
      bool RequiredInstruction = false;
      for_each_memory(*I, *mTLI,
        [this, &STR, DINI, &RequiredInstruction] (Instruction &I,
            MemoryLocation &&Loc, unsigned Idx, AccessInfo, AccessInfo W) {
          if (W == AccessInfo::No)
            return;
          auto EM = mAliasTree->find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto &DL = I.getModule()->getDataLayout();
          auto *DIM = mDIMInfo->findFromClient(
            *EM->getTopLevelParent(), DL, *mDT).get<Clone>();
          RequiredInstruction |=
              (!DIM || !STR.isUnreachable(DINI, DIM->getAliasNode()));
        },
        [this, &STR, &DINI, &RequiredInstruction] (Instruction &I,
            AccessInfo, AccessInfo W) {
          if (W == AccessInfo::No)
            return;
          /// TODO (kaniandr@gmail.com): use results from server to check
          /// that function call doesn't write any memory.
          if (!(RequiredInstruction = (isa<CallBase>(I) && onlyReadsMemory(I))))
            return;
          auto DIM = mDIMInfo->findFromClient(
            I, *mDT, DIUnknownMemory::NoFlags).get<Clone>();
          RequiredInstruction |=
              (!DIM || !STR.isUnreachable(DINI, DIM->getAliasNode()));
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
    auto *Call = dyn_cast<CallBase>(&Inst);
    return Call && mAliasTree->getAliasAnalysis().onlyReadsMemory(Call);
  }

  /// \brief Checks whether operands (except inductive variable) of a specified
  /// user are invariant for a specified loop.
  ///
  /// \return
  /// - `true` on success,
  /// - number of reads from induction variable memory (on success),
  /// - number of writes to induction variable memory (on success),
  /// - operand at the beginning of def-use chain that does not access
  /// induction variable or `nullptr`,
  /// - instruction accesses this operand or `nullptr`.
  std::tuple<bool, unsigned, unsigned, Value *, Value *> isLoopInvariantMemory(
      const SpanningTreeRelation<const DIAliasTree *> &STR,
      const DIDependenceSet &DIDepSet, DIMemory &DIMI, User &U) {
    assert(mDIMInfo && mDIMInfo->isValid() &&
      "Client-to-server mapping must be available!");
    bool Result = true;
    unsigned InductUseNum = 0, InductDefNum = 0;
    SmallSet<unsigned, 2> InductIdx;
    if (auto Inst = dyn_cast<Instruction>(&U)) {
      auto &DL = Inst->getModule()->getDataLayout();
      auto *DINI = DIMI.getAliasNode();
      for_each_memory(*Inst, *mTLI,
        [this, &STR, Inst, DL, DINI, &DIDepSet,
        &Result, &InductUseNum, &InductDefNum, &InductIdx](
          Instruction &, MemoryLocation &&Loc, unsigned Idx,
          AccessInfo R, AccessInfo W) {
            if (!Result)
              return;
            auto EM = mAliasTree->find(Loc);
            assert(EM && "Estimate memory location must not be null!");
            auto *DIM = mDIMInfo->findFromClient(
              *EM->getTopLevelParent(), DL, *mDT).get<Clone>();
            /// TODO (kaniandr@gmail.com): investigate cases when DIM is nullptr.
            if (!(Result = DIM))
              return;
            if (!STR.isUnreachable(DIM->getAliasNode(), DINI)) {
              if (R != AccessInfo::No || W != AccessInfo::No)
                InductIdx.insert(Idx);
              if (R != AccessInfo::No)
                ++InductUseNum;
              if (W != AccessInfo::No)
                ++InductDefNum;
              return;
            }
            auto TraitItr = DIDepSet.find_as(DIM->getAliasNode());
            Result &= (TraitItr == DIDepSet.end() ||
                       TraitItr->is_any<trait::Readonly, trait::NoAccess>());
        },
        [this, &DIDepSet, &Result](Instruction &I, AccessInfo, AccessInfo) {
          if (!Result)
            return;
          /// TODO (kaniandr@gmail.com): use results from server to check
          /// that function call doesn't write any memory.
          if (!(Result = (isa<CallBase>(I) && onlyReadsMemory(I))))
            return;
          auto *DIM = mDIMInfo->findFromClient(
            I, *mDT, DIUnknownMemory::NoFlags).get<Clone>();
          assert(DIM && "Metadata-level memory must be available!");
          auto TraitItr = DIDepSet.find_as(DIM->getAliasNode());
          Result &= (TraitItr == DIDepSet.end() ||
                     TraitItr->is_any<trait::Readonly, trait::NoAccess>());
        });
    }
    std::tuple<bool, unsigned, unsigned, Value *, Value *> Tuple(
      Result, InductUseNum, InductDefNum, nullptr, nullptr);
    if (!std::get<0>(Tuple))
      return Tuple;
    bool OpNotAccessInduct = true;
    for (auto &Op : U.operands())
      if (isa<Instruction>(Op) || isa<llvm::ConstantExpr>(Op)) {
        auto T = isLoopInvariantMemory(STR, DIDepSet, DIMI, cast<User>(*Op));
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
          std::get<4>(Tuple) = &U;
        }
      } else if (isa<ConstantData>(Op) || isa<GlobalValue>(Op)) {
        if (!InductIdx.count(Op.getOperandNo())) {
          std::get<3>(Tuple) = Op;
          std::get<4>(Tuple) = &U;
        }
      } else {
        std::get<0>(Tuple) = false;
        return Tuple;
      }
   if (OpNotAccessInduct && InductUseNum == 0 && InductDefNum == 0)
     std::get<3>(Tuple) = &U;
    return Tuple;
  }

  /// \brief Checks that a specified loop is represented in canonical form.
  ///
  /// \post The`LInfo` parameter will be updated. Start, end, step will be set
  /// is possible, and loop will be marked as canonical on success.
  void checkLoop(tsar::DFLoop* Region, VarDecl *Var, CanonicalLoopInfo *LInfo) {
    if (!mDIMInfo || !mDIMInfo->isValid())
      return;
    auto MemMatch = mMemoryMatcher->find<AST>(Var);
    if (MemMatch == mMemoryMatcher->end())
      return;
    auto AI = MemMatch->get<IR>().front();
    if (!AI || !AI->getType() || !AI->getType()->isPointerTy())
      return;
    LInfo->setInduction(AI);
    LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: induction variable is";
               AI->print(dbgs()); dbgs() << "\n");
    llvm::MemoryLocation MemLoc(AI, 1);
    auto EMI = mAliasTree->find(MemLoc);
    assert(EMI && "Estimate memory location must not be null!");
    auto *L = Region->getLoop();
    assert(L && "Loop must not be null!");
    auto &DL = L->getHeader()->getModule()->getDataLayout();
    auto *DIMI = mDIMInfo->findFromClient(*EMI->getTopLevelParent(), DL, *mDT)
                     .get<Clone>();
    if (!DIMI) {
      LLVM_DEBUG(dbgs() << "[CANONICAL_LOOP]: metadata-level is not available "
                           "for induction\n");
      return;
    }
    SpanningTreeRelation<const DIAliasTree *> STR(mDIMInfo->DIAT);
    Instruction *Init = findLastWrite(*DIMI, L->getLoopPreheader(), STR);
    assert(Init && "Init instruction should not be nullptr!");
    Instruction *Condition = findLastCmp(L->getHeader());
    assert(Condition && "Condition instruction should not be nullptr!");
    if (auto CmpI = dyn_cast<CmpInst>(Condition)) {
      if (CmpI->isSigned())
        LInfo->markAsSigned();
      else if (CmpI->isUnsigned())
        LInfo->markAsUnsigned();
      // TODO (kaniandr@gmail.com): use Clang to determine sign if possible.
      // Note, that equal comparison in LLVM has no sign.
    }
    auto *LatchBB = L->getLoopLatch();
    auto *DINI = DIMI->getAliasNode();
    Instruction *Increment = nullptr;
    for (auto *BB: L->blocks()) {
      int NumOfWrites = 0;
      for_each_memory(*BB, *mTLI,
        [this, &STR, &DL, DINI, &NumOfWrites, &Increment] (Instruction &I,
            MemoryLocation &&Loc, unsigned Idx, AccessInfo, AccessInfo W) {
          if (W == AccessInfo::No)
            return;
          auto EM = mAliasTree->find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto *DIM = mDIMInfo->findFromClient(
            *EM->getTopLevelParent(), DL, *mDT).get<Clone>();
          /// TODO (kaniandr@gmail.com): investigate cases when DIM is nullptr.
          if (!DIM) {
            ++NumOfWrites;
          } else if (!STR.isUnreachable(DINI, DIM->getAliasNode())) {
            ++NumOfWrites;
            Increment = &I;
          }
        },
        [this, &STR, DINI, &NumOfWrites] (Instruction &I, AccessInfo, AccessInfo W) {
          if (W == AccessInfo::No)
            return;
          auto *DIM = mDIMInfo->findFromClient(
            I, *mDT, DIUnknownMemory::NoFlags).get<Clone>();
          /// TODO (kaniandr@gmail.com): investigate cases when DIM is nullptr.
          if (!DIM ||!STR.isUnreachable(DINI, DIM->getAliasNode()))
            ++NumOfWrites;
        }
      );
      // Let us check that there is only a single node which contains definition
      // of induction variable. This node must be a single latch. In case of
      // multiple latches `LatchBB` will be `nullptr`.
      if (NumOfWrites == 0)
        continue;
      if (NumOfWrites > 0 && LatchBB != BB || !Increment || NumOfWrites != 1)
        return;
    }
    auto *DIDepSet = mDIMInfo->findFromClient(*L);
    if (!DIDepSet) {
      LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: loop is not analyzed\n");
      return;
    }
    bool Result;
    unsigned InductUseNum, InductDefNum;
    Value *Expr;
    Value *Inst;
    std::tie(Result, InductUseNum, InductDefNum, Expr, Inst) =
      isLoopInvariantMemory(STR, *DIDepSet, *DIMI, *Init);
    if (!Result || InductDefNum != 1 || InductUseNum > 0)
      return;
    LInfo->setStart(Expr);
    LLVM_DEBUG(
      if (Expr) {
        dbgs() << "[CANONICAL LOOP]: lower bound of induction variable is ";
        Expr->print(dbgs()); dbgs() << "\n";
      });
    std::tie(Result, InductUseNum, InductDefNum, Expr, Inst) =
      isLoopInvariantMemory(STR, *DIDepSet, *DIMI, *Increment);
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
        LInfo->getStep()->print(dbgs()); dbgs() << "\n";
      });
    std::tie(Result, InductUseNum, InductDefNum, Expr, Inst) =
      isLoopInvariantMemory(STR, *DIDepSet, *DIMI, *Condition);
    if (!Result || InductDefNum != 0 || InductUseNum > 1)
      return;
    LInfo->setEnd(Expr);
    if (Expr && Condition->getNumOperands() == 2)
      if (auto CmpI = dyn_cast<CmpInst>(Condition)) {
        if (CmpI->getOperand(0) == Expr)
          LInfo->setPredicate(CmpI->getInversePredicate());
        else
          LInfo->setPredicate(CmpI->getPredicate());
      }
    LLVM_DEBUG(
      if (Expr) {
        dbgs() << "[CANONICAL LOOP]: upper bound of induction variable is ";
        Expr->print(dbgs()); dbgs() << "\n";
      });
    LInfo->markAsCanonical();
    if (auto *Parent{L->getParentLoop()}) {
      auto *ParentDIDepSet = mDIMInfo->findFromClient(*Parent);
      if (!ParentDIDepSet) {
        LLVM_DEBUG(dbgs() << "[CANONICAL LOOP]: parent loop is not analyzed\n");
        return;
      }
      if (std::get<0>(
              isLoopInvariantMemory(STR, *ParentDIDepSet, *DIMI, *Init)) &&
          std::get<0>(
              isLoopInvariantMemory(STR, *ParentDIDepSet, *DIMI, *Increment)) &&
          std::get<0>(
              isLoopInvariantMemory(STR, *ParentDIDepSet, *DIMI, *Condition)))
        LInfo->markAsRectangular();
    }
  }

  CanonicalLoopSet *mCanonicalLoopInfo = nullptr;
  DFRegionInfo *mRgnInfo = nullptr;;
  const LoopMatcherPass::LoopMatcher *mLoopInfo = nullptr;
  const MemoryMatchInfo::MemoryMatcher *mMemoryMatcher = nullptr;
  AliasTree *mAliasTree = nullptr;
  TargetLibraryInfo *mTLI = nullptr;
  ScalarEvolution *mSE = nullptr;
  DominatorTree *mDT = nullptr;
  Optional<DIMemoryClientServerInfo> mDIMInfo;
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
              implicitCastExpr(
                hasImplicitDestinationType(isInteger()),
                hasSourceExpression(eachOf(
                  binaryOperator(
                    hasOperatorName("+"),
                    eachOf(
                      hasLHS(implicitCastExpr(
                        hasSourceExpression(implicitCastExpr(
                          hasSourceExpression(declRefExpr(to(
                            varDecl(hasType(isInteger()))
                            .bind("FirstAssignmentVarName")))))))),
                      hasRHS(implicitCastExpr(
                        hasSourceExpression(implicitCastExpr(
                          hasSourceExpression(declRefExpr(to(
                            varDecl(hasType(isInteger()))
                            .bind("SecondAssignmentVarName")))))))))).bind("BinaryIncr"),
                  binaryOperator(
                    hasOperatorName("-"),
                    hasLHS(implicitCastExpr(
                      hasSourceExpression(implicitCastExpr(
                        hasSourceExpression(declRefExpr(to(
                          varDecl(hasType(isInteger()))
                          .bind("FirstAssignmentVarName"))))))))).bind("BinaryIncr")))),
              binaryOperator(
                hasOperatorName("+"),
                eachOf(
                  hasLHS(implicitCastExpr(
                    hasImplicitDestinationType(isInteger()),
                      hasSourceExpression(eachOf(
                        declRefExpr(to(
                          varDecl(hasType(isInteger()))
                          .bind("FirstAssignmentVarName"))),
                        implicitCastExpr(
                          hasSourceExpression(declRefExpr(to(
                            varDecl(hasType(isInteger()))
                            .bind("FirstAssignmentVarName"))))))))),
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
  DeclarationMatcher LoopMatcher = makeLoopMatcher();
  CanonicalLoopLabeler Labeler(*this, F, mCanonicalLoopInfo);
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
  std::vector<const CanonicalLoopInfo *> Loops(mCanonicalLoopInfo.begin(),
                                               mCanonicalLoopInfo.end());
  std::sort(Loops.begin(), Loops.end(), [](const CanonicalLoopInfo *LHS,
    const CanonicalLoopInfo *RHS) {
      auto LHSLoc = LHS->getLoop()->getLoop()->getStartLoc();
      auto RHSLoc = RHS->getLoop()->getLoop()->getStartLoc();
      return LHSLoc && RHSLoc &&
        (LHSLoc.getLine() < RHSLoc.getLine() ||
          LHSLoc.getLine() == RHSLoc.getLine() &&
            LHSLoc.getCol() < RHSLoc.getCol());
    });
  for (auto Info : Loops) {
    auto *DFL = Info->getLoop();
    OS << "loop at ";
    tsar::print(OS, DFL->getLoop()->getStartLoc(),
                GlobalOpts.PrintFilenameOnly);
    OS << " is " << (Info->isCanonical() ? "semantically" : "syntactically")
       << " canonical";
    if (Info->isRectangular())
      OS << "and rectangular";
    OS << "\n";
  }
}

void CanonicalLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequiredTransitive<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createCanonicalLoopPass() {
  return new CanonicalLoopPass();
}
