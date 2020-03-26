#include "tsar/Transform/Clang/LoopReverse.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Support/PassProvider.h"
#include "llvm/IR/LegacyPassManager.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>

#include "tsar/Support/Clang/Utils.h"
using namespace llvm;
using namespace clang;
using namespace tsar;
using namespace std;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-reverse"
#define DEBUG_PREFIX "[LoopReverse]: "

char ClangLoopReversePass::ID = 0;

namespace {
using LoopReversePassProvider =
    FunctionPassProvider<TransformationEnginePass, CanonicalLoopPass,
                         DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
                         DIDependencyAnalysisPass, DIEstimateMemoryPass,
                         GlobalOptionsImmutableWrapper,
                         MemoryMatcherImmutableWrapper, ClangPerfectLoopPass>;

class LoopReversePassGroupInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &PM) const override {
    PM.add(createMemoryMatcherPass());
    PM.add(createDIMemoryTraitPoolStorage());
    PM.add(createDIMemoryEnvironmentStorage());
  }
};
} // namespace

INITIALIZE_PROVIDER_BEGIN(LoopReversePassProvider, "loop-reverse-provider",
                          "Loop reverse provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PROVIDER_END(LoopReversePassProvider, "loop-reverse-provider",
                        "Loop reverse provider")

INITIALIZE_PASS_IN_GROUP_BEGIN(
    ClangLoopReversePass, "clang-reverse", "Reverse loop pass", false, false,
    tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(LoopReversePassGroupInfo)
INITIALIZE_PASS_DEPENDENCY(LoopReversePassProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(
    ClangLoopReversePass, "clang-reverse", "Reverse loop pass", false, false,
    tsar::TransformationQueryManager::getPassRegistry())

void ClangLoopReversePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<LoopReversePassProvider>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createClangLoopReversePass() {
  return new ClangLoopReversePass();
}

namespace {
inline void dbgPrintln(const char *Msg) {
  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << Msg << "\n");
}
class ClauseVisitor : public RecursiveASTVisitor<ClauseVisitor> {
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
class MiniVisitor : public RecursiveASTVisitor<MiniVisitor> {
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
    : public RecursiveASTVisitor<ClangLoopReverseVisitor> {
public:
  ClangLoopReverseVisitor(TransformationContext *TfmCtx,
                          ClangGlobalInfoPass::RawInfo &RawInfo,
                          ClangLoopReversePass &Pass, llvm::Module &M)
      : mTfmCtx(TfmCtx), mRawInfo(RawInfo), mRewriter(TfmCtx->getRewriter()),
        mContext(TfmCtx->getContext()), mSrcMgr(mRewriter.getSourceMgr()),
        mLangOpts(mRewriter.getLangOpts()), mPass(Pass), mModule(M),
        mStatus(SEARCH_PRAGMA), mGO(NULL), mDIAT(NULL), mDIDepInfo(NULL),
        mMemMatcher(NULL), mPerfectLoopInfo(NULL), mCurrentLoops(NULL),
        mIsStrict(false) {}
  const CanonicalLoopInfo *getLoopInfo(ForStmt *FS) {
    if (!FS)
      return NULL;
    for (auto Info : *mCurrentLoops) {
      if (Info->getASTLoop() == FS) {
        return Info;
      }
    }
    return NULL;
  }
  bool TraverseForStmt(ForStmt *FS) {
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
  bool TraverseDecl(Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             diag::err_reverse_not_forstmt);
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }
  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case Status::SEARCH_PRAGMA: {
      Pragma P(*S);
      llvm::SmallVector<clang::Stmt *, 1> Clauses;
      if (findClause(P, ClauseId::LoopReverse, Clauses)) {
        dbgPrintln("Pragma -> start");
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
          dbgPrintln("Found nostrict clause");
        }
        // remove clauses
        llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
        auto IsPossible =
            pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts, ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   diag::warn_remove_directive_in_include);
          else
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   diag::warn_remove_directive);
        Rewriter::RewriteOptions RemoveEmptyLine;
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
      if (!isa<ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(),
               diag::err_reverse_not_forstmt);
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      if (mNewAnalisysRequired) {
        Function *F = mModule.getFunction(mCurrentFD->getNameAsString());
        mProvider = &mPass.getAnalysis<LoopReversePassProvider>(*F);
        mCurrentLoops =
            &mProvider->get<CanonicalLoopPass>().getCanonicalLoopInfo();
        mDIAT = &mProvider->get<DIEstimateMemoryPass>().getAliasTree();
        mGO = &mProvider->get<GlobalOptionsImmutableWrapper>().getOptions();
        mDIDepInfo =
            &mProvider->get<DIDependencyAnalysisPass>().getDependencies();
        mMemMatcher =
            &mProvider->get<MemoryMatcherImmutableWrapper>().get().Matcher;
        mPerfectLoopInfo =
            &mProvider->get<ClangPerfectLoopPass>().getPerfectLoopInfo();
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
                 diag::warn_reverse_cant_match);
      }

      for (int i = 0; i < mIntegerLiterals.size(); i++) {
        auto Num = mIntegerLiterals[i]->getValue().getZExtValue() - 1;
        if (Num < mLoops.size()) {
          mToTransform.insert(Num);
        } else {
          toDiag(mSrcMgr.getDiagnostics(), mIntegerLiterals[i]->getLocStart(),
                 diag::warn_reverse_cant_match);
        }
      }

      for (auto Num : mToTransform) {
        if (transformLoop(mLoops[Num].first)) {
          errs() << "Transformed " << mLoops[Num].second->getName() << "\n";
        } else
          errs() << "Not transformed " << mLoops[Num].second->getName() << "\n";
      }
      resetVisitor();
      return Ret;
    }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }
  bool VisitFunctionDecl(FunctionDecl *FD) {
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
  bool transformLoop(const tsar::CanonicalLoopInfo *LI) {
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
      if (UnaryIncr) {
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
        mRewriter.ReplaceText(IncrRightExprSR, TextLeftIncrExpr);
        mRewriter.ReplaceText(IncrLeftExprSR, TextRightIncrExpr);
      }
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
             diag::err_reverse_not_canonical);
      return false;
    }
    return false;
  }
  TransformationContext *mTfmCtx;
  ClangGlobalInfoPass::RawInfo &mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  // get analysis from provider only if it is required
  bool mNewAnalisysRequired;
  FunctionDecl *mCurrentFD;
  const CanonicalLoopSet *mCurrentLoops;
  ClangLoopReversePass &mPass;
  llvm::Module &mModule;
  LoopReversePassProvider *mProvider;
  tsar::DIDependencInfo *mDIDepInfo;
  tsar::DIAliasTree *mDIAT;
  const tsar::GlobalOptions *mGO;
  tsar::MemoryMatchInfo::MemoryMatcher *mMemMatcher;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;
  bool mIsStrict;

  llvm::SmallVector<clang::StringLiteral *, 1> mStringLiterals;
  llvm::SmallVector<clang::IntegerLiteral *, 1> mIntegerLiterals;
  llvm::SmallVector<
      std::pair<const tsar::CanonicalLoopInfo *, clang::VarDecl *>, 1>
      mLoops;
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, TRAVERSE_LOOPS } mStatus;
}; // namespace

} // namespace

bool ClangLoopReversePass::runOnModule(llvm::Module &M) {
  dbgPrintln("Start Loop Reverse pass");
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  // set provider's wrappers
  LoopReversePassProvider::initialize<TransformationEnginePass>(
      [&M, &TfmCtx](TransformationEnginePass &TEP) {
        TEP.setContext(M, TfmCtx);
      });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  LoopReversePassProvider::initialize<MemoryMatcherImmutableWrapper>(
      [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*MMWrapper);
      });
  auto &DIMEW = getAnalysis<DIMemoryEnvironmentWrapper>();
  LoopReversePassProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEW](DIMemoryEnvironmentWrapper &Wrapper) { Wrapper.set(*DIMEW); });
  auto &DIMTPW = getAnalysis<DIMemoryTraitPoolWrapper>();
  LoopReversePassProvider::initialize<DIMemoryTraitPoolWrapper>(
      [&DIMTPW](DIMemoryTraitPoolWrapper &Wrapper) { Wrapper.set(*DIMTPW); });
  auto &mGlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  LoopReversePassProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&mGlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&mGlobalOpts);
      });
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  ClangLoopReverseVisitor Visitor(TfmCtx, GIP.getRawInfo(), *this, M);
  Visitor.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  dbgPrintln("Finish Loop Reverse pass");
  return false;
}
