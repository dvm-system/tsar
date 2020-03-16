#include "tsar/Transform/Clang/LoopSwap.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <unordered_set>
#include <vector>
// required for function pass
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Support/PassProvider.h"
#include "llvm/IR/LegacyPassManager.h"
//
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Support/GlobalOptions.h"
//
#include "tsar/Analysis/Clang/PerfectLoop.h"
using namespace llvm;
using namespace clang;
using namespace tsar;
using namespace std;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-swap"

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
        mStub("0") {}
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
      // to get name necessary match IR Value* to AST ValueDecl*, it is possible
      // that on Linux 'Value->getName()' doesn't works
      bool IsCanonical;
      bool IsPerfect;
      if (LI) {
        IsCanonical = LI->isCanonical();
        IsPerfect = mPerfectLoopInfo->count(LI->getLoop());
        mInductions.push_back(std::pair<clang::StringRef, clang::ForStmt *>(
            mMemMatcher->find<IR>(LI->getInduction())->get<AST>()->getName(),
            FS));
      } else {
        // if it's not canonical loop, gathering induction is difficult, there
        // may be no induction at all; so just set a stub; '0' can't be an
        // identifier
        IsCanonical = false;
        IsPerfect = false; // don't need this if not canonical
        mInductions.push_back(
            std::pair<clang::StringRef, clang::ForStmt *>(mStub, FS));
      }
      if (IsCanonical && IsPerfect)
        mLoopsKinds.push_back(CANONICAL_AND_PERFECT);
      else if (IsCanonical && !IsPerfect)
        mLoopsKinds.push_back(NOT_PERFECT);
      else if (!IsCanonical && IsPerfect)
        mLoopsKinds.push_back(NOT_CANONICAL);
      else
        mLoopsKinds.push_back(NOT_CANONICAL_AND_PERFECT);

      return RecursiveASTVisitor::TraverseForStmt(FS);
    } else {
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
  }
  bool TraverseDecl(Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      errs() << "NOT FOR STMT\n";
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(), diag::err_assert);
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
      // if found expand clause
      bool Found = false;
      if (findClause(P, ClauseId::LoopSwap, Clauses)) {
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
        SwapClauseVisitor SCV;
        for (auto C : Clauses)
          SCV.TraverseStmt(C);
        auto &Literals = SCV.getLiterals();
        for (auto L : Literals) {
          mSwaps.push_back(L);
        }
        Clauses.clear();
        mStatus = Status::TRAVERSE_STMT;
        return true;
      } else {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
    }

    case Status::TRAVERSE_STMT: {
      if (!isa<ForStmt>(S)) {
        errs() << "NOT FOR STMT\n";
        toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(), diag::err_assert);
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
      mStatus = GET_REFERENCES;
      auto res = TraverseForStmt((ForStmt *)S);
      mStatus = SEARCH_PRAGMA;
      errs() << "INDUCTIONS\n";
      for (auto ind : mInductions) {
        errs() << ind.first << "\n";
      }

      bool Changed = false;

      int MaxIdx = 0;

      for (int i = 0; i < mSwaps.size(); i++) {
        bool Found = false;
        auto Str = mSwaps[i]->getString();
        for (int j = 0; j < mInductions.size(); j++) {
          if (mInductions[j].first == mStub) {
            errs() << "EXPECTED CANONICAL LOOP\n";
            toDiag(mSrcMgr.getDiagnostics(),
                   mInductions[j].second->getBeginLoc(), diag::err_assert);
            resetVisitor();
            return RecursiveASTVisitor::TraverseStmt(S);
          }
          if (mInductions[j].first == Str) {
            if (j > MaxIdx)
              MaxIdx = j;
            Found = true;
            break;
          }
        }
        if (!Found) {
          errs() << "INDUCTION NOT FOUND " << Str << "\n";
          toDiag(mSrcMgr.getDiagnostics(), mSwaps[i]->getBeginLoc(),
                 diag::err_assert);
          resetVisitor();
          return RecursiveASTVisitor::TraverseStmt(S);
        }
      }
      errs() << "MAXIDX " << MaxIdx << "\n";
      bool IsPossible = true;
      for (int i = 0; i < MaxIdx; i++) {
        if (mLoopsKinds[i] == NOT_CANONICAL_AND_PERFECT) {
          errs() << "EXPECTED CANONICAL AND PERFECT LOOP "
                 << mInductions[i].first << "\n";
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::err_assert);
        } else if (mLoopsKinds[i] == NOT_CANONICAL) {
          errs() << "EXPECTED CANONICAL LOOP " << mInductions[i].first << "\n";
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::err_assert);
        } else if (mLoopsKinds[i] == NOT_PERFECT) {
          errs() << "EXPECTED PERFECT LOOP " << mInductions[i].first << "\n";
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 diag::err_assert);
        }
      }
      if (!IsPossible) {
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      std::string *Order = new std::string[MaxIdx + 1];
      for (int i = 0; i < MaxIdx + 1; i++) {
        Order[i] = mInductions[i].first;
      }
      for (int i = 0; i < mSwaps.size(); i += 2) {
        auto FirstIdx = findIdx(Order, MaxIdx + 1, mSwaps[i]->getString());
        auto SecondIdx = findIdx(Order, MaxIdx + 1, mSwaps[i + 1]->getString());
        errs() << "swap " << mSwaps[i]->getString() << " "
               << mSwaps[i + 1]->getString() << "\n";
        auto Buf = Order[FirstIdx];
        Order[FirstIdx] = Order[SecondIdx];
        Order[SecondIdx] = Buf;
      }
      errs() << "ORDER\n";
      for (int i = 0; i < MaxIdx + 1; i++) {
        errs() << Order[i] << "\n";
      }

      // replace
      for (int i = 0; i < MaxIdx + 1; i++) {
        if (Order[i] != mInductions[i].first) {
          auto Idx = findIdx(mInductions, Order[i]);

          errs() << "replace " << mInductions[i].first << " "
                 << mInductions[Idx].first << "\n";
          clang::SourceRange Destination(
              mInductions[i].second->getInit()->getBeginLoc(),
              mInductions[i].second->getInc()->getEndLoc());
          clang::SourceRange Source(
              mInductions[Idx].second->getInit()->getBeginLoc(),
              mInductions[Idx].second->getInc()->getEndLoc());
          mRewriter.ReplaceText(Destination, Source);
        }
      }

      delete[] Order;
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
  }

private:
  // use carefully
  int findIdx(std::string *Array, int Size, std::string Item) {
    for (int i = 0; i < Size; i++) {
      if (Array[i] == Item)
        return i;
    }
  }
  int findIdx(
      llvm::SmallVectorImpl<std::pair<clang::StringRef, clang::ForStmt *>>
          &Array,
      std::string Item) {
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

  enum LoopKind {
    CANONICAL_AND_PERFECT,
    NOT_PERFECT,
    NOT_CANONICAL,
    NOT_CANONICAL_AND_PERFECT
  };
  llvm::SmallVector<LoopKind, 2> mLoopsKinds;
  llvm::SmallVector<std::pair<clang::StringRef, clang::ForStmt *>, 2>
      mInductions;
  std::vector<clang::StringLiteral *> mSwaps;
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, GET_REFERENCES } mStatus;
  const std::string mStub;
}; // namespace

} // namespace

bool ClangLoopSwapPass::runOnModule(llvm::Module &M) {
  errs() << "Start loop swap pass\n";
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
  errs() << "Finish loop swap pass\n";
  return false;
}
