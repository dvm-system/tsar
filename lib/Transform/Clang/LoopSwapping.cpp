//===- LoopSwapping.cpp - Source-level Renaming of Local Objects - *- C++ -*===//
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
// The file declares a pass to perform swapping of specific loops.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/SourceLocationTraverse.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Support/Tags.h"
#include "tsar/Transform/Clang/Passes.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/Analysis/LoopInfo.h>
#include <vector>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-l-swap"

namespace {

/// This provides access to function-level analysis results on server.
using ClangLoopSwappingProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass>;

using RangeVector = SmallVector<SourceRange, 2>;

using RangePairVector = std::vector<RangeVector>;

using LoopVector = SmallVector<Loop *, 2>;

using LoopPairVector = std::vector<LoopVector>;

using DIAliasTraitVector = std::vector<const DIAliasTrait *>;

class LoopVisitor : public RecursiveASTVisitor<LoopVisitor> {
private:
  enum TraverseState { NONE, PRAGMA, OUTERFOR, INNERFOR };
public:
  LoopVisitor(Rewriter &Rewr, const LoopMatcherPass::LoopMatcher &LM,
      const ASTImportInfo &ImportInfo)
    : mRewriter(Rewr)
    , mSrcMgr(Rewr.getSourceMgr())
    , mImportInfo(ImportInfo)
    , mLangOpts(Rewr.getLangOpts())
    , mLoopInfo(LM)
    , mState(TraverseState::NONE)
  {}

  void EnterInScope() {
    mForLocations.clear();
    mForIRs.clear();
  }

  void ExitFromScope() {
    mForLocations.clear();
    mForIRs.clear();
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    Pragma P(*S);
    if (P) {
      // Search for loop swapping clause and disable renaming in other pragmas.
      if (findClause(P, ClauseId::SwapLoops, mClauses)) {
        llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
        auto IsPossible =
          pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts, mImportInfo,
                              ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
              diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
              diag::warn_remove_directive_in_include);
          else
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
              diag::warn_remove_directive);
        Rewriter::RewriteOptions RemoveEmptyLine;
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
        /// set to true then removing (in RewriterBuffer) works incorrect.
        RemoveEmptyLine.RemoveLineIfEmpty = false;
        for (auto SR : ToRemove)
          mRewriter.RemoveText(SR, RemoveEmptyLine);
        mState = TraverseState::PRAGMA;
        EnterInScope();
      }
      return true;
    }
    auto Res = RecursiveASTVisitor::TraverseStmt(S);
    return Res;
  }

  bool TraverseCompoundStmt(CompoundStmt *S) {
    if (mState == TraverseState::PRAGMA) {
      mState = TraverseState::OUTERFOR;
      mForLocations.clear();
      mForIRs.clear();
      auto Res = RecursiveASTVisitor::TraverseCompoundStmt(S);
      mState = TraverseState::PRAGMA;
      mRangePairs.push_back(mForLocations);
      mLoopPairs.push_back(mForIRs);
      ExitFromScope();
      return Res;
    }
    return RecursiveASTVisitor::TraverseCompoundStmt(S);
  }

  bool TraverseForStmt(ForStmt *S) {
    if (mState == TraverseState::OUTERFOR) {
      auto Match = mLoopInfo.find<AST>(S);
      if (Match != mLoopInfo.end()) {
        Loop *MatchLoop = Match->get<IR>();
        mForIRs.push_back(MatchLoop);
        SourceRange Range(S->getBeginLoc(), S->getEndLoc());
        mForLocations.push_back(Range);
      }
      mState = TraverseState::INNERFOR;
      auto Res = RecursiveASTVisitor::TraverseForStmt(S);
      mState = TraverseState::OUTERFOR;
      return Res;
    }
    return RecursiveASTVisitor::TraverseForStmt(S);
  }

  RangePairVector getRangePairs() const {
    return mRangePairs;
  }

  LoopPairVector getLoopPairs() const {
    return mLoopPairs;
  }

  size_t getLoopCount() const {
    return mRangePairs.size();
  }
  
  void printLocations() const {
    LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: 'for' loop locations:\n");
    size_t LoopNumber = 0;
    for (auto locs: mRangePairs) {
      for (auto location : locs) {
        LLVM_DEBUG(dbgs() << "Loop #" << LoopNumber << ":\n");
        SourceLocation Begin = location.getBegin();
        SourceLocation End = location.getEnd();
        LLVM_DEBUG(dbgs() << "\tBegin: ");
        Begin.print(dbgs(), mSrcMgr);
        LLVM_DEBUG(dbgs() << "\n\tEnd: ");
        End.print(dbgs(), mSrcMgr);
        LLVM_DEBUG(dbgs() << '\n');
        LoopNumber++;
      }
    }
  }

private:
  Rewriter &mRewriter;
  SourceManager &mSrcMgr;
  const ASTImportInfo &mImportInfo;
  const LangOptions &mLangOpts;
  const LoopMatcherPass::LoopMatcher &mLoopInfo;
  TraverseState mState;
  SmallVector<Stmt *, 1> mClauses;
  RangeVector mForLocations;
  LoopVector mForIRs;
  RangePairVector mRangePairs;
  LoopPairVector mLoopPairs;
};

class ClangLoopSwapping : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangLoopSwapping() : FunctionPass(ID) {
    initializeClangLoopSwappingPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  void swapLoops(const LoopVisitor &Visitor);
  DIAliasTraitVector getLoopTraits(MDNode *LoopID) const;
  bool isSwappingAvailable(const LoopVector &Loops) const;
  bool hasSameReductionKind(const DIAliasTraitVector &TV0,
                            const DIAliasTraitVector &TV1) const;
  bool hasTrueOrAntiDependence(const DIAliasTraitVector &TV0,
                               const DIAliasTraitVector &TV1) const;

  Function *mFunction = nullptr;
  TransformationContext *mTfmCtx = nullptr;
  const GlobalOptions *mGlobalOpts = nullptr;
  AnalysisSocketInfo *mSocketInfo = nullptr;
  DIDependencInfo *mDIDepInfo = nullptr;
  DIAliasTree *mDIAT = nullptr;
  std::function<ObjectID(ObjectID)> mGetLoopID;
  const SourceManager *mSrcMgr = nullptr;
};

class ClangLoopSwappingInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override {
    addImmutableAliasAnalysis(Passes);
    addInitialTransformations(Passes);
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(createDIMemoryAnalysisServer());
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createAnalysisWaitServerPass());
  }

  void addAfterPass(legacy::PassManager &Passes) const override {
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
  }
};

} //namespace

char ClangLoopSwapping::ID = 0;

DIAliasTraitVector ClangLoopSwapping::getLoopTraits(MDNode *LoopID) const {
  auto DepItr = mDIDepInfo->find(LoopID);
  assert(DepItr != mDIDepInfo->end() && "Loop must be analyzed!");
  auto &DIDepSet = DepItr->get<DIDependenceSet>();
  DenseSet<const DIAliasNode *> Coverage;
  accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
                                      mGlobalOpts->IgnoreRedundantMemory);
  DIAliasTraitVector Traits;
  for (auto &TS : DIDepSet) {
    if (!Coverage.count(TS.getNode()))
      continue;
    Traits.push_back(&TS);
  }
  return Traits;
}

bool ClangLoopSwapping::hasSameReductionKind(
    const DIAliasTraitVector &TV0, const DIAliasTraitVector &TV1) const {
  for (auto &TS0: TV0) {
    auto *Node0 = TS0->getNode();
    MemoryDescriptor Dptr0 = *TS0;
    if (!Dptr0.is<trait::Reduction>())
      continue;
    for (auto &TS1: TV1) {
      auto *Node1 = TS1->getNode();
      MemoryDescriptor Dptr1 = *TS1;
      if (Node0 == Node1 && Dptr1.is<trait::Reduction>()) {
        LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: Same nodes with reduction.\n");
        auto I0 = TS0->begin(), I1 = TS1->begin();
        auto *Red0 = (**I0).get<trait::Reduction>();
        auto *Red1 = (**I1).get<trait::Reduction>();
        if (!Red0 || !Red1) {
          LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: Unknown Reduction.\n");
          return false;
        }
        auto Kind0 = Red0->getKind(), Kind1 = Red1->getKind();
        if (Kind0 == trait::DIReduction::RK_NoReduction ||
            Kind1 == trait::DIReduction::RK_NoReduction) {
          LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: Unknown Reduction.\n");
          return false;
        }
        if (Kind0 != Kind1)
          return false;
      }
    }
  }
  return true;
}

bool ClangLoopSwapping::hasTrueOrAntiDependence(
    const DIAliasTraitVector &TV0, const DIAliasTraitVector &TV1) const {
  for (auto &TS0: TV0) {
    auto *Node0 = TS0->getNode();
    MemoryDescriptor Dptr0 = *TS0;
    for (auto &TS1: TV1) {
      auto *Node1 = TS1->getNode();
      MemoryDescriptor Dptr1 = *TS1;
      if (Node0 == Node1) {
        if (Dptr0.is<trait::Readonly>() && !Dptr1.is<trait::Readonly>()) {
          // anti dependence
          return true;
        } else if (Dptr1.is<trait::Readonly>() && !Dptr0.is<trait::Readonly>()){
          // true dependence
          return true;
        }
      }
    }
  }
  return false;
}

