#include "tsar/Transform/Clang/LoopReverse.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include "llvm/IR/LegacyPassManager.h"
#include <algorithm>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-loop-reverse"
const char *DEBUG_PREFIX = "[Loop Reverse]: ";

// server, providers, passgroupinfo
namespace {
// local provider
using ClangLoopReverseProvider =
    FunctionPassAAProvider<AnalysisSocketImmutableWrapper, LoopInfoWrapperPass,
                           ParallelLoopPass, CanonicalLoopPass, LoopMatcherPass,
                           DFRegionInfoPass, ClangDIMemoryMatcherPass,
                           ClangPerfectLoopPass>;
// server provider
using ClangLoopReverseServerProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass>;
// server response
using ClangLoopReverseServerResponse =
    AnalysisResponsePass<GlobalsAAWrapperPass, DIMemoryTraitPoolWrapper,
                         DIMemoryEnvironmentWrapper, GlobalDefinedMemoryWrapper,
                         GlobalLiveMemoryWrapper, ClonedDIMemoryMatcherWrapper,
                         ClangLoopReverseServerProvider>;
// server
class ClangLoopReverseServer final : public AnalysisServer {
public:
  static char ID;
  ClangLoopReverseServer();
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void prepareToClone(Module &ClientM,
                      ValueToValueMapTy &ClientToServer) override;
  void initializeServer(Module &CM, Module &SM, ValueToValueMapTy &CToS,
                        legacy::PassManager &PM) override;
  void addServerPasses(Module &M, legacy::PassManager &PM) override;
  void prepareToClose(legacy::PassManager &PM) override;
};
// pass info
class ClangLoopReverseInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
} // namespace
// server methods
namespace {
void ClangLoopReverseServer::getAnalysisUsage(AnalysisUsage &AU) const {
  AnalysisServer::getAnalysisUsage(AU);
  ClientToServerMemory::getAnalysisUsage(AU);
  AU.addRequired<GlobalOptionsImmutableWrapper>();
}

void ClangLoopReverseServer::prepareToClone(Module &ClientM,
                                            ValueToValueMapTy &ClientToServer) {
  ClientToServerMemory::prepareToClone(ClientM, ClientToServer);
}

void ClangLoopReverseServer::initializeServer(Module &CM, Module &SM,
                                              ValueToValueMapTy &CToS,
                                              legacy::PassManager &PM) {
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>();
  PM.add(createGlobalOptionsImmutableWrapper(&GO.getOptions()));
  PM.add(createGlobalDefinedMemoryStorage());
  PM.add(createGlobalLiveMemoryStorage());
  PM.add(createDIMemoryTraitPoolStorage());
  ClientToServerMemory::initializeServer(*this, CM, SM, CToS, PM);
}

void ClangLoopReverseServer::addServerPasses(Module &M,
                                             legacy::PassManager &PM) {
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  addImmutableAliasAnalysis(PM);
  addBeforeTfmAnalysis(PM);
  addAfterSROAAnalysis(GO, M.getDataLayout(), PM);
  addAfterLoopRotateAnalysis(PM);
  PM.add(createVerifierPass());
  PM.add(new ClangLoopReverseServerResponse);
}

void ClangLoopReverseServer::prepareToClose(legacy::PassManager &PM) {
  ClientToServerMemory::prepareToClose(PM);
}
} // namespace
// passgroupinfo methods
namespace {
void ClangLoopReverseInfo::addBeforePass(legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(new ClangLoopReverseServer);
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
}

void ClangLoopReverseInfo::addAfterPass(legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}
} // namespace

