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
#include "tsar/Analysis/Clang/MemoryMatcher.h"
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
    GlobalsAAWrapperPass, DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
    ClangLoopSwappingServerProvider>;


/// This provider access to function-level analysis results on client.
using ClangLoopSwappingProvider =
  FunctionPassAAProvider<AnalysisSocketImmutableWrapper>;

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
  
  void PrintLocations() {
    LLVM_DEBUG(dbgs() << "'for' loop locations:\n");
    for (auto locs: mRangePairs) {
      for (auto location : locs) {
        SourceLocation begin = location.getBegin();
        SourceLocation end = location.getEnd();
        LLVM_DEBUG(dbgs() << "Begin: ");
        begin.print(dbgs(), mSrcMgr);
        LLVM_DEBUG(dbgs() << "; End: ");
        end.print(dbgs(), mSrcMgr);
        LLVM_DEBUG(dbgs() << '\n');
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
    Passes.add(createMemoryMatcherPass());
  }

  void addAfterPass(legacy::PassManager &Passes) const override {
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
  }
};

}

void ClangLoopSwapping::initializeProviderOnClient(llvm::Module &M) {
  ClangLoopSwappingProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  ClangLoopSwappingProvider::initialize<AnalysisSocketImmutableWrapper>(
      [this](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*mSocket);
      });
  ClangLoopSwappingProvider::initialize<TransformationEnginePass>(
      [this, &M](TransformationEnginePass &Wrapper) {
        Wrapper.setContext(M, mTfmCtx);
      });
  ClangLoopSwappingProvider::initialize<MemoryMatcherImmutableWrapper>(
      [this](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*mMemoryMatcher);
      });
  ClangLoopSwappingProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [this](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*mGlobalsAA);
      });
}

void ClangLoopSwapping::initializeProviderOnServer() {
  ClangLoopSwappingServerProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  auto R = mSocket->getAnalysis<GlobalsAAWrapperPass,
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
  auto &GlobalsAAServer = R->value<GlobalsAAWrapperPass *>()->getResult();
  ClangLoopSwappingServerProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [&GlobalsAAServer](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAAServer);
      });
      
}

void ClangLoopSwapping::releaseMemory() {
  mTfmCtx = nullptr;
  DIDepInfo = nullptr;
  DIAT = nullptr;
  mGlobalOpts = nullptr;
  mGlobalsAA = nullptr;
  mSocket = nullptr;
  mMemoryMatcher = nullptr;
}

bool ClangLoopSwapping::IsNoLoopID(MDNode *LoopID) {
  if (!LoopID || !(LoopID = getLoopID(LoopID))) {
    dbgs() << "[LOOP SWAPPING]: ignore loop without ID.";
    return true;
  }
  return false;
}

std::vector<DIAliasNode *> ClangLoopSwapping::GetLoopNodes(MDNode *LoopID) {
  auto DepItr = DIDepInfo->find(LoopID);
  assert(DepItr != DIDepInfo->end() && "Loop must be analyzed!");
  auto &DIDepSet = DepItr->get<DIDependenceSet>();
  DenseSet<const DIAliasNode *> Coverage;
  auto tmp = mGlobalOpts->IgnoreRedundantMemory;
  auto &tmp2 = *DIAT;
  dbgs() << "Here1\n";
  accessCoverage<bcl::SimpleInserter>(DIDepSet, tmp2, Coverage,
                                      tmp);
  dbgs() << "Here2\n";                                    
  std::vector<DIAliasNode *> nodes;
  for (auto &TS : DIDepSet) {
    auto node = TS.getNode();
    //TS.print(dbgs() << "[LOOP SWAPPING]: current node is: ");
    if (!Coverage.count(node))
      continue;
    MemoryDescriptor Dptr = TS;
    if (Dptr.is<trait::Readonly>()) {
      dbgs() << "[LOOP SWAPPING]: readonly node found (" << TS.getNode() << ")\n";
      continue;
    }
    if (Dptr.is<trait::Reduction>()) {
      dbgs() << "[LOOP SWAPPING]: reduction node found (" << TS.getNode() << "); type: ";
      auto I = TS.begin(), EI = TS.end();
      auto *Red = (**I).get<trait::Reduction>();
      auto Kind = Red->getKind();
      if (!Red || Kind == trait::DIReduction::RK_NoReduction) {
        dbgs() << "No Reduction\n";
      } else if (Kind == trait::DIReduction::RK_Add) {
        dbgs() << "Add\n";
      } else if (Kind == trait::DIReduction::RK_Mult) {
        dbgs() << "Mult\n";
      }
      continue;
    }
    if (Dptr.is<trait::Shared>()) {
      dbgs() << "[LOOP SWAPPING]: shared node found (" << TS.getNode() << ")\n";
      continue;
    }
    if (TS.is<trait::Induction>()) {
      dbgs() << "[LOOP SWAPPING]: induction node found (" << TS.getNode() << ")\n";
      continue;
    }
    nodes.push_back(const_cast<DIAliasNode *>(node));
  }
  return nodes;
}

bool ClangLoopSwapping::IsSwappingAvailable(std::vector<Loop *> loops) {
  Loop *loop0 = loops[0];
  Loop *loop1 = loops[1];
  
  auto *Loop0_ID = loop0->getLoopID();
  auto *Loop1_ID = loop1->getLoopID();
  

  if (IsNoLoopID(Loop0_ID) || IsNoLoopID(Loop1_ID)) {
    dbgs() << "[LOOP SWAPPING]: No loop ID.\n";
    return false;
  }
  std::vector<DIAliasNode *> nodes0 = GetLoopNodes(Loop0_ID);
  std::vector<DIAliasNode *> nodes1 = GetLoopNodes(Loop1_ID);
  SpanningTreeRelation<DIAliasTree *> STR(DIAT);
  for (auto *node0: nodes0) {
    for (auto *node1: nodes1) {
      if (!STR.isUnreachable(node0, node1))
        return false;
    }
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
      dbgs() << "[LOOP SWAPPING]: Failed to swap loops: shared memory\n";
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
  dbgs() << "test\n";
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
  mMemoryMatcher = &getAnalysis<MemoryMatcherImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  mGlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  initializeProviderOnClient(*M);
  initializeProviderOnServer();
  if (!LoadDependenceAnalysisInfo(F))
    return false;

  auto &mLoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  LoopVisitor lv(mTfmCtx->getRewriter(), mLoopInfo);
  lv.TraverseDecl(FuncDecl);
  lv.PrintLocations();
  std::vector<std::vector<SourceRange>> mRangePairs = lv.getRangePairs();
  std::vector<std::vector<Loop *>> mLoopPairs = lv.getLoopPairs();

  //auto &Provider = getAnalysis<ClangLoopSwappingProvider>(F);
  SwapLoops(mRangePairs, mLoopPairs);
  return false;
}

void ClangLoopSwapping::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangLoopSwappingProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<DIDependencyAnalysisPass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<GlobalsAAWrapperPass>();
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
                

INITIALIZE_PROVIDER(ClangLoopSwappingProvider,
                    "clang-loop-swapping-provider",
                    "Loop Swapping (Clang, Provider)")

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
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingProvider);
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass);
INITIALIZE_PASS_DEPENDENCY(GlobalsAAResultImmutableWrapper);
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingServerProvider);
INITIALIZE_PASS_DEPENDENCY(ClangLoopSwappingServerResponse);
INITIALIZE_PASS_IN_GROUP_END(ClangLoopSwapping,"clang-l-swap",
  "'for' Loops Swapping (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry());