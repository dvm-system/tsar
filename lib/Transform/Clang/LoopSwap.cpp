#include "tsar/Transform/Clang/LoopSwap.h"
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
using namespace llvm;
using namespace clang;
using namespace tsar;
using namespace std;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-swap"
#define DEBUG_PREFIX "[LoopSwap]: "

char ClangLoopSwapPass::ID = 0;

namespace {
using LoopSwapPassProvider =
    FunctionPassProvider<TransformationEnginePass, CanonicalLoopPass,
                         DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
                         DIDependencyAnalysisPass, DIEstimateMemoryPass,
                         GlobalOptionsImmutableWrapper,
                         MemoryMatcherImmutableWrapper, ClangPerfectLoopPass>;

class LoopSwapPassGroupInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &PM) const override {
    PM.add(createMemoryMatcherPass());
    PM.add(createDIMemoryTraitPoolStorage());
    PM.add(createDIMemoryEnvironmentStorage());
  }
};
} // namespace

INITIALIZE_PROVIDER_BEGIN(LoopSwapPassProvider, "loop-swap-provider",
                          "Loop swap provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PROVIDER_END(LoopSwapPassProvider, "loop-swap-provider",
                        "Loop swap provider")

INITIALIZE_PASS_IN_GROUP_BEGIN(
    ClangLoopSwapPass, "clang-swap", "", false, false,
    tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(LoopSwapPassGroupInfo)
INITIALIZE_PASS_DEPENDENCY(LoopSwapPassProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(
    ClangLoopSwapPass, "clang-swap", "", false, false,
    tsar::TransformationQueryManager::getPassRegistry())

void ClangLoopSwapPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<LoopSwapPassProvider>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createClangLoopSwapPass() { return new ClangLoopSwapPass(); }

namespace {
inline void dbgPrintln(const char * Msg) {
  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << Msg << "\n");
}
class SwapClauseVisitor : public RecursiveASTVisitor<SwapClauseVisitor> {
public:
  bool VisitStringLiteral(clang::StringLiteral *SL) {
    if (SL->getString() != "swap")
      mLiterals.push_back(SL);
    return true;
  }
  llvm::SmallVectorImpl<clang::StringLiteral *> &getLiterals() {
    return mLiterals;
  }

private:
  llvm::SmallVector<clang::StringLiteral *, 2> mLiterals;
};
class ClangLoopSwapVisitor : public RecursiveASTVisitor<ClangLoopSwapVisitor> {
public:
  ClangLoopSwapVisitor(TransformationContext *TfmCtx,
                       ClangGlobalInfoPass::RawInfo &RawInfo,
                       ClangLoopSwapPass &Pass, llvm::Module &M)
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
    if (mStatus == GET_REFERENCES) {
      auto LI = getLoopInfo(FS);
      bool IsCanonical;
      bool IsPerfect;
      clang::ValueDecl *VD = NULL;
      if (LI) {
        IsCanonical = LI->isCanonical();
        IsPerfect = mPerfectLoopInfo->count(LI->getLoop());
        // to get Value name it is necessary to match IR Value* to AST
        // ValueDecl*, it is possible that on Linux 'Value->getName()' doesn't
        // works
        VD = mMemMatcher->find<IR>(LI->getInduction())->get<AST>();
        mInductions.push_back(
            std::pair<clang::ValueDecl *, clang::ForStmt *>(VD, FS));
      } else {
        // if it's not canonical loop, gathering induction is difficult, there
        // may be no induction at all; so just set a NULL stub
        IsCanonical = false;
        IsPerfect = false; // don't need this if not canonical
        mInductions.push_back(
            std::pair<clang::ValueDecl *, clang::ForStmt *>(NULL, FS));
      }
      if (IsCanonical && IsPerfect)
        mLoopsKinds.push_back(CANONICAL_AND_PERFECT);
      else if (IsCanonical && !IsPerfect)
        mLoopsKinds.push_back(NOT_PERFECT);
      else if (!IsCanonical && IsPerfect)
        mLoopsKinds.push_back(NOT_CANONICAL);
      else
        mLoopsKinds.push_back(NOT_CANONICAL_AND_PERFECT);
      // check only if canonical and perfect, otherwise it's not necessary or
      // there is no LoopInfo
      if (mIsStrict && IsCanonical && IsPerfect) {
        bool Dependency = false;
        auto *Loop = LI->getLoop()->getLoop();
        auto *LoopID = Loop->getLoopID();
        auto DepItr = mDIDepInfo->find(LoopID);
        auto &DIDepSet = DepItr->get<DIDependenceSet>();
        DenseSet<const DIAliasNode *> Coverage;
        accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
                                            mGO->IgnoreRedundantMemory);
        if (!Coverage.empty()) {
          for (auto &DIAT : DIDepSet) {
            if (!Coverage.count(DIAT.getNode()))
              continue;
            if (DIAT.is_any<trait::Output, trait::Anti, trait::Flow>()) {
              Dependency = true;
            }
          }
        }
        if (Dependency) {
          mLoopsKinds[mLoopsKinds.size() - 1] = HAS_DEPENDENCY;
        }
      }

      return RecursiveASTVisitor::TraverseForStmt(FS);
    } else {
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
  }
  bool TraverseDecl(Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             diag::warn_loopswap_not_forstmt);
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
      if (findClause(P, ClauseId::LoopSwap, Clauses)) {
        dbgPrintln("Pragma -> start");
        // collect info from clauses
        SwapClauseVisitor SCV;
        for (auto C : Clauses)
          SCV.TraverseStmt(C);
        auto &Literals = SCV.getLiterals();
        for (auto L : Literals) {
          mSwaps.push_back(L);
        }
        // also check for nostrict clause
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
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
        /// set to true then removing (in RewriterBuffer) works incorrect.
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
               diag::warn_loopswap_not_forstmt);
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // Macro check
      bool HasMacro = false;
      for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawInfo.Macros,
                     [&HasMacro, this](clang::SourceLocation Loc) {
                       if (!HasMacro) {
                         toDiag(mContext.getDiagnostics(), Loc,
                                diag::note_assert_no_macro);
                         HasMacro = true;
                       }
                     });
      if (HasMacro) {
        mStatus = SEARCH_PRAGMA;
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // get analisis from provider for current fucntion, if it not done already
      if (mNewAnalisysRequired) {
        Function *F = mModule.getFunction(mCurrentFD->getNameAsString());
        mProvider = &mPass.getAnalysis<LoopSwapPassProvider>(*F);
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
      // collect inductions
      mStatus = GET_REFERENCES;
      auto res = TraverseForStmt((ForStmt *)S);
      mStatus = SEARCH_PRAGMA;

      // match inductions from clauses to real inductions
      int MaxIdx = 0;
      llvm::SmallVector<clang::ValueDecl *, 2> mValueSwaps;
      for (int i = 0; i < mSwaps.size(); i++) {
        bool Found = false;
        auto Str = mSwaps[i]->getString();
        for (int j = 0; j < mInductions.size(); j++) {
          if (mInductions[j].first == NULL) {
            toDiag(mSrcMgr.getDiagnostics(),
                   mInductions[j].second->getBeginLoc(),
                   diag::warn_loopswap_not_canonical);
            resetVisitor();
            return RecursiveASTVisitor::TraverseStmt(S);
          }
          if (mInductions[j].first->getName() == Str) {
            mValueSwaps.push_back(mInductions[j].first);
            if (j > MaxIdx)
              MaxIdx = j;
            Found = true;
            break;
          }
        }
        if (!Found) {
          toDiag(mSrcMgr.getDiagnostics(), mSwaps[i]->getBeginLoc(),
                 diag::warn_loopswap_id_not_found);
          resetVisitor();
          return RecursiveASTVisitor::TraverseStmt(S);
        }
      }

      // check that loops was canonical and perfect
      bool IsPossible = true;
      for (int i = 0; i < MaxIdx; i++) {
        if (mLoopsKinds[i] == NOT_CANONICAL_AND_PERFECT) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::warn_loopswap_not_canonical);
        } else if (mLoopsKinds[i] == NOT_CANONICAL) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::warn_loopswap_not_canonical);
        } else if (mLoopsKinds[i] == NOT_PERFECT) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::warn_loopswap_not_perfect);
        } else if (mLoopsKinds[i] == HAS_DEPENDENCY) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::err_loopswap_dependency);
        }
      }
      if (!IsPossible) {
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // copy inductions to new 'Order' vector
      clang::SmallVector<clang::ValueDecl *, 2> Order;
      for (int i = 0; i < MaxIdx + 1; i++) {
        Order.push_back(mInductions[i].first);
      }
      // perform transpositions
      for (int i = 0; i < mValueSwaps.size(); i += 2) {
        auto FirstIdx = findIdx(Order, mValueSwaps[i]);
        auto SecondIdx = findIdx(Order, mValueSwaps[i + 1]);
        auto Buf = Order[FirstIdx];
        Order[FirstIdx] = Order[SecondIdx];
        Order[SecondIdx] = Buf;
      }

      // perform code replace
      for (int i = 0; i < MaxIdx + 1; i++) {
        if (Order[i] != mInductions[i].first) {
          auto Idx = findIdx(mInductions, Order[i]);
          clang::SourceRange Destination(
              mInductions[i].second->getInit()->getBeginLoc(),
              mInductions[i].second->getInc()->getEndLoc());
          clang::SourceRange Source(
              mInductions[Idx].second->getInit()->getBeginLoc(),
              mInductions[Idx].second->getInc()->getEndLoc());
          mRewriter.ReplaceText(Destination, Source);
        }
      }

      dbgPrintln("Pragma -> done");
      resetVisitor();
      return res;
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
    mSwaps.clear();
    mInductions.clear();
    mLoopsKinds.clear();
    mIsStrict = false;
  }

private:
  // use this only if sure there is an Item
  int findIdx(llvm::SmallVectorImpl<clang::ValueDecl *> &Vec, ValueDecl *Item) {
    for (int i = 0; i < Vec.size(); i++) {
      if (Vec[i] == Item)
        return i;
    }
  }
  int findIdx(
      llvm::SmallVectorImpl<std::pair<clang::ValueDecl *, clang::ForStmt *>>
          &Array,
      clang::ValueDecl *Item) {
    for (int i = 0; i < Array.size(); i++) {
      if (Array[i].first == Item)
        return i;
    }
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
  ClangLoopSwapPass &mPass;
  llvm::Module &mModule;
  LoopSwapPassProvider *mProvider;
  tsar::DIDependencInfo *mDIDepInfo;
  tsar::DIAliasTree *mDIAT;
  const tsar::GlobalOptions *mGO;
  tsar::MemoryMatchInfo::MemoryMatcher *mMemMatcher;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;
  bool mIsStrict;
  enum LoopKind {
    CANONICAL_AND_PERFECT,
    NOT_PERFECT,
    NOT_CANONICAL,
    NOT_CANONICAL_AND_PERFECT,
    HAS_DEPENDENCY
  };
  llvm::SmallVector<LoopKind, 2> mLoopsKinds;
  llvm::SmallVector<std::pair<clang::ValueDecl *, clang::ForStmt *>, 2>
      mInductions;
  std::vector<clang::StringLiteral *> mSwaps;
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, GET_REFERENCES } mStatus;
}; // namespace

} // namespace

bool ClangLoopSwapPass::runOnModule(llvm::Module &M) {
  dbgPrintln( "Start Loop Swap pass");
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  // set provider's wrappers
  LoopSwapPassProvider::initialize<TransformationEnginePass>(
      [&M, &TfmCtx](TransformationEnginePass &TEP) {
        TEP.setContext(M, TfmCtx);
      });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  LoopSwapPassProvider::initialize<MemoryMatcherImmutableWrapper>(
      [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*MMWrapper);
      });
  auto &DIMEW = getAnalysis<DIMemoryEnvironmentWrapper>();
  LoopSwapPassProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEW](DIMemoryEnvironmentWrapper &Wrapper) { Wrapper.set(*DIMEW); });
  auto &DIMTPW = getAnalysis<DIMemoryTraitPoolWrapper>();
  LoopSwapPassProvider::initialize<DIMemoryTraitPoolWrapper>(
      [&DIMTPW](DIMemoryTraitPoolWrapper &Wrapper) { Wrapper.set(*DIMTPW); });
  auto &mGlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  LoopSwapPassProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&mGlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&mGlobalOpts);
      });
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  ClangLoopSwapVisitor vis(TfmCtx, GIP.getRawInfo(), *this, M);
  vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  dbgPrintln("Finish Loop Swap pass");
  return false;
}