void ClangLoopReverse::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangLoopReverseProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<ClangRegionCollector>();
  AU.setPreservesAll();
}
// pass code
namespace {
class ClauseVisitor : public clang::RecursiveASTVisitor<ClauseVisitor> {
public:
  ClauseVisitor() : mSL(NULL), mIL(NULL) {}
  bool VisitStringLiteral(clang::StringLiteral *SL) {
    if (!SL)
      return true;
    if (SL->getString() != "reverse")
      mSL = SL;
    return true;
  }
  bool VisitIntegerLiteral(clang::IntegerLiteral *IL) {
    if (!IL)
      return true;
    mIL = IL;
    return true;
  }
  clang::StringLiteral *getSL() { return mSL; }
  clang::IntegerLiteral *getIL() { return mIL; }
  void clear() {
    mSL = NULL;
    mIL = NULL;
  }

private:
  clang::StringLiteral *mSL;
  clang::IntegerLiteral *mIL;
};
class MiniVisitor : public clang::RecursiveASTVisitor<MiniVisitor> {
public:
  MiniVisitor() : mDRE(NULL) {}
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
    if (!DRE)
      return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
    mDRE = DRE;
  }
  clang::DeclRefExpr *getDRE() { return mDRE; }

private:
  clang::DeclRefExpr *mDRE;
};
class ClangLoopReverseVisitor
    : public clang::RecursiveASTVisitor<ClangLoopReverseVisitor> {
public:
  ClangLoopReverseVisitor(TransformationContext *TfmCtx, ClangLoopReverse &Pass,
                          llvm::Module &M,
                          const tsar::GlobalOptions *GlobalOpts,
                          tsar::MemoryMatchInfo::MemoryMatcher *MemMatcher,
                          AnalysisSocket &Socket)
      : mTfmCtx(TfmCtx), mRewriter(TfmCtx->getRewriter()),
        mContext(TfmCtx->getContext()), mSrcMgr(mRewriter.getSourceMgr()),
        mLangOpts(mRewriter.getLangOpts()), mGO(GlobalOpts), mPass(Pass),
        mModule(M), mMemMatcher(MemMatcher), mSocket(Socket),
        mClientToServer(
            **mSocket.getAnalysis<AnalysisClientServerMatcherWrapper>()
                  ->value<AnalysisClientServerMatcherWrapper *>()),
        mStatus(SEARCH_PRAGMA), mIsStrict(false), mDIAT(NULL), mDIDepInfo(NULL),
        mPerfectLoopInfo(NULL), mCurrentLoops(NULL) {}
  const CanonicalLoopInfo *getLoopInfo(clang::ForStmt *FS) {
    if (!FS)
      return NULL;
    for (auto Info : *mCurrentLoops) {
      if (Info->getASTLoop() == FS) {
        return Info;
      }
    }
    return NULL;
  }
  bool TraverseForStmt(clang::ForStmt *FS) {
    if (!FS)
      return false;
    if (mStatus == TRAVERSE_LOOPS) {
      if (auto *LI = getLoopInfo(FS)) {
        auto *Ind = mMemMatcher->find<IR>(LI->getInduction())->get<AST>();
        if (mPerfectLoopInfo->count(LI->getLoop())) {
          mLoops.push_back(
              std::pair<const tsar::CanonicalLoopInfo *, clang::VarDecl *>(
                  LI, Ind));
          // traverse further
          return RecursiveASTVisitor::TraverseForStmt(FS);
        } else {
          mLoops.push_back(
              std::pair<const tsar::CanonicalLoopInfo *, clang::VarDecl *>(
                  LI, Ind));
          // no traverse further
          return true;
        }
      } else {
        // no traverse further
        return true;
      }
    } else {
      return RecursiveASTVisitor::VisitForStmt(FS);
    }
  }
  bool TraverseDecl(clang::Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             clang::diag::err_reverse_not_forstmt);
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }
  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case Status::SEARCH_PRAGMA: {
      Pragma P(*S);
      llvm::SmallVector<clang::Stmt *, 1> Clauses;
      if (findClause(P, ClauseId::LoopReverse, Clauses)) {
        LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Found reverse clause\n");
        // collect info from clauses
        ClauseVisitor CV;
        for (auto *C : Clauses) {
          CV.TraverseStmt(C);
          if (auto *SL = CV.getSL()) {
            mStringLiterals.push_back(SL);
          } else if (auto *IL = CV.getIL()) {
            mIntegerLiterals.push_back(IL);
          }
          CV.clear();
        }

        // check for nostrict clause
        mIsStrict = true;
        auto Csize = Clauses.size();
        findClause(P, ClauseId::NoStrict, Clauses);
        if (Csize != Clauses.size()) {
          mIsStrict = false;
          LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Found nostrict clause\n");
        }
        // remove clauses
        llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
        auto IsPossible =
            pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts, ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   clang::diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   clang::diag::warn_remove_directive_in_include);
          else
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   clang::diag::warn_remove_directive);
        clang::Rewriter::RewriteOptions RemoveEmptyLine;
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty
        /// is set to true then removing (in RewriterBuffer) works
        /// incorrect.
        RemoveEmptyLine.RemoveLineIfEmpty = false;
        for (auto SR : ToRemove)
          mRewriter.RemoveText(SR, RemoveEmptyLine);
        Clauses.clear();
        mStatus = Status::TRAVERSE_STMT;
        return true;
      } else {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
    }

    case Status::TRAVERSE_STMT: {
      if (!isa<clang::ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(),
               clang::diag::err_reverse_not_forstmt);
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      if (mNewAnalisysRequired) {
        Function *F = mModule.getFunction(mCurrentFD->getNameAsString());
        mProvider = &mPass.getAnalysis<ClangLoopReverseProvider>(*F);
        mCurrentLoops =
            &mProvider->get<CanonicalLoopPass>().getCanonicalLoopInfo();
        mPerfectLoopInfo =
            &mProvider->get<ClangPerfectLoopPass>().getPerfectLoopInfo();
        auto RF =
            mSocket.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(
                *F);
        assert(RF &&
               "Dependence analysis must be available for a loop reverse!");
        mDIAT = &RF->value<DIEstimateMemoryPass *>()->getAliasTree();
        mDIDepInfo =
            &RF->value<DIDependencyAnalysisPass *>()->getDependencies();
        mNewAnalisysRequired = false;
      }

      mStatus = TRAVERSE_LOOPS;
      auto Ret = RecursiveASTVisitor::TraverseStmt(S);

      llvm::SmallSet<int, 1> mToTransform;

      for (int i = 0; i < mStringLiterals.size(); i++) {
        bool Matched = false;
        for (int j = 0; j < mLoops.size(); j++) {
          if (mStringLiterals[i]->getString() == mLoops[j].second->getName()) {
            mToTransform.insert(j);
            Matched = true;
            break;
          }
        }
        if (!Matched)
          toDiag(mSrcMgr.getDiagnostics(), mStringLiterals[i]->getLocStart(),
                 clang::diag::warn_reverse_cant_match);
      }

      for (int i = 0; i < mIntegerLiterals.size(); i++) {
        auto Num = mIntegerLiterals[i]->getValue().getZExtValue() - 1;
        if (Num < mLoops.size()) {
          mToTransform.insert(Num);
          LLVM_DEBUG(dbgs()
                     << DEBUG_PREFIX << "Match " << Num + 1 << " as "
                     << mLoops[Num].first->getInduction()->getName() << "\n");
        } else {
          toDiag(mSrcMgr.getDiagnostics(), mIntegerLiterals[i]->getLocStart(),
                 clang::diag::warn_reverse_cant_match);
        }
      }
      for (auto Num : mToTransform) {
        const CanonicalLoopInfo *LI = mLoops[Num].first;
        llvm::Optional<llvm::APSInt> StartOpt;
        llvm::Optional<llvm::APSInt> EndOpt;
        llvm::Optional<llvm::APSInt> StepOpt;
        if (mIsStrict) {
          bool Dependency = false;
          auto *Loop = LI->getLoop()->getLoop();
          auto *LoopID = Loop->getLoopID();
          auto ServerLoopID =
              cast<MDNode>(*mClientToServer.getMappedMD(LoopID));
          auto DepItr = mDIDepInfo->find(ServerLoopID);
          if (DepItr == mDIDepInfo->end()) {
            toDiag(mSrcMgr.getDiagnostics(),
                   mLoops[Num].first->getASTLoop()->getBeginLoc(),
                   clang::diag::err_reverse_no_analysis);
            continue;
          } else {
            auto &DIDepSet = DepItr->get<DIDependenceSet>();
            DenseSet<const DIAliasNode *> Coverage;
            accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
                                                mGO->IgnoreRedundantMemory);
            if (!Coverage.empty()) {
              for (auto &Trait : DIDepSet) {
                if (!Coverage.count(Trait.getNode()))
                  continue;
                if (Trait.is_any<trait::Output, trait::Anti, trait::Flow>()) {
                  Dependency = true;
                  break;
                }
                // no check that it is the Induction what we looking for
                // trait->getMemory() is empty, moreover it must be matched with
                // local value
                if (Trait.is<trait::Induction>())
                  for (auto T : Trait) {
                    if (auto *Opts = T->get<trait::Induction>()) {
                      StartOpt = Opts->getStart();
                      StepOpt = Opts->getStep();
                      EndOpt = Opts->getEnd();
                    }
                  }
              }
            }
            if (Dependency) {
              toDiag(mSrcMgr.getDiagnostics(), LI->getASTLoop()->getBeginLoc(),
                     clang::diag::err_reverse_dependency);
              continue;
            }
          }
        }
        if (transformLoop(mLoops[Num].first, StartOpt, StepOpt, EndOpt)) {
          LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Transformed "
                            << mLoops[Num].second->getName() << "\n");
        } else
          LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Not transformed "
                            << mLoops[Num].second->getName() << "\n");
      }
      resetVisitor();
      return Ret;
    }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }
  bool VisitFunctionDecl(clang::FunctionDecl *FD) {
    if (!FD)
      return RecursiveASTVisitor::VisitFunctionDecl(FD);
    mCurrentFD = FD;
    mNewAnalisysRequired = true;
    return RecursiveASTVisitor::VisitFunctionDecl(FD);
  }
  void resetVisitor() {
    mStatus = SEARCH_PRAGMA;
    mIsStrict = false;
    mStringLiterals.clear();
    mStringLiterals.clear();
    mLoops.clear();
  }