bool ClangLoopSwapping::isSwappingAvailable(const LoopVector &Loops) const {
  auto HasLoopID = [this](MDNode*& LoopID, int LoopN) {
    if (!LoopID || !(LoopID = mGetLoopID(LoopID))) {
      LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: ignore loop without ID (loop " <<
        LoopN << ").");
      return false;
    }
    return true;
  };
  auto *LoopID0 = Loops[0]->getLoopID();
  auto *LoopID1 = Loops[1]->getLoopID();
  if (!HasLoopID(LoopID0, 0))
    return false;
  if (!HasLoopID(LoopID1, 1))
    return false;
  auto Traits0 = getLoopTraits(LoopID0);
  auto Traits1 = getLoopTraits(LoopID1);
  if (!hasSameReductionKind(Traits0, Traits1)) {
    toDiag(mSrcMgr->getDiagnostics(), diag::warn_loop_swapping_diff_reduction);
    return false;
  }
  if (hasTrueOrAntiDependence(Traits0, Traits1)) {
    toDiag(mSrcMgr->getDiagnostics(),
           diag::warn_loop_swapping_true_anti_dependence);
    return false;
  }
  return true;
}

void ClangLoopSwapping::swapLoops(const LoopVisitor &Visitor) {
  const auto &RangePairs = Visitor.getRangePairs();
  const auto &LoopPairs = Visitor.getLoopPairs();
  Rewriter &mRewriter = mTfmCtx->getRewriter();
  for (size_t i = 0; i < RangePairs.size(); i++) {
    const auto &Ranges = RangePairs[i];
    const auto &Loops = LoopPairs[i];
    if (Ranges.size() < 2) {
      toDiag(mSrcMgr->getDiagnostics(),
              diag::warn_loop_swapping_missing_loop);
      continue;
    }
    if (Ranges.size() > 2) {
      toDiag(mSrcMgr->getDiagnostics(),
              diag::warn_loop_swapping_redundant_loop);
    }
    if (isSwappingAvailable(Loops)) {
      const auto &FirstRange = Ranges[0];
      const auto &SecondRange = Ranges[1];
      const auto &FirstLoop = mRewriter.getRewrittenText(FirstRange);
      const auto &SecondLoop = mRewriter.getRewrittenText(SecondRange);
      mRewriter.ReplaceText(FirstRange, SecondLoop);
      mRewriter.ReplaceText(SecondRange, FirstLoop);
    }
  }
}

bool ClangLoopSwapping::runOnFunction(Function &F) {
  mFunction = &F;
  auto *M = F.getParent();
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M->getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto FuncDecl = mTfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  mSocketInfo = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto *Socket = mSocketInfo->getActiveSocket();
  auto RF =
      Socket->getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(F);
  assert(RF && "Dependence analysis must be available for a parallel loop!");
  mDIAT = &RF->value<DIEstimateMemoryPass *>()->getAliasTree();
  mDIDepInfo = &RF->value<DIDependencyAnalysisPass *>()->getDependencies();
  auto R = Socket->getAnalysis<AnalysisClientServerMatcherWrapper>();
  auto *Matcher = R->value<AnalysisClientServerMatcherWrapper *>();
  mGetLoopID = [Matcher](ObjectID ID) {
    auto ServerID = (*Matcher)->getMappedMD(ID);
    return ServerID ? cast<MDNode>(*ServerID) : nullptr;
  };
  auto &mLoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  LoopVisitor Visitor(mTfmCtx->getRewriter(), mLoopInfo, *ImportInfo);
  mSrcMgr = &mTfmCtx->getRewriter().getSourceMgr();
  Visitor.TraverseDecl(FuncDecl);
  if (Visitor.getLoopCount() == 0) {
    LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: no loop found.\n");
    return false;
  }
  Visitor.printLocations();
  swapLoops(Visitor);
  return false;
}

void ClangLoopSwapping::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DIDependencyAnalysisPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createClangLoopSwapping() {
  return new ClangLoopSwapping();
}

INITIALIZE_PROVIDER_BEGIN(ClangLoopSwappingProvider,
                  "clang-loop-swapping-provider",
                  "Loop Swapping (Clang, Provider)");
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass);
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass);
INITIALIZE_PROVIDER_END(ClangLoopSwappingProvider,
                    "clang-loop-swapping-provider",
                    "Loop Swapping (Clang, Provider)");
                    
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopSwapping,"clang-l-swap",
  "'for' Loops Swapping (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry());
INITIALIZE_PASS_IN_GROUP_INFO(ClangLoopSwappingInfo);
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass);
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass);
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingProvider);
INITIALIZE_PASS_IN_GROUP_END(ClangLoopSwapping,"clang-l-swap",
  "'for' Loops Swapping (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry());