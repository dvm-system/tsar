//=== MergeLoops.cpp --- High Level Loops Merger (Clang)---------*- C++ -*-===//
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
// This file defines a pass that makes loop distribution transformation.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/LoopDistribution.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DependenceAnalysis.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Unparse/Utils.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <vector> // TODO: use SmallVector

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "loop-distribution"

namespace {
    class LoopDistributionPassInfo final : public PassGroupInfo {
        void addBeforePass(legacy::PassManager& Passes) const override {
            addImmutableAliasAnalysis(Passes);
            addInitialTransformations(Passes);
            Passes.add(createAnalysisSocketImmutableStorage());
            Passes.add(createDIMemoryTraitPoolStorage());
            Passes.add(createDIMemoryEnvironmentStorage());
            Passes.add(createDIEstimateMemoryPass());
            Passes.add(createDependenceAnalysisWrapperPass());
            Passes.add(createDIMemoryAnalysisServer());
            Passes.add(createAnalysisWaitServerPass());
            Passes.add(createMemoryMatcherPass());
            Passes.add(createAnalysisWaitServerPass());
        }
        void addAfterPass(legacy::PassManager& Passes) const override {
            Passes.add(createAnalysisReleaseServerPass());
            Passes.add(createAnalysisCloseConnectionPass());
        }
    };
}

char LoopDistributionPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(LoopDistributionPass, "loop-distribution",
    "Loop Distribution", false, false,
    TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(LoopDistributionPassInfo)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(LoopDistributionPass, "loop-distribution",
    "Loop Distribution", false, false,
    TransformationQueryManager::getPassRegistry())

namespace {
class ASTVisitor : public RecursiveASTVisitor<ASTVisitor> {
public:
  ASTVisitor(FunctionPass& P, Function& F) {
    mDFRI = &P.getAnalysis<DFRegionInfoPass>().getRegionInfo();
    mTLI = &P.getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    mAliasTree = &P.getAnalysis<EstimateMemoryPass>().getAliasTree();
    mDomTree = &P.getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto& DIEMPass = P.getAnalysis<DIEstimateMemoryPass>();
    assert(DIEMPass.isConstructed() && "Alias tree must be constructed!");
    mDIMInfo = new DIMemoryClientServerInfo(DIEMPass.getAliasTree(), P, F);
    mSTR = new SpanningTreeRelation<const DIAliasTree*>(mDIMInfo->DIAT);
    mCLI = &P.getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo();
    mLM = &P.getAnalysis<LoopMatcherPass>().getMatcher();
    mGO = &P.getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    auto& SocketInfo = P.getAnalysis<AnalysisSocketImmutableWrapper>().get();
    auto& Socket = SocketInfo.getActive()->second;
    auto RF = Socket.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass,
        DependenceAnalysisWrapperPass>(F);
    assert(RF && "Dependence analysis must be available!");
    mDIAT = &RF->value<DIEstimateMemoryPass*>()->getAliasTree();
    mDIDep = &RF->value<DIDependencyAnalysisPass*>()->getDependencies();
    mDepInfo = &RF->value<DependenceAnalysisWrapperPass*>()->getDI();
    auto RM = Socket.getAnalysis<AnalysisClientServerMatcherWrapper>();
    assert(RM && "Client to server IR-matcher must be available!");
    auto Matcher = RM->value<AnalysisClientServerMatcherWrapper*>();
    mGetLoopID = [Matcher](ObjectID ID) {
      auto ServerID = (*Matcher)->getMappedMD(ID);
      return ServerID ? cast<MDNode>(*ServerID) : nullptr;
    };
    mGetInstruction = [Matcher](Instruction* I) {
      return cast<Instruction>((**Matcher)[I]);
    };
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    ForStmt* ForS = dyn_cast<ForStmt>(S);
    if (!ForS)
      return RecursiveASTVisitor::TraverseStmt(S);
    dbgs() << "Loop\n";
    auto Match = mLM->find<AST>(ForS);
    if (Match == mLM->end())
      return false;
    Loop* L = Match->get<IR>();
    auto LoopDepth = L->getLoopDepth();
    auto DFL = dyn_cast<DFRegion>(mDFRI->getRegionFor(L));
    auto CanonItr = mCLI->find_as(DFL);
    if (CanonItr == mCLI->end() || !(**CanonItr).isCanonical() ||
      !(**CanonItr).getLoop())
      return false;
    dbgs() << "Canonical loop\n";
    // TODO: see ParallelLoop.cpp, probably I should use for_each_loop instead.
    MDNode* LoopID = L->getLoopID();
    if (!LoopID || !(LoopID = mGetLoopID(LoopID))) {
      dbgs() << "Ignore loop without ID";
      return false;
    }
    auto DepItr = mDIDep->find(LoopID);
    if (DepItr == mDIDep->end())
      return false;
    auto& DIDepSet = DepItr->get<DIDependenceSet>();
    DenseSet<const DIAliasNode*> Coverage;
    accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
      mGO->IgnoreRedundantMemory);
    for (auto& TS : DIDepSet) {
      if (!Coverage.count(TS.getNode()))
        continue;
      auto& DIMTraitItr = *TS.begin();
      auto* DIMem = DIMTraitItr->getMemory();
      if (DIMTraitItr->is<trait::Flow>()) {
        printDILocationSource(dwarf::DW_LANG_C, *DIMem, dbgs());
        dbgs() << "Flow dependency\n";
      }
      if (DIMTraitItr->is<trait::Anti>()) {
        printDILocationSource(dwarf::DW_LANG_C, *DIMem, dbgs());
        dbgs() << "Antiflow dependency\n";
      }
      if (!DIMTraitItr->is_any<trait::Flow, trait::Anti>())
        continue;
      auto* DINode = DIMem->getAliasNode();
      for (auto BB = L->block_begin(); BB != L->block_end(); ++BB) {
        std::vector<Instruction*> Reads, Writes;
        for_each_memory(**BB, *mTLI, [this, DINode, &Reads, &Writes](Instruction& I,
          MemoryLocation&& Loc, unsigned Idx, AccessInfo R, AccessInfo W) {
          auto EM = mAliasTree->find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto& DL = I.getModule()->getDataLayout();
          auto* DIM = mDIMInfo->findFromClient(
            *EM->getTopLevelParent(), DL, *mDomTree).get<Clone>();
          if (!DIM || mSTR->isUnreachable(DINode, DIM->getAliasNode())) {
            return;
          }
          dbgs() << "Instruction\n";
          if (W != AccessInfo::No) {
            Writes.push_back(&I);
            dbgs() << "Write\n";
          }
          if (R != AccessInfo::No) {
            Reads.push_back(&I);
            dbgs() << "Read\n";
          }
        }, [](Instruction& I, AccessInfo, AccessInfo W) {
          // TODO: fill in
        });
        for (auto Write = Writes.begin(); Write != Writes.end(); ++Write) {
          for (auto Read = Reads.begin(); Read != Reads.end(); ++Read) {
            // TODO: probably true instead of false
            auto Dep = mDepInfo->depends(mGetInstruction(*Write),
                mGetInstruction(*Read), false);
            if (!Dep) continue;
            auto Dir = Dep.get()->getDirection(LoopDepth);
            if (Dir == tsar_impl::Dependence::DVEntry::EQ) {
              // TODO: Probably don't need to do so
              dbgs() << "Ignore loop independent dependence\n";
              continue;
            }
            if (Dir == tsar_impl::Dependence::DVEntry::ALL) {
              dbgs() << "Flow and anti\n";
            }
            else if (Dep.get()->isFlow())
              if (Dir == tsar_impl::Dependence::DVEntry::LT ||
                  Dir == tsar_impl::Dependence::DVEntry::LE) {
                dbgs() << "Flow\n";
              } else {
                dbgs() << "Anti\n";
              }
            else if (Dep.get()->isAnti())
              if (Dir == tsar_impl::Dependence::DVEntry::LT ||
                  Dir == tsar_impl::Dependence::DVEntry::LE) {
                dbgs() << "Anti\n";
              }
              else {
                dbgs() << "Flow\n";
              }
          }
        }
      }
      //auto *Alias = DIMem.getAliasNode();
    }
    //
    bool PrevIsInsideLoop = mIsInsideLoop;
    mIsInsideLoop = true;
    bool result = RecursiveASTVisitor::TraverseStmt(ForS->getBody());
    mIsInsideLoop = PrevIsInsideLoop;
    return result;
  }

private:
  DFRegionInfo* mDFRI;
  TargetLibraryInfo* mTLI;
  AliasTree* mAliasTree;
  DominatorTree* mDomTree;
  DIMemoryClientServerInfo* mDIMInfo;
  SpanningTreeRelation<const DIAliasTree*>* mSTR;
  const CanonicalLoopSet* mCLI;
  const LoopMatcherPass::LoopMatcher* mLM;
  const GlobalOptions* mGO;
  DIAliasTree* mDIAT;
  DIDependencInfo* mDIDep;
  DependenceInfo* mDepInfo;
  std::function<ObjectID(ObjectID)> mGetLoopID;
  std::function<Instruction* (Instruction*)> mGetInstruction;
  bool mIsInsideLoop = false;
};
}

bool LoopDistributionPass::runOnFunction(Function& F) {
  auto M = F.getParent();
  auto& TfmInfo = getAnalysis<TransformationEnginePass>();
  if (!TfmInfo)
    return false;
  auto TfmCtx = TfmInfo->getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  /*
  auto& RgnInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto& TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto& AliasTree = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto& DIEMPass = getAnalysis<DIEstimateMemoryPass>();
  assert(DIEMPass.isConstructed() && "Alias tree must be constructed!");
  auto& DIMInfo = DIMemoryClientServerInfo(DIEMPass.getAliasTree(), *this, F);
  auto& DomTree = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto& SocketInfo = getAnalysis<AnalysisSocketImmutableWrapper>().get();
  auto& Socket = SocketInfo.getActive()->second;
  auto RF =
      Socket.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass, DependenceAnalysisWrapperPass>(F);
  assert(RF && "Dependence analysis must be available!");
  auto& DIAT = RF->value<DIEstimateMemoryPass*>()->getAliasTree();
  auto& DIDepInfo = RF->value<DIDependencyAnalysisPass*>()->getDependencies();
  auto& DepInfo = RF->value<DependenceAnalysisWrapperPass*>()->getDI();

  auto RM = Socket.getAnalysis<AnalysisClientServerMatcherWrapper>();
  assert(RM && "Client to server IR-matcher must be available!");
  auto Matcher = RM->value<AnalysisClientServerMatcherWrapper*>();
  auto& GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>()
      .getOptions();
  auto& CLI = getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto& LoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  */
  ASTVisitor LoopVisitor(*this, F);
  LoopVisitor.TraverseDecl(FuncDecl);
  return false;
}

void LoopDistributionPass::getAnalysisUsage(AnalysisUsage& AU) const {
    AU.addRequired<TransformationEnginePass>();
    AU.addRequired<DFRegionInfoPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<EstimateMemoryPass>();
    AU.addRequired<DIEstimateMemoryPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<DependenceAnalysisWrapperPass>();
    AU.addRequired<CanonicalLoopPass>();
    AU.addRequired<LoopMatcherPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<GlobalOptionsImmutableWrapper>();
    AU.addRequired<AnalysisSocketImmutableWrapper>();
    AU.setPreservesAll();
}

FunctionPass* llvm::createLoopDistributionPass() {
    return new LoopDistributionPass();
}