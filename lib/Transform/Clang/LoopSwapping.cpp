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

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Transform/Clang/LoopSwapping.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/SourceLocationTraverse.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassAAProvider.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <stack>


using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-l-swap"

char ClangLoopSwapping::ID = 0;

namespace llvm {

static void initializeClangLoopSwappingServerPass(PassRegistry &);
static void initializeClangLoopSwappingServerResponsePass(PassRegistry &);

}

namespace {

/// This provides access to function-level analysis results on server.
using ClangLoopSwappingServerProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass>;

/// List of responses available from server (client may request corresponding
/// analysis, in case of provider all analysis related to a provider may
/// be requested separately).
using ClangLoopSwappingServerResponse = AnalysisResponsePass<
    DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
    ClangLoopSwappingServerProvider>;

/// This analysis server performs transformation-based analysis.
class ClangLoopSwappingServer final : public AnalysisServer {
public:
  static char ID;
  ClangLoopSwappingServer() : AnalysisServer(ID) {
    initializeClangLoopSwappingServerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AnalysisServer::getAnalysisUsage(AU);
    ClientToServerMemory::getAnalysisUsage(AU);
    AU.addRequired<GlobalOptionsImmutableWrapper>();
  }

  void prepareToClone(llvm::Module &ClientM,
                      ValueToValueMapTy &ClientToServer) override {
    ClientToServerMemory::prepareToClone(ClientM, ClientToServer);
  }

  void initializeServer(llvm::Module &CM, llvm::Module &SM, ValueToValueMapTy &CToS,
                        legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>();
    PM.add(createGlobalOptionsImmutableWrapper(&GO.getOptions()));
    PM.add(createDIMemoryTraitPoolStorage());
    ClientToServerMemory::initializeServer(*this, CM, SM, CToS, PM);
  }

  void addServerPasses(llvm::Module &M, legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    addImmutableAliasAnalysis(PM);
    addBeforeTfmAnalysis(PM);
    addAfterSROAAnalysis(GO, M.getDataLayout(), PM);
    addAfterLoopRotateAnalysis(PM);
    PM.add(createVerifierPass());
    PM.add(new ClangLoopSwappingServerResponse);
  }

  void prepareToClose(legacy::PassManager &PM) override {
    ClientToServerMemory::prepareToClose(PM);
  }
};

class LoopVisitor : public RecursiveASTVisitor<LoopVisitor> {
public:
  LoopVisitor(Rewriter &Rewr, const LoopMatcherPass::LoopMatcher &LM) :
    mRewriter(Rewr),
    mLoopInfo(LM),
    mSrcMgr(Rewr.getSourceMgr()),
    mLangOpts(Rewr.getLangOpts()),
    mIsInScope(false), mForStack(nullptr)
    {}

  void EnterInScope() {
    mForLocations.clear();
    mForIRs.clear();
  	mIsInScope = true;
  }

  void ExitFromScope() {
    mForLocations.clear();
    mForIRs.clear();
  	mIsInScope = false;
  }

  bool IsInScope() {
  	return mIsInScope;
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
          pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts, ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getLocStart(),
              diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getLocStart(),
              diag::warn_remove_directive_in_include);
          else
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getLocStart(),
              diag::warn_remove_directive);
        Rewriter::RewriteOptions RemoveEmptyLine;
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
        /// set to true then removing (in RewriterBuffer) works incorrect.
        RemoveEmptyLine.RemoveLineIfEmpty = false;
        for (auto SR : ToRemove)
          mRewriter.RemoveText(SR, RemoveEmptyLine);
        EnterInScope();
      }
      return true;
  	}
  	auto Res = RecursiveASTVisitor::TraverseStmt(S);
    return Res;
  }

  bool TraverseCompoundStmt(CompoundStmt *S) {
    if (IsInScope() && mForStack == nullptr) {
      mForStack = new std::stack<ForStmt *>;
      mForLocations.clear();
      mForIRs.clear();
      auto Res = RecursiveASTVisitor::TraverseCompoundStmt(S);
      while (!mForStack->empty())
        mForStack->pop();
      delete mForStack;
      mForStack = nullptr;
      mRangePairs.push_back(mForLocations);
      mLoopPairs.push_back(mForIRs);
      ExitFromScope();
      return Res;
    }
  	return RecursiveASTVisitor::TraverseCompoundStmt(S);
  }

  bool TraverseForStmt(ForStmt *S) {
    if (IsInScope() && mForStack != nullptr) {
      if (mForStack->empty()) {
	      auto Match = mLoopInfo.find<AST>(S);
        if (Match != mLoopInfo.end()) {
          Loop *loop = Match->get<IR>();
          mForIRs.push_back(loop);
          SourceRange range(S->getBeginLoc(), S->getEndLoc());
          mForLocations.push_back(range);
        }
	      mForStack->push(S);
	      auto Res = RecursiveASTVisitor::TraverseForStmt(S);
	      mForStack->pop();
	      return Res;
	    }
	  }
    return RecursiveASTVisitor::TraverseForStmt(S);
  }

  std::vector<std::vector<SourceRange>> getRangePairs() {
    return mRangePairs;
  }

  std::vector<std::vector<Loop *>> getLoopPairs() {
    return mLoopPairs;
  }

  size_t getLoopsCount() {
    return mRangePairs.size();
  }
  
  void PrintLocations() {
    dbgs() << "[LOOP SWAPPING]: 'for' loop locations:\n";
    unsigned int loopNumber = 0;
    for (auto locs: mRangePairs) {
      for (auto location : locs) {
        dbgs() << "Loop #" << loopNumber << ":\n";
        SourceLocation begin = location.getBegin();
        SourceLocation end = location.getEnd();
        dbgs() << "\tBegin: ";
        begin.print(dbgs(), mSrcMgr);
        dbgs() << "\n\tEnd: ";
        end.print(dbgs(), mSrcMgr);
        dbgs() << '\n';
        loopNumber++;
      }
    }
  }

private:
  Rewriter &mRewriter;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  bool mIsInScope;
  const LoopMatcherPass::LoopMatcher &mLoopInfo;

  SmallVector<Stmt *, 1> mClauses;
  std::vector<SourceRange> mForLocations;
  std::vector<Loop *> mForIRs;
  std::vector<std::vector<SourceRange>> mRangePairs;
  std::vector<std::vector<Loop *>> mLoopPairs;
  std::stack<ForStmt *> *mForStack;
};

class ClangLoopSwappingInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override {
    addImmutableAliasAnalysis(Passes);
    addInitialTransformations(Passes);
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(new ClangLoopSwappingServer);
    Passes.add(createAnalysisWaitServerPass());
  }

  void addAfterPass(legacy::PassManager &Passes) const override {
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
  }
};

}

void ClangLoopSwapping::initializeProviderOnServer() {
  ClangLoopSwappingServerProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  auto R = mSocket->getAnalysis<
      DIMemoryEnvironmentWrapper, DIMemoryTraitPoolWrapper>();
  assert(R && "Immutable passes must be available on server!");
  auto *DIMEnvServer = R->value<DIMemoryEnvironmentWrapper *>();
  ClangLoopSwappingServerProvider::initialize<DIMemoryEnvironmentWrapper>(
      [DIMEnvServer](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(**DIMEnvServer);
      });
  auto *DIMTraitPoolServer = R->value<DIMemoryTraitPoolWrapper *>();
  ClangLoopSwappingServerProvider::initialize<DIMemoryTraitPoolWrapper>(
      [DIMTraitPoolServer](DIMemoryTraitPoolWrapper &Wrapper) {
        Wrapper.set(**DIMTraitPoolServer);
      });
      
}

std::vector<const DIAliasTrait *> ClangLoopSwapping::GetLoopTraits(MDNode *LoopID) {
  auto DepItr = DIDepInfo->find(LoopID);
  assert(DepItr != DIDepInfo->end() && "Loop must be analyzed!");
  auto &DIDepSet = DepItr->get<DIDependenceSet>();
  DenseSet<const DIAliasNode *> Coverage;
  accessCoverage<bcl::SimpleInserter>(DIDepSet, *DIAT, Coverage,
                                      mGlobalOpts->IgnoreRedundantMemory);
  std::vector<const DIAliasTrait *> traits;
  for (auto &TS : DIDepSet) {
    auto node = TS.getNode();
    if (!Coverage.count(node))
      continue;
    traits.push_back(&TS);
  }
  return traits;
}

bool ClangLoopSwapping::HasSameReductionKind(std::vector<const DIAliasTrait *> &traits0,
                        std::vector<const DIAliasTrait *> &traits1,
                        SpanningTreeRelation<DIAliasTree *> &STR) {
  for (auto TS0: traits0) {
    auto *node0 = const_cast<DIAliasNode *>(TS0->getNode());
    MemoryDescriptor Dptr0 = *TS0;
    for (auto TS1: traits1) {
      auto *node1 = const_cast<DIAliasNode *>(TS1->getNode());
      MemoryDescriptor Dptr1 = *TS1;
      if (STR.isEqual(node0, node1) && Dptr0.is<trait::Reduction>() &&
          Dptr1.is<trait::Reduction>()) {
        dbgs() << "[LOOP SWAPPING]: same reduction nodes;\n";
        auto I0 = TS0->begin(), I1 = TS1->begin();
        auto *Red0 = (**I0).get<trait::Reduction>();
        auto *Red1 = (**I1).get<trait::Reduction>();
        auto Kind0 = Red0->getKind(), Kind1 = Red1->getKind();
        if (!Red0 || !Red1 || Kind0 == trait::DIReduction::RK_NoReduction ||
            Kind1 == trait::DIReduction::RK_NoReduction) {
          dbgs() << "[LOOP SWAPPING]: No Reduction\n";
          return true;
        }
        if (Kind0 != Kind1)
          return false;
      }
    }
  }
  return true;
}

bool ClangLoopSwapping::HasTrueOrAntiDependence(std::vector<const DIAliasTrait *> &traits0,
                        std::vector<const DIAliasTrait *> &traits1,
                        SpanningTreeRelation<DIAliasTree *> &STR) {
  for (auto TS0: traits0) {
    auto *node0 = const_cast<DIAliasNode *>(TS0->getNode());
    MemoryDescriptor Dptr0 = *TS0;
    for (auto TS1: traits1) {
      auto *node1 = const_cast<DIAliasNode *>(TS1->getNode());
      MemoryDescriptor Dptr1 = *TS1;
      if (STR.isEqual(node0, node1)) {
        if (Dptr0.is<trait::Readonly>() && !Dptr1.is<trait::Readonly>()) {
          dbgs() << "[LOOP SWAPPING]: anti dependence found;\n";
          return true;
        } else if (Dptr1.is<trait::Readonly>() && !Dptr0.is<trait::Readonly>()) {
          dbgs() << "[LOOP SWAPPING]: true dependence found;\n";
          return true;
        }
      }
    }
  }
  return false;
}

bool ClangLoopSwapping::IsSwappingAvailable(std::vector<Loop *> loops) {
  Loop *loop0 = loops[0];
  Loop *loop1 = loops[1];
  
  auto *Loop0_ID = loop0->getLoopID();
  auto *Loop1_ID = loop1->getLoopID();
  
  if (!Loop0_ID || !(Loop0_ID = getLoopID(Loop0_ID))) {
    dbgs() << "[LOOP SWAPPING]: ignore loop without ID (loop 0).";
    return false;
  }

  if (!Loop1_ID || !(Loop1_ID = getLoopID(Loop1_ID))) {
    dbgs() << "[LOOP SWAPPING]: ignore loop without ID (loop 1).";
    return false;
  }

  std::vector<const DIAliasTrait *> traits0 = GetLoopTraits(Loop0_ID);
  std::vector<const DIAliasTrait *> traits1 = GetLoopTraits(Loop1_ID);

  SpanningTreeRelation<DIAliasTree *> STR(DIAT);

  if (!HasSameReductionKind(traits0, traits1, STR)) {
    dbgs() << "[LOOP SWAPPING]: Failed to swap loops: different reduction kinds.";
    return false;
  }
  if (HasTrueOrAntiDependence(traits0, traits1, STR)) {
    dbgs() << "[LOOP SWAPPING]: Failed to swap loops: true or anti depencende.";
    return false;
  }
  return true;
}

void ClangLoopSwapping::SwapLoops(const std::vector<std::vector<SourceRange>> &mRangePairs,
                                  const std::vector<std::vector<Loop *>> &mLoopPairs) {
  Rewriter &mRewriter = mTfmCtx->getRewriter();
  for (int i = 0; i < mRangePairs.size(); i++) {
    std::vector<SourceRange> ranges = mRangePairs[i];
    std::vector<Loop *> loops = mLoopPairs[i];
    if (ranges.size() < 2) {
      dbgs() << "[LOOP SWAPPING]: Too few loops for swap. Ignore.\n";
      continue;
    }
    if (ranges.size() > 2) {
      dbgs() << "[LOOP SWAPPING]: Too many loops for swap. Ignore additional loops.\n";
    }
    if (IsSwappingAvailable(loops)) {
      SourceRange first = ranges[0];
      SourceRange second = ranges[1];
      std::string first_loop = mRewriter.getRewrittenText(first);
      std::string second_loop = mRewriter.getRewrittenText(second);
      mRewriter.ReplaceText(first, second_loop);
      mRewriter.ReplaceText(second, first_loop);
    } else {
      dbgs() << " Loops will not be swapped.\n";
    }
  }
}

bool ClangLoopSwapping::LoadDependenceAnalysisInfo(Function &F) {
  getLoopID = [](ObjectID ID) { return ID; };
  if (auto *Socket = getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>()) {
    if (auto R = (*Socket)->getAnalysis<AnalysisClientServerMatcherWrapper>()) {
      auto *Matcher = R->value<AnalysisClientServerMatcherWrapper *>();
      getLoopID = [Matcher](ObjectID ID) {
        auto ServerID = (*Matcher)->getMappedMD(ID);
        return ServerID ? cast<MDNode>(*ServerID) : nullptr;
      };
      if (auto R =(*Socket)->getAnalysis<
            DIEstimateMemoryPass, DIDependencyAnalysisPass>(F)) {
        DIAT = &R->value<DIEstimateMemoryPass *>()->getAliasTree();
        DIDepInfo = &R->value<DIDependencyAnalysisPass *>()->getDependencies();
      }
    }
  }
  if (!DIAT || !DIDepInfo) {
    dbgs() << "[LOOP SWAPPING]: analysis server is not available\n";
    if (auto *P = getAnalysisIfAvailable<DIEstimateMemoryPass>())
      DIAT = &P->getAliasTree();
    else
      return false;
    if (auto *P = getAnalysisIfAvailable<DIDependencyAnalysisPass>())
      DIDepInfo = &P->getDependencies();
    else
      return false;
    dbgs() << "[LOOP SWAPPING]: use dependence analysis from client\n";
  }
  return true;
}

bool ClangLoopSwapping::runOnFunction(Function &F) {
  mFunction = &F;
  dbgs() << "\n[LOOP SWAPPING]: Function '" << F.getName() << "'.\n";
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
  mSocket = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  initializeProviderOnServer();
  if (!LoadDependenceAnalysisInfo(F))
    return false;
  auto &mLoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  LoopVisitor lv(mTfmCtx->getRewriter(), mLoopInfo);
  lv.TraverseDecl(FuncDecl);
  if (lv.getLoopsCount() == 0) {
    dbgs() << "[LOOP SWAPPING]: no loops found.\n";
    return false;
  }
  lv.PrintLocations();
  std::vector<std::vector<SourceRange>> mRangePairs = lv.getRangePairs();
  std::vector<std::vector<Loop *>> mLoopPairs = lv.getLoopPairs();
  SwapLoops(mRangePairs, mLoopPairs);
  return false;
}

void ClangLoopSwapping::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<DIDependencyAnalysisPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createClangLoopSwapping() {
  return new ClangLoopSwapping();
}

INITIALIZE_PROVIDER(ClangLoopSwappingServerProvider, "clang-loop-swapping-server-provider",
                    "Loop Swapping (Clang, Server, Provider)")

template <> char ClangLoopSwappingServerResponse::ID = 0;
INITIALIZE_PASS(ClangLoopSwappingServerResponse, "clang-loop-swapping-response",
                "Loop Swapping (Clang, Server, Response)", true,
                false)

char ClangLoopSwappingServer::ID = 0;
INITIALIZE_PASS(ClangLoopSwappingServer, "clang-loop-swapping-server",
                "Loop Swapping (Clang, Server)", false, false)
                
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopSwapping,"clang-l-swap",
  "'for' Loops Swapping (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry());
INITIALIZE_PASS_IN_GROUP_INFO(ClangLoopSwappingInfo);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass);
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass);
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass);
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass);
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass);
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper);
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingServerProvider);
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingServerResponse);
INITIALIZE_PASS_IN_GROUP_END(ClangLoopSwapping,"clang-l-swap",
  "'for' Loops Swapping (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry());