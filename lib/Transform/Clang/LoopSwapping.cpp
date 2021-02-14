//===- LoopSwapping.cpp - Loop Swapping (Clang) -----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/VariableCollector.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Support/Clang/SourceLocationTraverse.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Support/Tags.h"
#include "tsar/Transform/Clang/Passes.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <stack>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-l-swap"

namespace {

/// This provides access to function-level analysis results on server.
using ClangLoopSwappingProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass,
    ClonedDIMemoryMatcherWrapper>;
using DIAliasTraitList = SmallVector<const DIAliasTrait *, 8>;
using LoopRangeInfo = std::pair<Loop *, SourceRange>;
using LoopRangeList = SmallVector<LoopRangeInfo, 2>;
using PragmaInfoList = DenseMap<Stmt *, LoopRangeList>;

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

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    Pragma P(*S);
    if (P) {
      // Search for loop swapping clause and disable renaming in other pragmas.
      if (findClause(P, ClauseId::SwapLoops, mClauses)) {
        SmallVector<CharSourceRange, 8> ToRemove;
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
        /*for (auto SR : ToRemove)
          mRewriter.RemoveText(SR, RemoveEmptyLine);*/
        mPragmaLoopsInfo.insert(std::make_pair(S, LoopRangeList()));
        mPragmaStack.push(S);
        mState = TraverseState::PRAGMA;
      }
      return true;
    }
    if (mState == TraverseState::PRAGMA && !dyn_cast<CompoundStmt>(S)) {
      toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
          diag::error_loop_swapping_expect_compound);
      return false;
    }
    if (mState == TraverseState::OUTERFOR && !dyn_cast<ForStmt>(S)) {
      toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
          diag::error_loop_swapping_redundant_stmt);
      return false;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseCompoundStmt(CompoundStmt *S) {
    if (mState == TraverseState::PRAGMA) {
      mState = TraverseState::OUTERFOR;
      auto Res = RecursiveASTVisitor::TraverseCompoundStmt(S);
      mPragmaStack.pop();
      mState = mPragmaStack.empty() ? TraverseState::NONE : TraverseState::OUTERFOR;
      return Res;
    }
    return RecursiveASTVisitor::TraverseCompoundStmt(S);
  }

  bool TraverseForStmt(ForStmt *S) {
    if (mState == TraverseState::OUTERFOR) {
      auto Match = mLoopInfo.find<AST>(S);
      if (Match != mLoopInfo.end()) {
        auto &LRL = mPragmaLoopsInfo[mPragmaStack.top()];
        LRL.push_back(std::make_pair(Match->get<IR>(), S->getSourceRange()));
      } else {
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
            diag::warn_loop_swapping_lost_loop);
      }
      mState = TraverseState::INNERFOR;
      auto Res = RecursiveASTVisitor::TraverseForStmt(S);
      mState = TraverseState::OUTERFOR;
      return Res;
    }
    return RecursiveASTVisitor::TraverseForStmt(S);
  }

  const PragmaInfoList &getPragmaLoopsInfo() const {
    return mPragmaLoopsInfo;
  }

  bool hasPragma() const {
    return !mPragmaLoopsInfo.empty();
  }
  
  #ifdef LLVM_DEBUG
  void printLocations() const {
    int N = 0;
    for (auto It = mPragmaLoopsInfo.begin(); It != mPragmaLoopsInfo.end();
        ++It, ++N) {
      dbgs() << "Pragma " << N << " (" << It->first <<"):\n";
      for (const auto &Info : It->second) {
        const auto LoopPtr = Info.first;
        const auto &Range = Info.second;
        dbgs() << "\t[Range]\n";
        dbgs() << "\tBegin:" << Range.getBegin().printToString(mSrcMgr)
            << "\n";
        dbgs() << "\tEnd:" << Range.getEnd().printToString(mSrcMgr) <<"\n";
        dbgs() << "\t\n\t[Loop]\n";
        const auto &LoopText = mRewriter.getRewrittenText(Range);
        dbgs() << LoopText << "\n\n";
      }
    }
  }
  #endif

private:
  Rewriter &mRewriter;
  SourceManager &mSrcMgr;
  const ASTImportInfo &mImportInfo;
  const LangOptions &mLangOpts;
  const LoopMatcherPass::LoopMatcher &mLoopInfo;
  TraverseState mState;
  SmallVector<Stmt *, 1> mClauses;
  PragmaInfoList mPragmaLoopsInfo;
  std::stack<Stmt *> mPragmaStack;
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
  enum OutputDepKind : char {
    Output = 0,
    UnknownReduction,
    DiffReduction,
    SameReduction,
    Private
  };
  void swapLoops(const LoopVisitor &Visitor);
  DIAliasTraitList getLoopTraits(MDNode *LoopID) const;
  bool isSwappingAvailable(const LoopRangeList &LRL, const Stmt *Pragma);
  OutputDepKind getOutputDepType(const DIAliasTrait *T0,
                                 const DIAliasTrait *T1) const;
  Function *mFunction = nullptr;
  TransformationContext *mTfmCtx = nullptr;
  const GlobalOptions *mGlobalOpts = nullptr;
  AnalysisSocketInfo *mSocketInfo = nullptr;
  DIDependencInfo *mDIDepInfo = nullptr;
  DIAliasTree *mDIAT = nullptr;
  std::function<ObjectID(ObjectID)> mGetLoopID;
  const SourceManager *mSrcMgr = nullptr;
  const ClangDIMemoryMatcherPass::DIMemoryMatcher *mMemoryMatcher = nullptr;
  const tsar::ClonedDIMemoryMatcher *mClonedMatcher = nullptr;
  VariableCollector mASTVars;
  
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
    Passes.add(createMemoryMatcherPass());
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

DIAliasTraitList ClangLoopSwapping::getLoopTraits(MDNode *LoopID) const {
  auto DepItr = mDIDepInfo->find(LoopID);
  assert(DepItr != mDIDepInfo->end() && "Loop must be analyzed!");
  auto &DIDepSet = DepItr->get<DIDependenceSet>();
  DenseSet<const DIAliasNode *> Coverage;
  accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
                                      mGlobalOpts->IgnoreRedundantMemory);
  DIAliasTraitList Traits;
  for (auto &TS : DIDepSet) {
    if (!Coverage.count(TS.getNode()))
      continue;
    Traits.push_back(&TS);
  }
  return Traits;
}

ClangLoopSwapping::OutputDepKind ClangLoopSwapping::getOutputDepType(
    const DIAliasTrait *T0, const DIAliasTrait *T1) const {
  auto Kind = OutputDepKind::Output;
  for (auto MemIt0 = T0->begin(); MemIt0 != T0->end(); MemIt0++) {
    for (auto MemIt1 = T1->begin(); MemIt1 != T1->end(); MemIt1++) {
      if ((*MemIt0)->getMemory() == (*MemIt1)->getMemory()) {
        if ((**MemIt0).is<trait::Private>() && (**MemIt1).is<trait::Private>()){
          LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: Private.\n");
          Kind = OutputDepKind::Private;
          continue;
        }
        auto *Red0 = (**MemIt0).get<trait::Reduction>();
        auto *Red1 = (**MemIt1).get<trait::Reduction>();
        auto Ind0 = (**MemIt0).is<trait::Induction>();
        auto Ind1 = (**MemIt1).is<trait::Induction>();
        if (Red0 && Red1) {
          auto Kind0 = Red0->getKind(), Kind1 = Red1->getKind();
          if (Kind0 == trait::DIReduction::RK_NoReduction ||
              Kind1 == trait::DIReduction::RK_NoReduction) {
            return OutputDepKind::UnknownReduction;
          }
          if (Kind0 != Kind1)
            return OutputDepKind::DiffReduction;
          Kind = OutputDepKind::SameReduction;
        } else if ((Red0 && Ind1) || (Ind0 && Red1)) {
          auto RedKind = Red0 ? Red0->getKind() : Red1->getKind();
          if (RedKind == trait::DIReduction::RK_NoReduction)
            return OutputDepKind::UnknownReduction;
          else if (!RedKind == trait::DIReduction::RK_Add)
            return OutputDepKind::DiffReduction;
          else
            Kind = OutputDepKind::SameReduction;
        } else if (Ind0 && Ind1) {
          Kind = OutputDepKind::SameReduction;
        } else {
          return OutputDepKind::Output;
        }
      }
    }
  }
  return Kind;
}

