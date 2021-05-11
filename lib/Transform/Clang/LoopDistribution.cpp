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
#include "tsar/Analysis/Clang/ExpressionMatcher.h"
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
#include <llvm/ADT/Optional.h>
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
        void addBeforePass(legacy::PassManager &Passes) const override {
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
        void addAfterPass(legacy::PassManager &Passes) const override {
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
INITIALIZE_PASS_DEPENDENCY(ClangExprMatcherPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(LoopDistributionPass, "loop-distribution",
    "Loop Distribution", false, false,
    TransformationQueryManager::getPassRegistry())

namespace {

  typedef std::vector<Instruction *> DependencyInstructionVector;
  typedef std::set<Instruction *> SplitInstructionVector;

class ASTVisitor : public RecursiveASTVisitor<ASTVisitor> {
public:
  ASTVisitor(FunctionPass& Pass, Function& Function,
      ClangTransformationContext &TransformationContext) {
    mDFRegion = &Pass.getAnalysis<DFRegionInfoPass>().getRegionInfo();
    mTargetLibrary = &Pass.getAnalysis<TargetLibraryInfoWrapperPass>()
        .getTLI(Function);
    mAliasTree = &Pass.getAnalysis<EstimateMemoryPass>()
        .getAliasTree();
    mDominatorTree = &Pass.getAnalysis<DominatorTreeWrapperPass>()
        .getDomTree();
    auto& DIEMPass = Pass.getAnalysis<DIEstimateMemoryPass>();
    assert(DIEMPass.isConstructed() && "Alias tree must be constructed!");
    mServerDIMemory = new DIMemoryClientServerInfo(
        DIEMPass.getAliasTree(), Pass, Function);
    mSpanningTreeRelation = new SpanningTreeRelation<const DIAliasTree *>(
        mServerDIMemory->DIAT);
    mCanonicalLoop = &Pass.getAnalysis<CanonicalLoopPass>()
        .getCanonicalLoopInfo();
    mExpressionMatcher = &Pass.getAnalysis<ClangExprMatcherPass>().getMatcher();
    mLoopMatcher = &Pass.getAnalysis<LoopMatcherPass>()
        .getMatcher();
    mGlobalOptions = &Pass.getAnalysis<GlobalOptionsImmutableWrapper>()
        .getOptions();
    mSourceManager = &TransformationContext.getRewriter().getSourceMgr();
    mASTContext = &TransformationContext.getContext();
    auto& SocketInfo = Pass.getAnalysis<AnalysisSocketImmutableWrapper>().get();
    auto& Socket = SocketInfo.getActive()->second;
    auto RF = Socket.getAnalysis<DIEstimateMemoryPass,
        DIDependencyAnalysisPass, DependenceAnalysisWrapperPass>(Function);
    assert(RF && "Dependence analysis must be available!");
    mDIAliasTree = &RF->value<DIEstimateMemoryPass *>()->getAliasTree();
    mDIDependency = &RF->value<DIDependencyAnalysisPass *>()->getDependencies();
    mDependence = &RF->value<DependenceAnalysisWrapperPass *>()->getDI();
    auto RM = Socket.getAnalysis<AnalysisClientServerMatcherWrapper>();
    assert(RM && "Client to server IR-matcher must be available!");
    auto *Matcher = RM->value<AnalysisClientServerMatcherWrapper *>();
    mGetServerLoopIdFunction = [Matcher](ObjectID ID) {
      auto ServerID = (*Matcher)->getMappedMD(ID);
      return ServerID
          ? cast<MDNode>(*ServerID)
          : nullptr;
    };
    mGetInstructionFunction = [Matcher](Instruction *I) {
      return cast<Instruction>((**Matcher)[I]);
    };
  }

  [[maybe_unused]]
  bool TraverseStmt(Stmt *Statement) {
    if (!Statement) {
      return RecursiveASTVisitor::TraverseStmt(Statement);
    }
    auto *ForStatement = dyn_cast<ForStmt>(Statement);
    if (!ForStatement) {
      return RecursiveASTVisitor::TraverseStmt(Statement);
    }

    const auto LoopMatch = mLoopMatcher->find<AST>(ForStatement);
    if (LoopMatch == mLoopMatcher->end()) {
      return false;
    }

    auto *const Loop = LoopMatch->get<IR>();
    auto const OptionalDIDependenceSet = getDIDependenceSet(Loop);
    if (!OptionalDIDependenceSet.hasValue()) {
      return false;
    }

    LLVM_DEBUG(dbgs() << "Found canonical loop with dependencies at ";
               ForStatement->getBeginLoc().print(dbgs(), *mSourceManager);
               dbgs() << "\n";);
    auto &DIDependenceSet = OptionalDIDependenceSet.getValue();
    auto Splits = getLoopSplits(Loop, DIDependenceSet);
    LLVM_DEBUG(
      dbgs() << "Found splits:\n";
      for (const auto *Split : Splits) {
        Split->dump();
      }
    );
    processSplits(Splits);

    const auto PrevIsInsideLoop = mIsInsideLoop;
    mIsInsideLoop = true;
    const auto Result = RecursiveASTVisitor::TraverseStmt(ForStatement->getBody());
    mIsInsideLoop = PrevIsInsideLoop;
    return Result;
  }

private:
  Optional<DIDependenceSet> getDIDependenceSet(Loop *Loop) const {
    auto *DFLoopRegion = dyn_cast<DFRegion>(mDFRegion->getRegionFor(Loop));
    if (const auto &CanonicalLoopItr = mCanonicalLoop->find_as(DFLoopRegion);
        CanonicalLoopItr == mCanonicalLoop->end() ||
        !(**CanonicalLoopItr).isCanonical() || !(**CanonicalLoopItr).getLoop()) {
      return None;
    }

    auto *LoopID = Loop->getLoopID();
    if (!LoopID) {
      LLVM_DEBUG(dbgs() << "Ignored loop without ID\n");
      return None;
    }
    LoopID = mGetServerLoopIdFunction(LoopID);
    if (!LoopID) {
      LLVM_DEBUG(dbgs() << "Ignored loop collapsed on the pass server\n");
      return None;
    }

    const auto DependencyItr = mDIDependency->find(LoopID);
    if (DependencyItr == mDIDependency->end()) {
      return None;
    }

    return DependencyItr->get<DIDependenceSet>();
  }

  SplitInstructionVector getLoopSplits(
      Loop *Loop, const DIDependenceSet &DIDependenceSet) {
    DenseSet<const DIAliasNode *> Coverage;
    accessCoverage<bcl::SimpleInserter>(DIDependenceSet, *mDIAliasTree,
        Coverage, mGlobalOptions->IgnoreRedundantMemory);
    DependencyInstructionVector DependencyReads, DependencyWrites;
    for (auto &AliasTrait : DIDependenceSet) {
      if (!Coverage.count(AliasTrait.getNode())) {
        continue;
      }

      auto &DIMemoryTraitItr = *AliasTrait.begin();
      if (!DIMemoryTraitItr->is_any<trait::Flow, trait::Anti>()) {
        continue;
      }

      const auto *DIMemory = DIMemoryTraitItr->getMemory();
      LLVM_DEBUG(
          if (DIMemoryTraitItr->is<trait::Flow>()) {
            printDILocationSource(dwarf::DW_LANG_C, *DIMemory, dbgs());
            dbgs() << "Flow dependency\n";
          } if (DIMemoryTraitItr->is<trait::Anti>()) {
            printDILocationSource(dwarf::DW_LANG_C, *DIMemory, dbgs());
            dbgs() << "Antiflow dependency\n";
          });

      const auto *DINode = DIMemory->getAliasNode();
      for (const auto &BasicBlock : Loop->blocks()) {
        // Get all reads and writes of memory leading to dependencies
        for_each_memory(
            *BasicBlock, *mTargetLibrary,
            [this, DINode, &DependencyReads, &DependencyWrites](
                Instruction &I, MemoryLocation &&Loc, unsigned, AccessInfo R,
                AccessInfo W) {
              auto *EM = mAliasTree->find(Loc);
              assert(EM && "Estimate memory location must not be null!");
              const auto &DL = I.getModule()->getDataLayout();
              auto *DIM = mServerDIMemory
                              ->findFromClient(*EM->getTopLevelParent(), DL,
                                               *mDominatorTree)
                              .get<Clone>();
              if (!DIM || mSpanningTreeRelation->isUnreachable(
                              DINode, DIM->getAliasNode())) {
                return;
              }
              if (W != AccessInfo::No) {
                DependencyWrites.push_back(&I);
                LLVM_DEBUG(dbgs() << "Write "; I.dump());
              }
              if (R != AccessInfo::No) {
                DependencyReads.push_back(&I);
                LLVM_DEBUG(dbgs() << "Read "; I.dump());
              }
            },
            [](Instruction &I, AccessInfo, AccessInfo W) {
              // TODO: Fill in
            }
        );
      }
    }
    // Try to get write instructions after which should be split
    return getLoopSplits(Loop, DependencyReads, DependencyWrites);
  }

  SplitInstructionVector getLoopSplits(
      Loop *Loop, const DependencyInstructionVector &Reads,
                     const DependencyInstructionVector &Writes) const {
    const auto LoopDepth = Loop->getLoopDepth();
    SplitInstructionVector Splits(Writes.begin(), Writes.end());
    for (const auto &Write : Writes) {
      for (const auto &Read : Reads) {
        // TODO: Probably true instead of false
        const auto Dependence =
            mDependence->depends(mGetInstructionFunction(Write),
                                 mGetInstructionFunction(Read), false);
        if (!Dependence) {
          continue;
        }

        const auto Direction = Dependence->getDirection(LoopDepth);
        if (Direction == tsar_impl::Dependence::DVEntry::EQ) {
          LLVM_DEBUG(dbgs() << "Ignore loop independent dependence\n");
          continue;
        }

        // Get direction of the dependency
        auto Flow = false, Anti = false;
        if (Direction == tsar_impl::Dependence::DVEntry::ALL) {
          Flow = true;
          Anti = true;
        } else if (Dependence->isFlow()) {
          if (Direction == tsar_impl::Dependence::DVEntry::LT ||
              Direction == tsar_impl::Dependence::DVEntry::LE) {
            Flow = true;
          } else {
            Anti = true;
          }
        } else if (Dependence->isAnti()) {
          if (Direction == tsar_impl::Dependence::DVEntry::LT ||
              Direction == tsar_impl::Dependence::DVEntry::LE) {
            Anti = true;
          } else {
            Flow = true;
          }
        }

        const auto WriteBeforeRead = Write->comesBefore(Read);
        // If this is bad instructions dependency, can't split between them
        if (!(WriteBeforeRead && Anti || !WriteBeforeRead && Flow)) {
          continue;
        }

        for (const auto &Split : Writes) {
          if (!Splits.count(Split)) {
            continue;
          }
          if (Split->comesBefore(Write) && Read->comesBefore(Split) ||
              Split->comesBefore(Read) && Write->comesBefore(Split)) {
            Splits.erase(Split);
          }
        }
      }
    }
    return Splits;
  }

  void processSplits(const SplitInstructionVector &Splits) const {
    for (auto *Split : Splits) {
      const auto &ExpressionMatcherItr = mExpressionMatcher->find<IR>(Split);
      if (ExpressionMatcherItr == mExpressionMatcher->end()) {
        LLVM_DEBUG(dbgs() << "Store instruction can't be bound to AST node: ";
                   Split->dump(););
        continue;
      }
      const auto &SplitStatement = ExpressionMatcherItr->get<AST>();
      SplitStatement.dump(dbgs(), *mASTContext);
    }
  }

private:
  DFRegionInfo *mDFRegion;
  TargetLibraryInfo *mTargetLibrary;
  AliasTree *mAliasTree;
  DominatorTree *mDominatorTree;
  DIMemoryClientServerInfo *mServerDIMemory;
  SpanningTreeRelation<const DIAliasTree *> *mSpanningTreeRelation;
  const CanonicalLoopSet *mCanonicalLoop;
  const ClangExprMatcherPass::ExprMatcher *mExpressionMatcher;
  const LoopMatcherPass::LoopMatcher *mLoopMatcher;
  const GlobalOptions *mGlobalOptions;
  const SourceManager *mSourceManager;
  const ASTContext *mASTContext;
  DIAliasTree *mDIAliasTree;
  DIDependencInfo *mDIDependency;
  DependenceInfo *mDependence;
  std::function<ObjectID(ObjectID)> mGetServerLoopIdFunction;
  std::function<Instruction * (Instruction *)> mGetInstructionFunction;
  bool mIsInsideLoop = false;
};
}

bool LoopDistributionPass::runOnFunction(Function& Function) {
  auto *Module = Function.getParent();
  auto& TransformationInfo =
      getAnalysis<TransformationEnginePass>();
  if (!TransformationInfo) {
    return false;
  }
  auto *TransformationContext = TransformationInfo->getContext(*Module);
  if (!TransformationContext || !TransformationContext->hasInstance()) {
    return false;
  }
  auto *FunctionDecl =
      TransformationContext->getDeclForMangledName(Function.getName());
  if (!FunctionDecl) {
    return false;
  }
  ASTVisitor LoopVisitor(*this, Function, *TransformationContext);
  LoopVisitor.TraverseDecl(FunctionDecl);
  return false;
}

void LoopDistributionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<DependenceAnalysisWrapperPass>();
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangExprMatcherPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createLoopDistributionPass() {
    return new LoopDistributionPass();
}