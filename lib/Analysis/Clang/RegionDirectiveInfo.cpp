//=== RegionDirectiveInfo.cpp - Source-level Region Directive  --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file implement passes to collect '#pragma spf region' directives.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/ExpressionMatcher.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/PassProvider.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Utils.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/InitializePasses.h>
#include <stack>

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-region"

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
using ClangRegionCollectorProvider =
    FunctionPassProvider<LoopMatcherPass, ClangExprMatcherPass,
                         LoopInfoWrapperPass>;

class RegionDirectiveVisitor
    : public RecursiveASTVisitor<RegionDirectiveVisitor> {
public:
  RegionDirectiveVisitor(Rewriter &R, const LoopMatcherPass::LoopMatcher &Loops,
                         const ClangExprMatcherPass::ExprMatcher &Exprs,
                         OptimizationRegionInfo &Regions)
      : mSrcMgr(R.getSourceMgr()), mLangOpts(R.getLangOpts()), mRewriter(R),
        mLoops(Loops), mExprs(Exprs), mRegions(Regions) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    Pragma P(*S);
    if (P) {
      if (P.getDirectiveId() != DirectiveId::Region)
        return true;
      if (mNewPragma) {
        toDiag(mSrcMgr.getDiagnostics(), mNewPragma->getBeginLoc(),
          tsar::diag::warn_unexpected_directive);
        mActiveRegions.resize(mActiveRegions.size() - mNewActiveRegions);
      }
      SmallVector<Stmt *, 1> Clauses;
      mNewActiveRegions = 0;
      if (findClause(P, ClauseId::RegionName, Clauses)) {
        for (auto *RawClause : Clauses) {
          auto C = Pragma::clause(&RawClause);
          for (auto NameStmt : C) {
            auto Cast = dyn_cast<ImplicitCastExpr>(NameStmt);
            // In case of C there will be ImplicitCastExpr,
            // however in case of C++ it will be omitted.
            auto LiteralStmt = Cast ? *Cast->child_begin() : NameStmt;
            auto Name = cast<clang::StringLiteral>(LiteralStmt)->getString();
            mActiveRegions.push_back(mRegions.insert(Name).first);
            ++mNewActiveRegions;
          }
        }
      } else {
        mActiveRegions.push_back(mRegions.insert("").first);
        ++mNewActiveRegions;
      }
      mNewPragma = S;
      return true;
    }
    if (!(isa<CompoundStmt>(S) || isa<ForStmt>(S) || isa<WhileStmt>(S) ||
          isa<DoStmt>(S))) {
      if (mNewPragma) {
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
          tsar::diag::warn_unexpected_directive);
        mActiveRegions.resize(mActiveRegions.size() - mNewActiveRegions);
        mNewPragma = nullptr;
      }
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    if (mActiveRegions.empty())
      return RecursiveASTVisitor::TraverseStmt(S);
    auto StashActivePragmaCount = mActiveRegions.size() - mNewActiveRegions;
    mNewPragma = nullptr;
    mNewActiveRegions = 0;
    LLVM_DEBUG(
        dbgs() << "[OPT REGION]: number of active regions on the level entry "
               << mActiveRegions.size() << "\n");
    if (isa<ForStmt>(S) || isa<WhileStmt>(S) || isa<DoStmt>(S)) {
      auto MatchItr = mLoops.find<AST>(S);
      if (MatchItr == mLoops.end()) {
        dbgs() << "error at " << __LINE__ << "\n";
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
          tsar::diag::warn_region_add_loop_unable);
      } else {
        bool AddToRegionError = false;
        for (auto *R : mActiveRegions)
          if (!R->markForOptimization(*MatchItr->get<IR>())) {
            if (!AddToRegionError)
              toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
                tsar::diag::warn_region_add_loop_unable);
            AddToRegionError = true;
          }
      }
    }
    auto Res = RecursiveASTVisitor::TraverseStmt(S);
    mActiveRegions.resize(StashActivePragmaCount);
    LLVM_DEBUG(
        dbgs() << "[OPT REGION]: number of active regions on the level exit "
               << mActiveRegions.size() << "\n");
    return Res;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (mActiveRegions.empty())
      return true;
    auto MatchItr = mExprs.find<AST>(DynTypedNode::create(*CE));
    if (MatchItr == mExprs.end()) {
      toDiag(mSrcMgr.getDiagnostics(), CE->getBeginLoc(),
        tsar::diag::warn_region_add_call_unable);
    } else {
      bool AddToRegionError = false;
      for (auto *R : mActiveRegions) {
        LLVM_DEBUG(dbgs() << "[OPT REGION]: add to region ";
                   TSAR_LLVM_DUMP(MatchItr->get<IR>()->dump()));
        if (!R->markForOptimization(*MatchItr->get<IR>())) {
          if (!AddToRegionError)
            toDiag(mSrcMgr.getDiagnostics(), CE->getBeginLoc(),
              tsar::diag::warn_region_add_call_unable);
          AddToRegionError = true;
        }
      }
    }
    return true;
  }

private:
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  Rewriter &mRewriter;
  const LoopMatcherPass::LoopMatcher &mLoops;
  const ClangExprMatcherPass::ExprMatcher &mExprs;
  OptimizationRegionInfo &mRegions;
  SmallVector<OptimizationRegion *, 4> mActiveRegions;
  Stmt *mNewPragma = nullptr;
  unsigned mNewActiveRegions = 0;
};
}

INITIALIZE_PROVIDER(ClangRegionCollectorProvider, "clang-region-provider",
                    "Source-level Region Collector (Clang, Provider)")

char ClangRegionCollector::ID = 0;
INITIALIZE_PASS_BEGIN(ClangRegionCollector, "clang-region",
                      "Source-level Region Collector (Clang)", true, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ClangExprMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangRegionCollectorProvider)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(ClangRegionCollector, "clang-region",
                    "Source-level Region Collector (Clang)", true, true)

void ClangRegionCollector::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangRegionCollectorProvider>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}

bool ClangRegionCollector::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  ClangRegionCollectorProvider::initialize<TransformationEnginePass>(
      [&TfmInfo, &M](TransformationEnginePass &Wrapper) {
        Wrapper.set(*TfmInfo);
      });
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::vector<CallGraphNode *> Worklist;
  std::vector<std::size_t> SCCBounds{0};
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    for (auto *CGN : *I)
      if (auto F = CGN->getFunction()) {
        if (F->isDeclaration() || F->isIntrinsic() ||
            hasFnAttr(*F, AttrKind::LibFunc))
          continue;
        Worklist.push_back(CGN);
      }
    SCCBounds.push_back(Worklist.size());
  }
  if (Worklist.empty())
    return false;
  // Traverse SCCs in reverse postorder (top-down).
  for (auto I = SCCBounds.size() - 1; I > 0; --I) {
    auto SCCStartIdx = SCCBounds[I - 1];
    auto SCCEndIdx = SCCBounds[I];
    SmallPtrSet<OptimizationRegion *, 4> SCCRegions;
    for (auto CGNIdx = SCCStartIdx; CGNIdx < SCCEndIdx; ++CGNIdx) {
      auto *CGN = Worklist[CGNIdx];
      if (auto F = CGN->getFunction()) {
        if (F->isDeclaration() || F->isIntrinsic() ||
            hasFnAttr(*F, AttrKind::LibFunc))
          continue;
        LLVM_DEBUG(dbgs() << "[OPT REGION]: look up regions in "
                          << F->getName() << "\n");
        auto &Provider = getAnalysis<ClangRegionCollectorProvider>(*F);
        if (auto *DISub{findMetadata(F)})
          if (auto *CU{DISub->getUnit()};
              CU && (isC(CU->getSourceLanguage()) ||
                     isCXX(CU->getSourceLanguage()))) {
            auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
            auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                                       TfmInfo->getContext(*CU))
                                 : nullptr};
            if (TfmCtx && TfmCtx->hasInstance())
              if (auto *FD = TfmCtx->getDeclForMangledName(F->getName())) {
                auto &LM = Provider.get<LoopMatcherPass>().getMatcher();
                auto &EM = Provider.get<ClangExprMatcherPass>().getMatcher();
                RegionDirectiveVisitor(TfmCtx->getRewriter(), LM, EM, mRegions)
                    .TraverseDecl(FD);
              }
          }
        LLVM_DEBUG(dbgs() << "[OPT REGION]: total number of regions is "
                          << mRegions.size() << "\n");
        auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
        for (auto &R : mRegions)
          switch (R.contain(*F)) {
          case OptimizationRegion::CS_Always:
          case OptimizationRegion::CS_Condition:
            SCCRegions.insert(&R);
            for (auto &Callee : *CGN)
              if (Callee.first && *Callee.first) {
                LLVM_DEBUG(dbgs() << "[OPT REGION]: add to region ";
                           TSAR_LLVM_DUMP((**Callee.first).dump()));
                if (!R.markForOptimization(**Callee.first))
                  LLVM_DEBUG(dbgs() << "[OPT REGION]: unable to add\n");
              }
            break;
          case OptimizationRegion::CS_Child:
            for (auto &Callee : *CGN)
              if (Callee.first)
                if (auto *I = dyn_cast_or_null<Instruction>(*Callee.first)) {
                  if (auto *L = LI.getLoopFor(I->getParent()))
                    if (R.contain(*L)) {
                      LLVM_DEBUG(dbgs() << "[OPT REGION]: add to region ";
                                TSAR_LLVM_DUMP(I->dump()));
                      if (!R.markForOptimization(**Callee.first))
                        LLVM_DEBUG(dbgs() << "[OPT REGION]: unable to add\n");
                    }
                }
            break;
          default:
            break;
          }
      }
    }
    LLVM_DEBUG(dbgs() << "[OPT REGION]: SCC is located in " << SCCRegions.size()
                      << " regions\n");
    bool HasNewSCCRegion = false;
    do {
      HasNewSCCRegion = false;
      for (auto CGNIdx = SCCStartIdx; CGNIdx < SCCEndIdx; ++CGNIdx) {
        auto *CGN = Worklist[CGNIdx];
        if (auto F = CGN->getFunction()) {
          if (F->isDeclaration() || F->isIntrinsic() ||
            hasFnAttr(*F, AttrKind::LibFunc))
            continue;
          LLVM_DEBUG(dbgs() << "[OPT REGION]: update regions for "
            << F->getName() << "\n");
          for (auto &R : mRegions)
            switch (R.contain(*F)) {
            case OptimizationRegion::CS_Always:
            case OptimizationRegion::CS_Condition:
              HasNewSCCRegion |= SCCRegions.insert(&R).second;
              for (auto &Callee : *CGN)
                if (Callee.first && *Callee.first) {
                  LLVM_DEBUG(dbgs() << "[OPT REGION]: add to region ";
                  TSAR_LLVM_DUMP((**Callee.first).dump()));
                  if (!R.markForOptimization(**Callee.first))
                    LLVM_DEBUG(dbgs() << "[OPT REGION]: unable to add\n");
                }
              break;
            }
        }
      }
    } while (HasNewSCCRegion);
    LLVM_DEBUG(dbgs() << "[OPT REGION]: SCC is located in " << SCCRegions.size()
                      << " regions\n");
    for (auto CGNIdx = SCCStartIdx; CGNIdx < SCCEndIdx; ++CGNIdx) {
      auto *CGN = Worklist[CGNIdx];
      if (auto F = CGN->getFunction()) {
        if (F->isDeclaration() || F->isIntrinsic() ||
            hasFnAttr(*F, AttrKind::LibFunc))
          continue;
        LLVM_DEBUG(dbgs() << "[OPT REGION]: update regions for "
                          << F->getName() << "\n");
        for (auto *R : SCCRegions)
          for (auto &Callee : *CGN)
            if (Callee.first && *Callee.first) {
              LLVM_DEBUG(dbgs() << "[OPT REGION]: add to region ";
                         TSAR_LLVM_DUMP((**Callee.first).dump()));
              if (!R->markForOptimization(**Callee.first))
                LLVM_DEBUG(dbgs() << "[OPT REGION]: unable to add\n");
            }
      }
    }
  }
  LLVM_DEBUG(for (auto &R : mRegions) R.dump());
  return false;
}

bool OptimizationRegion::markForOptimization(const llvm::Loop &L) {
  mFunctions.try_emplace(L.getHeader()->getParent(), CS_Child);
  if (L.getLoopID())
    return mScopes.insert(L.getLoopID()).second;
  return contain(L);
}

bool OptimizationRegion::markForOptimization(const llvm::Value &V) {
  if (auto *F = dyn_cast<Function>(&V)) {
    auto Info = mFunctions.try_emplace(F, CS_Always);
    if (Info.second)
      return true;
    if (Info.first->second.Status == CS_Condition)
      Info.first->second.Condition.clear();
    Info.first->second.Status = CS_Always;
    return true;
  }
  if (auto *Call = dyn_cast<CallBase>(&V)) {
    auto F = dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
    if (!F)
      return false;
    auto Info = mFunctions.try_emplace(F, CS_Condition);
    if (Info.first->second.Status != CS_Always) {
      Info.first->second.Status = CS_Condition;
      Info.first->second.Condition.insert(&V);
    }
    return true;
  }
  return false;
}

bool OptimizationRegion::contain(const llvm::Loop &L) const {
  switch (contain(*L.getHeader()->getParent())) {
  case OptimizationRegion::CS_Always:
  case OptimizationRegion::CS_Condition:
    if (L.getLoopID())
      mScopes.insert(L.getLoopID());
    return true;
  }
  auto *CurrL = &L;
  while (CurrL && (!CurrL->getLoopID() || !mScopes.count(CurrL->getLoopID())))
    CurrL = CurrL->getParentLoop();
  if (CurrL && L.getLoopID())
    mScopes.insert(L.getLoopID());
  return CurrL;
}

LLVM_DUMP_METHOD void OptimizationRegion::dump() {
  dbgs() << "region name: '" << mName << "'\n";
  dbgs() << "number of marked scopes " << mScopes.size() << "\n";
  dbgs() << "number of marked functions " << mFunctions.size() << "\n";
  for (auto Info : mFunctions) {
    dbgs() << "contain function '" << Info.first->getName() << "' with status ";
    switch (Info.second.Status) {
    case OptimizationRegion::CS_No: dbgs() << "CS_No\n"; break;
    case OptimizationRegion::CS_Always: dbgs() << "CS_Always\n"; break;
    case OptimizationRegion::CS_Condition: dbgs() << "CS_Condition\n"; break;
    case OptimizationRegion::CS_Child: dbgs() << "CS_Child\n"; break;
    default: dbgs() << "CS_Invalid\n"; break;
    }
  }
}