bool ClangLoopSwapping::isSwappingAvailable(
    const LoopRangeList &LRL, const Stmt *Pragma) {
  auto ClientLoopID0 = LRL[0].first->getLoopID();
  auto ClientLoopID1 = LRL[1].first->getLoopID();
  assert(ClientLoopID0 && ClientLoopID1 && "LoopID must not be null!");
  auto *LoopID0 = mGetLoopID(ClientLoopID0);
  auto *LoopID1 = mGetLoopID(ClientLoopID1);
  if (!LoopID0) {
    toDiag(mSrcMgr->getDiagnostics(), LRL[0].second.getBegin(),
        diag::warn_loop_swapping_no_loop_id);
    return false;
  }
  if (!LoopID1) {
    toDiag(mSrcMgr->getDiagnostics(), LRL[1].second.getBegin(),
        diag::warn_loop_swapping_no_loop_id);
    return false;
  }
  bool IsAvailable = true;
  SpanningTreeRelation<const DIAliasTree *> STR(mDIAT);
  auto GetVarLoc = [this](VarDecl *VarDecl) -> std::string {
    return VarDecl ?
        VarDecl->getSourceRange().printToString(*mSrcMgr) : "<unknown>";
  };
  for (auto &T0: getLoopTraits(LoopID0)) {
    clang::VarDecl *VarDecl = mASTVars.findDecl(*(*T0->begin())->getMemory(),
        *mMemoryMatcher, *mClonedMatcher).first;
    for (auto &T1: getLoopTraits(LoopID1)) {
      if (STR.isUnreachable(T0->getNode(), T1->getNode()))
          continue;
      if (!T0->is<trait::Readonly>() && T1->is<trait::Readonly>()) {
        toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
            diag::warn_loop_swapping_true_dependence) <<
            VarDecl->getName() << GetVarLoc(VarDecl);
        IsAvailable = false;
        break;
      } else if (T0->is<trait::Readonly>() && !T1->is<trait::Readonly>()) {
        toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
            diag::warn_loop_swapping_anti_dependence) <<
            VarDecl->getName() << GetVarLoc(VarDecl);
        IsAvailable = false;
        break;
      } else if (!T0->is<trait::Readonly>() && !T1->is<trait::Readonly>()) {
        auto OutDepKind = getOutputDepType(T0, T1);
        LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: Output dependency kind: " <<
            OutDepKind << "\n");
        if (OutDepKind == OutputDepKind::Output) {
          toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
              diag::warn_loop_swapping_output_dependence) <<
              VarDecl->getName() << GetVarLoc(VarDecl);
          IsAvailable = false;
          break;
        } else if (OutDepKind == OutputDepKind::DiffReduction) {
          toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
              diag::warn_loop_swapping_diff_reduction) <<
              VarDecl->getName() << GetVarLoc(VarDecl);
          IsAvailable = false;
          break;
        } else if (OutDepKind == OutputDepKind::UnknownReduction) {
          toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
              diag::warn_loop_swapping_unknown_reduction) <<
              VarDecl->getName() << GetVarLoc(VarDecl);
          IsAvailable = false;
          break;
        }
      }
    }
    if (!IsAvailable)
      break;
  }
  return IsAvailable;
}

void ClangLoopSwapping::swapLoops(const LoopVisitor &Visitor) {
  Rewriter &Rewr = mTfmCtx->getRewriter();
  auto GetLoopEnd = [this, &Rewr](const SourceRange &LoopRange) {
    Token SemiTok;
    return (!getRawTokenAfter(LoopRange.getEnd(), *mSrcMgr,
        Rewr.getLangOpts(), SemiTok) && SemiTok.is(tok::semi)) ?
        SemiTok.getLocation() : LoopRange.getEnd();
  };
  auto &PragmaLoopsInfo = Visitor.getPragmaLoopsInfo();
  for (auto It = PragmaLoopsInfo.begin(); It != PragmaLoopsInfo.end(); It++) {
    auto &Pragma = It->first;
    auto &Loops = It->second;
    if (Loops.size() < 2) {
      toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
              diag::warn_loop_swapping_missing_loop);
      continue;
    }
    if (Loops.size() > 2) {
      toDiag(mSrcMgr->getDiagnostics(), Pragma->getBeginLoc(),
              diag::warn_loop_swapping_redundant_loop);
    }
    if (isSwappingAvailable(Loops, Pragma)) {
      auto Range0 = Loops[0].second;
      auto Range1 = Loops[1].second;
      Range0.setEnd(GetLoopEnd(Range0));
      Range1.setEnd(GetLoopEnd(Range1));
      auto Range0End = Range0.getEnd();
      auto Range1Begin = Range1.getBegin();
      const auto &LoopText0 = Rewr.getRewrittenText(Range0);
      const auto &LoopText1 = Rewr.getRewrittenText(Range1);
      Rewr.RemoveText(Range0);
      Rewr.RemoveText(Range1);
      Rewr.InsertTextBefore(Range0End, LoopText1);
      Rewr.InsertTextAfter(Range1Begin, LoopText0);
    }
  }
}

bool ClangLoopSwapping::runOnFunction(Function &F) {
  mFunction = &F;
  auto *M = F.getParent();
  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  mTfmCtx = TfmInfo ? TfmInfo->getContext(*M) : nullptr;
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
  assert(RF && "Dependence analysis must be available!");
  mDIAT = &RF->value<DIEstimateMemoryPass *>()->getAliasTree();
  mDIDepInfo = &RF->value<DIDependencyAnalysisPass *>()->getDependencies();
  auto RM = Socket->getAnalysis<AnalysisClientServerMatcherWrapper,
      ClonedDIMemoryMatcherWrapper>();
  auto *Matcher = RM->value<AnalysisClientServerMatcherWrapper *>();
  mGetLoopID = [Matcher](ObjectID ID) {
    auto ServerID = (*Matcher)->getMappedMD(ID);
    return ServerID ? cast<MDNode>(*ServerID) : nullptr;
  };
  auto *ServerF = cast<Function>((**Matcher)[&F]);
  mClonedMatcher =
      (**RM->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF];
  auto &mLoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  mMemoryMatcher = &getAnalysis<ClangDIMemoryMatcherPass>().getMatcher();
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  LoopVisitor Visitor(mTfmCtx->getRewriter(), mLoopInfo, *ImportInfo);
  mSrcMgr = &mTfmCtx->getRewriter().getSourceMgr();
  Visitor.TraverseDecl(FuncDecl);
  if (mSrcMgr->getDiagnostics().hasErrorOccurred())
    return false;
  if (!Visitor.hasPragma()) {
    LLVM_DEBUG(dbgs() << "[LOOP SWAPPING]: no pragma found.\n");
    return false;
  }
  LLVM_DEBUG(Visitor.printLocations());
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
  AU.addRequired<ClangDIMemoryMatcherPass>();
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
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper);
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
INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass);
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingProvider);
INITIALIZE_PASS_IN_GROUP_END(ClangLoopSwapping,"clang-l-swap",
  "'for' Loops Swapping (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry());