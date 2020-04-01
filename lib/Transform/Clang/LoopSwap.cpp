#include "tsar/Transform/Clang/LoopSwap.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
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
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <algorithm>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Verifier.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-loop-swap"
#define DEBUG_PREFIX "[LoopSwap]: "

// server, providers, passgroupinfo
namespace {
// local provider
using ClangLoopSwapProvider =
    FunctionPassAAProvider<AnalysisSocketImmutableWrapper, LoopInfoWrapperPass,
                           ParallelLoopPass, CanonicalLoopPass, LoopMatcherPass,
                           DFRegionInfoPass, ClangDIMemoryMatcherPass,
                           ClangPerfectLoopPass>;
// server provider
using ClangLoopSwapServerProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass>;
// server response
using ClangLoopSwapServerResponse =
    AnalysisResponsePass<GlobalsAAWrapperPass, DIMemoryTraitPoolWrapper,
                         DIMemoryEnvironmentWrapper, GlobalDefinedMemoryWrapper,
                         GlobalLiveMemoryWrapper, ClonedDIMemoryMatcherWrapper,
                         ClangLoopSwapServerProvider>;
// server
class ClangLoopSwapServer final : public AnalysisServer {
public:
  static char ID;
  ClangLoopSwapServer();
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void prepareToClone(Module &ClientM,
                      ValueToValueMapTy &ClientToServer) override;
  void initializeServer(Module &CM, Module &SM, ValueToValueMapTy &CToS,
                        legacy::PassManager &PM) override;
  void addServerPasses(Module &M, legacy::PassManager &PM) override;
  void prepareToClose(legacy::PassManager &PM) override;
};
// pass info
class ClangLoopSwapInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
} // namespace
// server methods
namespace {
void ClangLoopSwapServer::getAnalysisUsage(AnalysisUsage &AU) const {
  AnalysisServer::getAnalysisUsage(AU);
  ClientToServerMemory::getAnalysisUsage(AU);
  AU.addRequired<GlobalOptionsImmutableWrapper>();
}

void ClangLoopSwapServer::prepareToClone(Module &ClientM,
                                         ValueToValueMapTy &ClientToServer) {
  ClientToServerMemory::prepareToClone(ClientM, ClientToServer);
}

void ClangLoopSwapServer::initializeServer(Module &CM, Module &SM,
                                           ValueToValueMapTy &CToS,
                                           legacy::PassManager &PM) {
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>();
  PM.add(createGlobalOptionsImmutableWrapper(&GO.getOptions()));
  PM.add(createGlobalDefinedMemoryStorage());
  PM.add(createGlobalLiveMemoryStorage());
  PM.add(createDIMemoryTraitPoolStorage());
  ClientToServerMemory::initializeServer(*this, CM, SM, CToS, PM);
}

void ClangLoopSwapServer::addServerPasses(Module &M, legacy::PassManager &PM) {
  auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  addImmutableAliasAnalysis(PM);
  addBeforeTfmAnalysis(PM);
  addAfterSROAAnalysis(GO, M.getDataLayout(), PM);
  addAfterLoopRotateAnalysis(PM);
  PM.add(createVerifierPass());
  PM.add(new ClangLoopSwapServerResponse);
}

void ClangLoopSwapServer::prepareToClose(legacy::PassManager &PM) {
  ClientToServerMemory::prepareToClose(PM);
}
} // namespace
// passgroupinfo methods
namespace {
void ClangLoopSwapInfo::addBeforePass(legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(new ClangLoopSwapServer);
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
}

void ClangLoopSwapInfo::addAfterPass(legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}
} // namespace
// this pass methods
void ClangLoopSwap::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangLoopSwapProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<ClangRegionCollector>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