private:
  bool transformLoop(const tsar::CanonicalLoopInfo *LI,
                     llvm::Optional<llvm::APSInt> &Start,
                     llvm::Optional<llvm::APSInt> &Step,
                     llvm::Optional<llvm::APSInt> &End) {
    if (!LI)
      return false;
    auto *FS = LI->getASTLoop();
    if (LI->isCanonical()) {
      auto *InitExpr = FS->getInit();
      auto *CondExpr = FS->getCond();
      auto *IncrExpr = FS->getInc();
      auto *IRInd = LI->getInduction();
      auto *ASTInd = mMemMatcher->find<IR>(IRInd)->get<AST>();
      bool IndSign = true;
      clang::SourceLocation IncrLoc;
      const char *NewIncrOp;
      clang::SourceRange IncExprSR;
      // if i = b + i -> i = i - b
      bool IncrSwapReq = false;
      bool UnaryIncr = false;
      clang::SourceRange IncrRightExprSR;
      clang::SourceRange IncrLeftExprSR;
      int Offset;
      // Increment expression transform
      if (auto *BO = dyn_cast<clang::BinaryOperator>(IncrExpr)) {
        switch (BO->getOpcode()) {
        case clang::BinaryOperator::Opcode::BO_AddAssign: {
          IncExprSR = BO->getRHS()->getSourceRange();
          NewIncrOp = "-=";
          Offset = 1;
          IncrLoc = BO->getOperatorLoc();
          IndSign = true;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_SubAssign: {
          IncExprSR = BO->getRHS()->getSourceRange();
          NewIncrOp = "+=";
          Offset = 1;
          IncrLoc = BO->getOperatorLoc();
          IndSign = false;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_Assign: {
          if (auto *OP = dyn_cast<clang::BinaryOperator>(BO->getRHS())) {
            switch (OP->getOpcode()) {
            case clang::BinaryOperator::Opcode::BO_Add: {
              auto *Left = OP->getLHS();
              auto *Right = OP->getRHS();
              MiniVisitor MV;
              MV.TraverseStmt(Right);
              if (auto *DRE = MV.getDRE()) {
                if (DRE->getDecl() == ASTInd) {
                  IncrSwapReq = true;
                }
              }
              IncrRightExprSR = Right->getSourceRange();
              IncrLeftExprSR = Left->getSourceRange();
              if (IncrSwapReq) {
                IncExprSR = IncrLeftExprSR;
              } else {
                IncExprSR = IncrRightExprSR;
              }
              Offset = 0;
              NewIncrOp = "-";
              IndSign = true;
              IncrLoc = OP->getOperatorLoc();
              break;
            }
            case clang::BinaryOperator::Opcode::BO_Sub: {
              IncExprSR = OP->getRHS()->getSourceRange();
              IncrLoc = OP->getOperatorLoc();
              Offset = 0;
              NewIncrOp = "+";
              IndSign = false;
              break;
            }
            default: {
              llvm_unreachable("Not canonical loop. Kind 1\n");
            }
            }
          } else {
            llvm_unreachable("Not canonical loop. Kind 2\n");
          }
          break;
        }
        default: {
          llvm_unreachable("Not canonical loop. Kind 3\n");
        }
        }

      } else if (auto *UO = dyn_cast<clang::UnaryOperator>(IncrExpr)) {
        UnaryIncr = true;
        switch (UO->getOpcode()) {
        case clang::UnaryOperator::Opcode::UO_PostInc:
        case clang::UnaryOperator::Opcode::UO_PreInc: {
          NewIncrOp = "--";
          Offset = 1;
          IncrLoc = UO->getOperatorLoc();
          IndSign = true;
          break;
        }
        case clang::UnaryOperator::Opcode::UO_PostDec:
        case clang::UnaryOperator::Opcode::UO_PreDec: {
          NewIncrOp = "++";
          Offset = 1;
          IncrLoc = UO->getOperatorLoc();
          IndSign = false;
          break;
        }
        default: {
          llvm_unreachable("Not canonical loop. Kind 5\n");
        }
        }
      } else {
        llvm_unreachable("Not canonical loop. Kind 4\n");
      }

      clang::SourceRange CondExprSR;
      clang::SourceRange CondIndSR;
      bool CondExclude = false;

      const char *NewCondOp;
      clang::SourceLocation CondOpLoc;
      if (auto *BO = dyn_cast<clang::BinaryOperator>(CondExpr)) {
        CondOpLoc = BO->getOperatorLoc();
        auto *Left = BO->getLHS();
        auto *Right = BO->getRHS();
        MiniVisitor MV;
        MV.TraverseStmt(Right);
        bool Flag = false;
        if (auto *DRE = MV.getDRE()) {
          if (DRE->getDecl() == ASTInd) {
            CondIndSR = Right->getSourceRange();
            CondExprSR = Left->getSourceRange();
            Flag = true;
          }
        }
        if (!Flag) {
          CondIndSR = Left->getSourceRange();
          CondExprSR = Right->getSourceRange();
        }
        switch (BO->getOpcode()) {
        case clang::BinaryOperator::Opcode::BO_LE: {
          NewCondOp = ">=";
          Offset = 1;
          CondExclude = false;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_LT: {
          NewCondOp = ">=";
          Offset = 0;
          CondExclude = true;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_GE: {
          NewCondOp = "<=";
          Offset = 1;
          CondExclude = false;
          break;
        }
        case clang::BinaryOperator::Opcode::BO_GT: {
          NewCondOp = "<=";
          Offset = 0;
          CondExclude = true;
          break;
        }
        default: {
          llvm_unreachable("Not canonical loop. Kind 7\n");
        }
        }
      } else {
        llvm_unreachable("Not canonical loop. Kind 6\n");
      }
      clang::SourceRange InitExprSR;
      if (auto *BO = dyn_cast<clang::BinaryOperator>(InitExpr)) {
        InitExprSR = BO->getRHS()->getSourceRange();
      } else if (auto *DS = dyn_cast<clang::DeclStmt>(InitExpr)) {
        InitExprSR = DS->child_begin()->getSourceRange();
      } else {
        llvm_unreachable("Not canonical loop. Kind 8\n");
      }

      // source ranges for old operator signs to replace
      clang::SourceRange IncrOperatorSR(IncrLoc,
                                        IncrLoc.getLocWithOffset(Offset));

      clang::SourceRange CondOperatorSR(CondOpLoc,
                                        CondOpLoc.getLocWithOffset(Offset));
      // get strings from none transformed code
      auto TextInitExpr = mRewriter.getRewrittenText(InitExprSR);
      auto TextCondExpr = mRewriter.getRewrittenText(CondExprSR);
      std::string TextIncrExpr;
      std::string TextNewInitExpr;
      // if have start value from dep analysis
      if (Start.hasValue())
        TextInitExpr = std::to_string(Start->getSExtValue());
      // if have end and step value from dep analysis
      if (End.hasValue() && Step.hasValue()) {
        TextNewInitExpr =
            std::to_string(End->getSExtValue() - Step->getSExtValue());
        // no analysis -> calculate
      } else if (UnaryIncr) {
        if (CondExclude) {
          if (IndSign) {
            TextNewInitExpr = "((" + TextCondExpr + ")-1)";
          } else {
            TextNewInitExpr = "((" + TextCondExpr + ")+1)";
          }
        } else {
          TextNewInitExpr = "(" + TextCondExpr + ")";
        }
      } else {
        TextIncrExpr = mRewriter.getRewrittenText(IncExprSR);
        if (CondExclude) {
          if (IndSign) {
            TextNewInitExpr = "((" + TextInitExpr + ")+(((" + TextCondExpr +
                              ")-1)-(" + TextInitExpr + "))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          } else {
            TextNewInitExpr = "((" + TextInitExpr + ")-((" + TextInitExpr +
                              ")-((" + TextCondExpr + ")+1))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          }
        } else {
          if (IndSign) {
            TextNewInitExpr = "((" + TextInitExpr + ")+((" + TextCondExpr +
                              ")-(" + TextInitExpr + "))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          } else {
            TextNewInitExpr = "((" + TextInitExpr + ")-((" + TextInitExpr +
                              ")-(" + TextCondExpr + "))/(" + TextIncrExpr +
                              ")*(" + TextIncrExpr + "))";
          }
        }
      }
      // swap incr expr i = b + i -> i = i + b
      if (IncrSwapReq) {
        auto TextRightIncrExpr = mRewriter.getRewrittenText(IncrRightExprSR);
        auto TextLeftIncrExpr = mRewriter.getRewrittenText(IncrLeftExprSR);
        /*       if (Step.hasValue())
                 mRewriter.ReplaceText(
                     IncrRightExprSR,
                     "(" + std::to_string(abs(Step->getSExtValue())) + ")");
               else*/
        mRewriter.ReplaceText(IncrRightExprSR, TextLeftIncrExpr);
        mRewriter.ReplaceText(IncrLeftExprSR, TextRightIncrExpr);
      } /* else {
         if (!UnaryIncr && Step.hasValue())
           mRewriter.ReplaceText(
               IncExprSR, "(" + std::to_string(abs(Step->getSExtValue())) +
       ")");
       }*/
      // replace incr operator i++ -> i-- ; i+=k -> i-=k; etc
      mRewriter.ReplaceText(IncrOperatorSR, NewIncrOp);
      // replace cond operator
      mRewriter.ReplaceText(CondOperatorSR, NewCondOp);
      // replace init expression
      mRewriter.ReplaceText(InitExprSR, TextNewInitExpr);
      // replace cond expression
      mRewriter.ReplaceText(CondExprSR, TextInitExpr);
      return true;
    } else {
      toDiag(mSrcMgr.getDiagnostics(), FS->getBeginLoc(),
             clang::diag::err_reverse_not_canonical);
      return false;
    }
    return false;
  }
  // written to don't add  math lib
  static int64_t abs(int64_t x) { return x > 0 ? x : -x; }
  TransformationContext *mTfmCtx;
  clang::Rewriter &mRewriter;
  clang::ASTContext &mContext;
  clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;
  // get analysis from provider only if it is required
  bool mNewAnalisysRequired;
  clang::FunctionDecl *mCurrentFD;
  const CanonicalLoopSet *mCurrentLoops;
  ClangLoopReverse &mPass;
  llvm::Module &mModule;
  ClangLoopReverseProvider *mProvider;
  AnalysisSocketInfo *mSocketInfo;
  AnalysisSocket &mSocket;
  tsar::DIDependencInfo *mDIDepInfo;
  tsar::DIAliasTree *mDIAT;
  const tsar::GlobalOptions *mGO;
  tsar::MemoryMatchInfo::MemoryMatcher *mMemMatcher;
  llvm::ValueToValueMapTy &mClientToServer;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;
  bool mIsStrict;

  llvm::SmallVector<clang::StringLiteral *, 1> mStringLiterals;
  llvm::SmallVector<clang::IntegerLiteral *, 1> mIntegerLiterals;
  llvm::SmallVector<
      std::pair<const tsar::CanonicalLoopInfo *, clang::VarDecl *>, 1>
      mLoops;
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, TRAVERSE_LOOPS } mStatus;
};

} // namespace
bool ClangLoopReverse::runOnModule(Module &M) {
  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Start\n");
  releaseMemory();
  auto *TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  auto *SocketInfo = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  auto *GlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto *MemoryMatcher = &getAnalysis<MemoryMatcherImmutableWrapper>().get();
  auto *GlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  // init provider on client
  ClangLoopReverseProvider::initialize<GlobalOptionsImmutableWrapper>(
      [GlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(GlobalOpts);
      });
  ClangLoopReverseProvider::initialize<AnalysisSocketImmutableWrapper>(
      [SocketInfo](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*SocketInfo);
      });
  ClangLoopReverseProvider::initialize<TransformationEnginePass>(
      [TfmCtx, &M](TransformationEnginePass &Wrapper) {
        Wrapper.setContext(M, TfmCtx);
      });
  ClangLoopReverseProvider::initialize<MemoryMatcherImmutableWrapper>(
      [MemoryMatcher](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*MemoryMatcher);
      });
  ClangLoopReverseProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*GlobalsAA);
      });
  // init provider on server
  ClangLoopReverseServerProvider::initialize<GlobalOptionsImmutableWrapper>(
      [GlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(GlobalOpts);
      });
  auto R =
      SocketInfo->getActive()
          ->second
          .getAnalysis<GlobalsAAWrapperPass, DIMemoryEnvironmentWrapper,
                       DIMemoryTraitPoolWrapper, GlobalDefinedMemoryWrapper,
                       GlobalLiveMemoryWrapper>();
  assert(R && "Immutable passes must be available on server!");
  auto *DIMEnvServer = R->value<DIMemoryEnvironmentWrapper *>();
  ClangLoopReverseServerProvider::initialize<DIMemoryEnvironmentWrapper>(
      [DIMEnvServer](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(**DIMEnvServer);
      });
  auto *DIMTraitPoolServer = R->value<DIMemoryTraitPoolWrapper *>();
  ClangLoopReverseServerProvider::initialize<DIMemoryTraitPoolWrapper>(
      [DIMTraitPoolServer](DIMemoryTraitPoolWrapper &Wrapper) {
        Wrapper.set(**DIMTraitPoolServer);
      });
  auto &GlobalsAAServer = R->value<GlobalsAAWrapperPass *>()->getResult();
  ClangLoopReverseServerProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [&GlobalsAAServer](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAAServer);
      });
  auto *GlobalDefUseServer = R->value<GlobalDefinedMemoryWrapper *>();
  ClangLoopReverseServerProvider::initialize<GlobalDefinedMemoryWrapper>(
      [GlobalDefUseServer](GlobalDefinedMemoryWrapper &Wrapper) {
        Wrapper.set(**GlobalDefUseServer);
      });
  auto *GlobalLiveMemoryServer = R->value<GlobalLiveMemoryWrapper *>();
  ClangLoopReverseServerProvider::initialize<GlobalLiveMemoryWrapper>(
      [GlobalLiveMemoryServer](GlobalLiveMemoryWrapper &Wrapper) {
        Wrapper.set(**GlobalLiveMemoryServer);
      });
  ClangLoopReverseVisitor Visitor(TfmCtx, *this, M, GlobalOpts,
                                  &MemoryMatcher->Matcher,
                                  SocketInfo->getActive()->second);
  Visitor.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Finish\n");
  return false;
}
namespace llvm {
static void initializeClangLoopReverseServerPass(PassRegistry &);
static void initializeClangLoopReverseServerResponsePass(PassRegistry &);
} // namespace llvm

