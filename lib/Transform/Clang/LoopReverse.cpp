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
  bool VisitForStmt(ForStmt *FS) {
    if (!FS)
      return false;
    if (mStatus == TRAVERSE_STMT) {
      // get analisis from provider for current fucntion, if it not done
      // already
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
      auto LI = getLoopInfo(FS);
      if (auto *LI = getLoopInfo(FS)) {
        if (LI->isCanonical()) {
          auto *InitExpr = FS->getInit();
          auto *CondExpr = FS->getCond();
          auto *IncrExpr = FS->getInc();
          auto *IRInd = LI->getInduction();
          auto *ASTInd = mMemMatcher->find<IR>(IRInd)->get<AST>();
          bool IndSign = true;
          clang::SourceLocation IncrLoc;
          const char *NewIncrOp;
          int Offset;
          // Increment expression transform
          if (auto *BO = dyn_cast<clang::BinaryOperator>(IncrExpr)) {
            switch (BO->getOpcode()) {
            case clang::BinaryOperator::Opcode::BO_AddAssign: {
              NewIncrOp = "-=";
              Offset = 1;
              IncrLoc = BO->getOperatorLoc();
              IndSign = true;
              break;
            }
            case clang::BinaryOperator::Opcode::BO_SubAssign: {
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
                  bool SwapReq = false;
                  MiniVisitor MV;
                  MV.TraverseStmt(Right);
                  if (auto *DRE = MV.getDRE()) {
                    if (DRE->getDecl() == ASTInd) {
                      SwapReq = true;
                    }
                  }
                  if (SwapReq) {
                    auto RightText =
                        mRewriter.getRewrittenText(Right->getSourceRange());
                    auto LeftText =
                        mRewriter.getRewrittenText(Left->getSourceRange());
                    mRewriter.ReplaceText(Left->getSourceRange(), RightText);
                    mRewriter.ReplaceText(Right->getSourceRange(), LeftText);
                  }
                  Offset = 0;
                  NewIncrOp = "-";
                  IndSign = true;
                  IncrLoc = OP->getOperatorLoc();
                  break;
                }
                case clang::BinaryOperator::Opcode::BO_Sub: {
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
            MV.VisitStmt(Right);
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

          std::string ExclExpr;
          if (CondExclude) {
            if (IndSign)
              ExclExpr = "-1";
            else
              ExclExpr = "+1";
          }
          clang::SourceRange IncrOperatorSR(IncrLoc,
                                            IncrLoc.getLocWithOffset(Offset));
          mRewriter.ReplaceText(IncrOperatorSR, NewIncrOp);

          clang::SourceRange CondOperatorSR(CondOpLoc,
                                            CondOpLoc.getLocWithOffset(Offset));
          mRewriter.ReplaceText(CondOperatorSR, NewCondOp);
          auto TextInitExpr = mRewriter.getRewrittenText(InitExprSR);
          auto TextCondExpr = mRewriter.getRewrittenText(CondExprSR) + ExclExpr;
          mRewriter.ReplaceText(InitExprSR, TextCondExpr);
          mRewriter.ReplaceText(CondExprSR, TextInitExpr);
          resetVisitor();
        } else {
          resetVisitor();
          toDiag(mSrcMgr.getDiagnostics(), FS->getBeginLoc(),
                 diag::err_reverse_not_canonical);
          return RecursiveASTVisitor::VisitForStmt(FS);
        }
      } else {
        resetVisitor();
        toDiag(mSrcMgr.getDiagnostics(), FS->getBeginLoc(),
               diag::err_reverse_not_canonical);
        return RecursiveASTVisitor::VisitForStmt(FS);
      }
      return RecursiveASTVisitor::VisitForStmt(FS);
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
      return RecursiveASTVisitor::TraverseStmt(S);
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
  }

private:
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
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, GET_REFERENCES } mStatus;
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