namespace {
class SwapClauseVisitor : public clang::RecursiveASTVisitor<SwapClauseVisitor> {
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
class ClangLoopSwapVisitor
    : public clang::RecursiveASTVisitor<ClangLoopSwapVisitor> {
public:
  ClangLoopSwapVisitor(TransformationContext *TfmCtx,
                       ClangGlobalInfoPass::RawInfo &RawInfo,
                       MemoryMatchInfo::MemoryMatcher *Matcher,
                       const GlobalOptions *GO, AnalysisSocket &Socket,
                       ClangLoopSwap &Pass, llvm::Module &M)
      : mTfmCtx(TfmCtx), mRewriter(TfmCtx->getRewriter()),
        mContext(TfmCtx->getContext()), mSrcMgr(mRewriter.getSourceMgr()),
        mLangOpts(mRewriter.getLangOpts()), mRawInfo(RawInfo), mSocket(Socket),
        mPass(Pass), mModule(M), mMemMatcher(Matcher), mGO(GO),
        mClientToServer(
            **mSocket.getAnalysis<AnalysisClientServerMatcherWrapper>()
                  ->value<AnalysisClientServerMatcherWrapper *>()),
        mStatus(SEARCH_PRAGMA), mIsStrict(false), mDIAT(NULL), mDIDepInfo(NULL),
        mPerfectLoopInfo(NULL), mCanonicalLoopInfo(NULL) {}
  const CanonicalLoopInfo *getLoopInfo(clang::ForStmt *FS) {
    if (!FS)
      return NULL;
    for (auto Info : *mCanonicalLoopInfo) {
      if (Info->getASTLoop() == FS) {
        return Info;
      }
    }
    return NULL;
  }
  bool TraverseForStmt(clang::ForStmt *FS) {
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
        auto ServerLoopID = cast<MDNode>(*mClientToServer.getMappedMD(LoopID));
        auto DepItr = mDIDepInfo->find(ServerLoopID);
        if (DepItr == mDIDepInfo->end()) {
          mLoopsKinds[mLoopsKinds.size() - 1] = NO_ANALYSIS;
        } else {
          auto &DIDepSet = DepItr->get<DIDependenceSet>();
          DenseSet<const DIAliasNode *> Coverage;
          auto &tmp = *mDIAT->getTopLevelNode();
          accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
                                              mGO->IgnoreRedundantMemory);
          if (!Coverage.empty()) {
            for (auto &Trait : DIDepSet) {
              if (!Coverage.count(Trait.getNode()))
                continue;
              if (Trait.is_any<trait::Output, trait::Anti, trait::Flow>()) {
                Dependency = true;
              }
            }
          }
          if (Dependency) {
            mLoopsKinds[mLoopsKinds.size() - 1] = HAS_DEPENDENCY;
          }
        }
      }