INITIALIZE_PROVIDER(ClangLoopReverseServerProvider,
                    "clang-loop-reverse-server-provider",
                    "Loop reverse (Clang, Server, Provider)")

template <> char ClangLoopReverseServerResponse::ID = 0;
INITIALIZE_PASS(ClangLoopReverseServerResponse, "clang-loop-reverse-response",
                "Loop reverse (Clang, Server, Response)", true, false)

char ClangLoopReverseServer::ID = 0;
INITIALIZE_PASS(ClangLoopReverseServer, "clang-loop-reverse-server",
                "Loop reverse (Clang, Server)", false, false)

INITIALIZE_PROVIDER(ClangLoopReverseProvider, "clang-loop-reverse-provider",
                    "Loop reverse (Clang, Provider)")

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopReverse, "clang-loop-reverse",
                               "Clang based loop reverse", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangLoopReverseInfo)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)
INITIALIZE_PASS_IN_GROUP_END(ClangLoopReverse, "clang-loop-reverse",
                             "Clang based loop reverse", false, false,
                             TransformationQueryManager::getPassRegistry())

ClangLoopReverseServer::ClangLoopReverseServer() : AnalysisServer(ID) {
  initializeClangLoopReverseServerPass(*PassRegistry::getPassRegistry());
}
char ClangLoopReverse::ID = '0';
ClangLoopReverse::ClangLoopReverse() : ModulePass(ID) {
  initializeClangLoopReverseProviderPass(*PassRegistry::getPassRegistry());
  initializeClangLoopReverseServerPass(*PassRegistry::getPassRegistry());
  initializeClangLoopReverseServerProviderPass(
      *PassRegistry::getPassRegistry());
  initializeClangLoopReverseServerResponsePass(
      *PassRegistry::getPassRegistry());
}