      return RecursiveASTVisitor::TraverseForStmt(FS);
    } else {
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
  }
  bool TraverseDecl(clang::Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             clang::diag::warn_loopswap_not_forstmt);
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
      if (findClause(P, ClauseId::LoopSwap, Clauses)) {
        LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Found swap clause\n");
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
          LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Found nostrict clause\n");
        }
        // remove clauses
        llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
        auto IsPossible =
            pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts, ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   clang ::diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   clang::diag::warn_remove_directive_in_include);
          else
            toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
                   clang::diag::warn_remove_directive);
        clang::Rewriter::RewriteOptions RemoveEmptyLine;
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
      if (!isa<clang::ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(),
               clang::diag::warn_loopswap_not_forstmt);
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // Macro check
      bool HasMacro = false;
      for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawInfo.Macros,
                     [&HasMacro, this](clang::SourceLocation Loc) {
                       if (!HasMacro) {
                         toDiag(mContext.getDiagnostics(), Loc,
                                clang::diag::note_assert_no_macro);
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
        mProvider = &mPass.getAnalysis<ClangLoopSwapProvider>(*F);
        mCanonicalLoopInfo =
            &mProvider->get<CanonicalLoopPass>().getCanonicalLoopInfo();
        mPerfectLoopInfo =
            &mProvider->get<ClangPerfectLoopPass>().getPerfectLoopInfo();
        auto RF =
            mSocket.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(
                *F);
        assert(RF &&
               "Dependence analysis must be available for a parallel loop!");
        mDIAT = &RF->value<DIEstimateMemoryPass *>()->getAliasTree();
        mDIDepInfo =
            &RF->value<DIDependencyAnalysisPass *>()->getDependencies();
        mNewAnalisysRequired = false;
      }
      // collect inductions
      mStatus = GET_REFERENCES;
      auto res = TraverseForStmt((clang::ForStmt *)S);
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
                   clang::diag::warn_loopswap_not_canonical);
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
                 clang::diag::warn_loopswap_id_not_found);
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
                 clang::diag::warn_loopswap_not_canonical);
        } else if (mLoopsKinds[i] == NOT_CANONICAL) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 clang::diag::warn_loopswap_not_canonical);
        } else if (mLoopsKinds[i] == NOT_PERFECT) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 clang::diag::warn_loopswap_not_perfect);
        } else if (mLoopsKinds[i] == HAS_DEPENDENCY) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 clang::diag::err_loopswap_dependency);
        } else if (mLoopsKinds[i] == NO_ANALYSIS) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(), mInductions[i].second->getBeginLoc(),
                 clang::diag::warn_loopswap_no_analysis);
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

      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Pragma -> done\n");
      resetVisitor();
      return res;
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
    mSwaps.clear();
    mInductions.clear();
    mLoopsKinds.clear();
    mIsStrict = false;
  }

private:
  // use this only if sure there is an Item
  int findIdx(llvm::SmallVectorImpl<clang::ValueDecl *> &Vec,
              clang::ValueDecl *Item) {
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
  clang::Rewriter &mRewriter;
  clang::ASTContext &mContext;
  clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;
  ClangGlobalInfoPass::RawInfo &mRawInfo;
  // get analysis from provider only if it is required
  bool mNewAnalisysRequired;
  clang::FunctionDecl *mCurrentFD;
  ClangLoopSwap &mPass;
  llvm::Module &mModule;
  const tsar::GlobalOptions *mGO;
  ClangLoopSwapProvider *mProvider;
  tsar::MemoryMatchInfo::MemoryMatcher *mMemMatcher;
  const CanonicalLoopSet *mCanonicalLoopInfo;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;
  tsar::DIDependencInfo *mDIDepInfo;
  tsar::DIAliasTree *mDIAT;
  AnalysisSocket &mSocket;
  llvm::ValueToValueMapTy &mClientToServer;

  bool mIsStrict;
  enum LoopKind {
    CANONICAL_AND_PERFECT,
    NOT_PERFECT,
    NOT_CANONICAL,
    NOT_CANONICAL_AND_PERFECT,
    HAS_DEPENDENCY,
    NO_ANALYSIS
  };
  llvm::SmallVector<LoopKind, 2> mLoopsKinds;
  llvm::SmallVector<std::pair<clang::ValueDecl *, clang::ForStmt *>, 2>
      mInductions;
  std::vector<clang::StringLiteral *> mSwaps;
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, GET_REFERENCES } mStatus;
};

} // namespace

bool ClangLoopSwap::runOnModule(Module &M) {
  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Start\n");
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
  ClangLoopSwapProvider::initialize<GlobalOptionsImmutableWrapper>(
      [GlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(GlobalOpts);
      });
  ClangLoopSwapProvider::initialize<AnalysisSocketImmutableWrapper>(
      [SocketInfo](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*SocketInfo);
      });
  ClangLoopSwapProvider::initialize<TransformationEnginePass>(
      [TfmCtx, &M](TransformationEnginePass &Wrapper) {
        Wrapper.setContext(M, TfmCtx);
      });
  ClangLoopSwapProvider::initialize<MemoryMatcherImmutableWrapper>(
      [MemoryMatcher](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*MemoryMatcher);
      });
  ClangLoopSwapProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*GlobalsAA);
      });
  // init provider on server
  ClangLoopSwapServerProvider::initialize<GlobalOptionsImmutableWrapper>(
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
  ClangLoopSwapServerProvider::initialize<DIMemoryEnvironmentWrapper>(
      [DIMEnvServer](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(**DIMEnvServer);
      });
  auto *DIMTraitPoolServer = R->value<DIMemoryTraitPoolWrapper *>();
  ClangLoopSwapServerProvider::initialize<DIMemoryTraitPoolWrapper>(
      [DIMTraitPoolServer](DIMemoryTraitPoolWrapper &Wrapper) {
        Wrapper.set(**DIMTraitPoolServer);
      });
  auto &GlobalsAAServer = R->value<GlobalsAAWrapperPass *>()->getResult();
  ClangLoopSwapServerProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [&GlobalsAAServer](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAAServer);
      });
  auto *GlobalDefUseServer = R->value<GlobalDefinedMemoryWrapper *>();
  ClangLoopSwapServerProvider::initialize<GlobalDefinedMemoryWrapper>(
      [GlobalDefUseServer](GlobalDefinedMemoryWrapper &Wrapper) {
        Wrapper.set(**GlobalDefUseServer);
      });
  auto *GlobalLiveMemoryServer = R->value<GlobalLiveMemoryWrapper *>();
  ClangLoopSwapServerProvider::initialize<GlobalLiveMemoryWrapper>(
      [GlobalLiveMemoryServer](GlobalLiveMemoryWrapper &Wrapper) {
        Wrapper.set(**GlobalLiveMemoryServer);
      });
  auto &RegionInfo = getAnalysis<ClangRegionCollector>().getRegionInfo();
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto &RawInfo = getAnalysis<ClangGlobalInfoPass>().getRawInfo();
  ClangLoopSwapVisitor CLSV(TfmCtx, RawInfo, &MemoryMatcher->Matcher,
                            GlobalOpts, SocketInfo->getActive()->second, *this,
                            M);
  CLSV.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "Finish\n");
  return false;
}

namespace llvm {
static void initializeClangLoopSwapServerPass(PassRegistry &);
static void initializeClangLoopSwapServerResponsePass(PassRegistry &);
} // namespace llvm

INITIALIZE_PROVIDER(ClangLoopSwapServerProvider,
                    "clang-loop-swap-server-provider",
                    "Loop swap (Clang, Server, Provider)")

template <> char ClangLoopSwapServerResponse::ID = 0;
INITIALIZE_PASS(ClangLoopSwapServerResponse, "clang-loop-swap-response",
                "Loop swap (Clang, Server, Response)", true, false)

char ClangLoopSwapServer::ID = 0;
INITIALIZE_PASS(ClangLoopSwapServer, "clang-loop-swap-server",
                "Loop swap (Clang, Server)", false, false)

INITIALIZE_PROVIDER(ClangLoopSwapProvider, "clang-loop-swap-provider",
                    "Loop swap (Clang, Provider)")

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopSwap, "clang-loop-swap",
                               "Clang based loop swap", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangLoopSwapInfo)
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
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangLoopSwap, "clang-loop-swap",
                             "Clang based loop swap", false, false,
                             TransformationQueryManager::getPassRegistry())

ClangLoopSwapServer::ClangLoopSwapServer() : AnalysisServer(ID) {
  initializeClangLoopSwapServerPass(*PassRegistry::getPassRegistry());
}
char ClangLoopSwap::ID = '0';
ClangLoopSwap::ClangLoopSwap() : ModulePass(ID) {
  initializeClangLoopSwapProviderPass(*PassRegistry::getPassRegistry());
  initializeClangLoopSwapServerPass(*PassRegistry::getPassRegistry());
  initializeClangLoopSwapServerProviderPass(*PassRegistry::getPassRegistry());
  initializeClangLoopSwapServerResponsePass(*PassRegistry::getPassRegistry());
}
