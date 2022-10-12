//===--- LoopInterchange.cpp - Loop Interchagne (Clang) ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements a pass to perform loop interchange in C programs.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <algorithm>
#include <bcl/utility.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-loop-interchange"
#define DEBUG_PREFIX "[LOOP INTERCHANGE]: "

namespace {
class ClangLoopInterchange : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangLoopInterchange() : FunctionPass(ID) {
    initializeClangLoopInterchangePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class ClangLoopInterchangeInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};
} // namespace

void ClangLoopInterchangeInfo::addBeforePass(
    legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createDIMemoryEnvironmentStorage());
  Passes.add(createGlobalsAccessStorage());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(createDIMemoryAnalysisServer());
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createAnalysisWaitServerPass());
}

void ClangLoopInterchangeInfo::addAfterPass(legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}

void ClangLoopInterchange::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.addRequired<ClangPerfectLoopPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

char ClangLoopInterchange::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangLoopInterchange, "clang-loop-interchange",
                               "Loop Interchange (Clang)", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangLoopInterchangeInfo)
INITIALIZE_PASS_DEPENDENCY(AnalysisClientServerMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(ClangLoopInterchange, "clang-loop-interchange",
                             "Loop Interchange (Clang)", false, false,
                             TransformationQueryManager::getPassRegistry())

namespace {
class InterchangeClauseVisitor
    : public clang::RecursiveASTVisitor<InterchangeClauseVisitor> {
public:
  explicit InterchangeClauseVisitor(
      SmallVectorImpl<std::tuple<clang::StringLiteral *, clang::Stmt *>> &Ls)
      : mLiterals(Ls) {}
  bool VisitStringLiteral(clang::StringLiteral *SL) {
    if (SL->getString() != getName(ClauseId::LoopInterchange))
      mLiterals.emplace_back(SL, mClause);
    return true;
  }

  void setClause(clang::Stmt *C) noexcept { mClause = C; }

private:
  SmallVectorImpl<std::tuple<clang::StringLiteral *, clang::Stmt *>> &mLiterals;
  clang::Stmt *mClause{nullptr};
};

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class ClangLoopInterchangeVisitor
    : public clang::RecursiveASTVisitor<ClangLoopInterchangeVisitor> {
  enum LoopKind : uint8_t {
    Ok = 0,
    NotCanonical = 1u << 0,
    NotPerfect = 1u << 1,
    NotAnalyzed = 1u << 2,
    HasDependency = 1u << 3,
    LLVM_MARK_AS_BITMASK_ENUM(HasDependency)
  };

  using VarList = SmallVector<clang::VarDecl *, 4>;

  using LoopNest = SmallVector<
      std::tuple<clang::VarDecl *, const DIMemory *, clang::ForStmt *, LoopKind,
                 VarList, const CanonicalLoopInfo *>,
      4>;

public:
  ClangLoopInterchangeVisitor(ClangLoopInterchange &P, llvm::Function &F,
                              ClangTransformationContext *TfmCtx,
                              const ASTImportInfo &ImportInfo)
      : mImportInfo(ImportInfo), mRewriter(TfmCtx->getRewriter()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()),
        mRawInfo(
            P.getAnalysis<ClangGlobalInfoPass>().getGlobalInfo(TfmCtx)->RI),
        mGlobalOpts(
            P.getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()),
        mMemMatcher(P.getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher),
        mDIMemMatcher(P.getAnalysis<ClangDIMemoryMatcherPass>().getMatcher()),
        mPerfectLoopInfo(
            P.getAnalysis<ClangPerfectLoopPass>().getPerfectLoopInfo()),
        mCanonicalLoopInfo(
            P.getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo()),
        mAT(P.getAnalysis<EstimateMemoryPass>().getAliasTree()),
        mTLI(P.getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F)),
        mDT(P.getAnalysis<DominatorTreeWrapperPass>().getDomTree()),
        mDIMInfo(P.getAnalysis<DIEstimateMemoryPass>().getAliasTree(), P, F) {
    if (mDIMInfo.isValid())
      mSTR = SpanningTreeRelation<const DIAliasTree *>{mDIMInfo.DIAT};
  }

  bool TraverseForStmt(clang::ForStmt *FS) {
    if (mStatus != GET_REFERENCES)
      return RecursiveASTVisitor::TraverseForStmt(FS);
    auto CanonicalItr{find_if(mCanonicalLoopInfo, [FS](auto *Info) {
      return Info->getASTLoop() == FS;
    })};
    auto IsCanonical{false}, IsPerfect{false};
    mInductions.emplace_back(nullptr, nullptr, FS, Ok, VarList{}, nullptr);
    if (CanonicalItr != mCanonicalLoopInfo.end() &&
        (*CanonicalItr)->isCanonical()) {
      IsCanonical = true;
      IsPerfect = mPerfectLoopInfo.count((*CanonicalItr)->getLoop());
      auto VarItr{mMemMatcher.find<IR>((*CanonicalItr)->getInduction())};
      auto ASTItr{find_if(VarItr->get<AST>(), [this](const clang::VarDecl *VD) {
        return &mSrcMgr == &VD->getASTContext().getSourceManager();
      })};
      assert(ASTItr != VarItr->get<AST>().end() &&
             "Matched variable must be presented in a current AST context.");
    std::get<clang::VarDecl *>(mInductions.back()) = *ASTItr;
      std::get<const CanonicalLoopInfo *>(mInductions.back()) = *CanonicalItr;
    }
    if (!IsCanonical)
      std::get<LoopKind>(mInductions.back()) |= NotCanonical;
    if (!IsPerfect)
      std::get<LoopKind>(mInductions.back()) |= NotPerfect;
    if (!mIsStrict || !IsCanonical) {
      if (!IsCanonical)
        std::get<LoopKind>(mInductions.back()) |= NotAnalyzed;
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
    auto Dependency{false};
    auto *L{(*CanonicalItr)->getLoop()->getLoop()};
    if (!mDIMInfo.isValid()) {
      std::get<LoopKind>(mInductions.back()) |= NotAnalyzed;
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
    auto *DIDepSet{mDIMInfo.findFromClient(*L)};
    if (!DIDepSet) {
      std::get<LoopKind>(mInductions.back()) |= NotAnalyzed;
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
    DenseSet<const DIAliasNode *> Coverage;
    accessCoverage<bcl::SimpleInserter>(*DIDepSet, *mDIMInfo.DIAT, Coverage,
                                        mGlobalOpts.IgnoreRedundantMemory);
    if (!Coverage.empty())
      for (auto &Trait : *DIDepSet) {
        if (!Coverage.count(Trait.getNode()) || hasNoDep(Trait) ||
            Trait.is_any<trait::Private, trait::Reduction>())
          continue;
        if(Trait.is_any<trait::Induction, trait::SecondToLastPrivate>() &&
          Trait.size() == 1) {
          if (auto DIEM{
                  dyn_cast<DIEstimateMemory>((*Trait.begin())->getMemory())};
              DIEM && DIEM->getExpression()->getNumElements() == 0) {
            auto *ClientDIM{
                mDIMInfo.getClientMemory(const_cast<DIEstimateMemory *>(DIEM))};
            assert(ClientDIM && "Origin memory must exist!");
            auto VarToDI{mDIMemMatcher.find<MD>(
                cast<DIEstimateMemory>(ClientDIM)->getVariable())};
            if (Trait.is<trait::Induction>()) {
              if (VarToDI->template get<AST>() ==
                  std::get<clang::VarDecl *>(mInductions.back())) {
                std::get<const DIMemory *>(mInductions.back()) = DIEM;
                continue;
              }
            } else {
              std::get<VarList>(mInductions.back())
                  .push_back(VarToDI->template get<AST>());
              continue;
            }
          }
        }
        std::get<LoopKind>(mInductions.back()) |= HasDependency;
        break;
      }
    return RecursiveASTVisitor::TraverseForStmt(FS);
  }

  bool TraverseDecl(clang::Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             tsar::diag::warn_interchange_not_for_loop);
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case SEARCH_PRAGMA: {
      Pragma P{*S};
      llvm::SmallVector<clang::Stmt *, 2> Clauses;
      if (!findClause(P, ClauseId::LoopInterchange, Clauses))
        return RecursiveASTVisitor::TraverseStmt(S);
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << "found interchange clause\n");
      InterchangeClauseVisitor SCV{mSwaps};
      for (auto *C : Clauses) {
        SCV.setClause(C);
        SCV.TraverseStmt(C);
      }
      mIsStrict = !findClause(P, ClauseId::NoStrict, Clauses);
      LLVM_DEBUG(if (!mIsStrict) dbgs()
                  << DEBUG_PREFIX << "found 'nostrict' clause\n");
      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
      auto IsPossible{pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
                                          mImportInfo, ToRemove)};
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
                  tsar::diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
                  tsar::diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
                  tsar::diag::warn_remove_directive);
      clang::Rewriter::RewriteOptions RemoveEmptyLine;
      /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
      /// set to true then removing (in RewriterBuffer) works incorrect.
      RemoveEmptyLine.RemoveLineIfEmpty = false;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine);
      Clauses.clear();
      mStatus = TRAVERSE_STMT;
      return true;
    }
    case TRAVERSE_STMT: {
      if (!isa<clang::ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
               tsar::diag::warn_interchange_not_for_loop);
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      bool HasMacro{false};
      for_each_macro(S, mSrcMgr, mLangOpts, mRawInfo.Macros,
                     [&HasMacro, this](clang::SourceLocation Loc) {
                       if (!HasMacro) {
                         toDiag(mSrcMgr.getDiagnostics(), Loc,
                                tsar::diag::note_assert_no_macro);
                         HasMacro = true;
                       }
                     });
      if (HasMacro) {
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      mStatus = GET_REFERENCES;
      auto Res = TraverseForStmt(cast<clang::ForStmt>(S));
      if (!Res) {
        resetVisitor();
        return false;
      }
      // Match induction names from clauses to loop induction variables.
      unsigned MaxIdx{0};
      llvm::SmallVector<clang::VarDecl *, 4> ValueSwaps;
      for (auto [Literal, Clause] : mSwaps) {
        auto Str{Literal->getString()};
        unsigned InductIdx{0}, InductIdxE = mInductions.size();
        for (; InductIdx < InductIdxE; ++InductIdx) {
          if (!std::get<clang::VarDecl *>(mInductions[InductIdx]))
            continue;
          if (std::get<clang::VarDecl *>(mInductions[InductIdx])->getName() ==
              Str) {
            ValueSwaps.push_back(
                std::get<clang::VarDecl *>(mInductions[InductIdx]));
            MaxIdx = (InductIdx > MaxIdx) ? InductIdx : MaxIdx;
            break;
          }
        }
        if (InductIdx == InductIdxE) {
          toDiag(mSrcMgr.getDiagnostics(), Clause->getBeginLoc(),
                 tsar::diag::warn_disable_loop_interchange);
          toDiag(mSrcMgr.getDiagnostics(), Literal->getBeginLoc(),
                 tsar::diag::note_interchange_induction_mismatch)
              << Str;
          resetVisitor();
          return RecursiveASTVisitor::TraverseStmt(S);
        }
      }
      // Check whether transfromation is possible.
      auto checkLoop = [this](unsigned I, unsigned MaxIdx) {
        auto checkPrivatizable = [](VarList &L, auto I, auto EI) {
          for (auto *V : L)
            if (EI == std::find_if(I, EI, [V](auto &Induct) {
                  return std::get<clang::VarDecl *>(Induct) == V;
                }))
              return false;

          return true;
        };
        if (std::get<LoopKind>(mInductions[I]) & NotCanonical) {
          toDiag(mSrcMgr.getDiagnostics(),
                 std::get<clang::ForStmt *>(mInductions[I])->getBeginLoc(),
                 tsar::diag::warn_interchange_not_canonical);
        } else if (std::get<LoopKind>(mInductions[I]) & HasDependency) {
          toDiag(mSrcMgr.getDiagnostics(),
                 std::get<clang::ForStmt *>(mInductions[I])->getBeginLoc(),
                 tsar::diag::warn_interchange_dependency);
        } else if (!mIsStrict) {
          return true;
        } else if (std::get<LoopKind>(mInductions[I]) & NotAnalyzed) {
          toDiag(mSrcMgr.getDiagnostics(),
                 std::get<clang::ForStmt *>(mInductions[I])->getBeginLoc(),
                 tsar::diag::warn_interchange_no_analysis);
        } else if (!checkPrivatizable(std::get<VarList>(mInductions[I]),
                                      mInductions.begin() + I + 1,
                                      mInductions.end())) {
          toDiag(mSrcMgr.getDiagnostics(),
                 std::get<clang::ForStmt *>(mInductions[I])->getBeginLoc(),
                 tsar::diag::warn_interchange_dependency);
        } else if (auto *For{isMemoryAccessedIn(
                       std::get<const DIMemory *>(mInductions[I]),
                       std::get<const CanonicalLoopInfo *>(mInductions[I])
                           ->getLoop()
                           ->getLoop(),
                       mInductions.begin() + I + 1,
                       mInductions.begin() + MaxIdx + 1)}) {
          toDiag(mSrcMgr.getDiagnostics(),
                 std::get<clang::ForStmt *>(mInductions[I])->getBeginLoc(),
                 tsar::diag::warn_disable_loop_interchange);
          toDiag(mSrcMgr.getDiagnostics(), For->getBeginLoc(),
                 tsar::diag::note_interchange_irregular_loop_nest);
        } else {
          return true;
        }
        return false;
      };
      auto IsPossible(true);
      for (auto I{0u}; I < MaxIdx; I++) {
        if (std::get<LoopKind>(mInductions[I]) & NotPerfect) {
          IsPossible = false;
          toDiag(mSrcMgr.getDiagnostics(),
                 std::get<clang::ForStmt *>(mInductions[I])->getBeginLoc(),
                 tsar::diag::warn_interchange_not_perfect);
        } else {
          IsPossible &= checkLoop(I, MaxIdx);
        }
      }
      // Check the innermost loop which participates in the transformation.
      // Note, the it may have not canonical loop form.
      IsPossible &= checkLoop(MaxIdx, MaxIdx);
      if (!IsPossible) {
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // Deduce new order.
      SmallVector<clang::VarDecl *, 4> Order;
      std::transform(mInductions.begin(), mInductions.begin() + MaxIdx + 1,
                     std::back_inserter(Order),
                     [](auto &I) { return std::get<clang::VarDecl *>(I); });
      for (unsigned I{0}, EI = ValueSwaps.size(); I < EI; I += 2) {
        auto FirstItr{find(Order, ValueSwaps[I])};
        assert(FirstItr != Order.end() && "Induction must exist!");
        auto SecondItr{find(Order, ValueSwaps[I + 1])};
        assert(SecondItr != Order.end() && "Induction must exist!");
        std::swap(*FirstItr, *SecondItr);
      }
      // Collect sources of loop headers before the transformation. Note, do not
      // use Rewriter::ReplaceText(SourceRange, SourceRange) because it uses
      // Rewriter::getRangeSize(SourceRange) to compute a length of a destination
      // as well as a length of a source and this method uses rewritten text to
      // collect size. Thus, the size of the source can be computed in the wrong
      // way because the transformation of outer loops has already taken place.
      SmallVector<std::string, 4> Sources;
      for (auto I{0u}; I < MaxIdx + 1; I++) {
        clang::SourceRange Source{
            std::get<clang::ForStmt *>(mInductions[I])->getInit()->getBeginLoc(),
            std::get<clang::ForStmt *>(mInductions[I])->getInc()->getEndLoc()};
        Sources.push_back(mRewriter.getRewrittenText(Source));
      }
      // Interchange loop headers.
      for (auto I{0u}; I < MaxIdx + 1 ; I++) {
        if (Order[I] != std::get<clang::VarDecl *>(mInductions[I])) {
          auto OriginItr{find_if(mInductions, [What = Order[I]](auto &Induct) {
            return std::get<clang::VarDecl *>(Induct) == What;
          })};
          clang::SourceRange Destination(
              std::get<clang::ForStmt *>(mInductions[I])
                  ->getInit()
                  ->getBeginLoc(),
              std::get<clang::ForStmt *>(mInductions[I])
                  ->getInc()
                  ->getEndLoc());
          mRewriter.ReplaceText(
              Destination,
              Sources[std::distance(mInductions.begin(), OriginItr)]);
        }
      }
      LLVM_DEBUG(dbgs() << DEBUG_PREFIX << ": finish pragma processing\n");
      resetVisitor();
      return true;
    }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }


private:
  void resetVisitor() {
    mStatus = SEARCH_PRAGMA;
    mSwaps.clear();
    mInductions.clear();
  }

  /// Return true if a specified induction variable `DIM` of a loop `L` may be
  /// accessed in an any header of specified canonical loops.
  clang::ForStmt *isMemoryAccessedIn(const DIMemory *DIM, const Loop *L,
                                     LoopNest::iterator I,
                                     LoopNest::iterator EI) {
    assert(DIM &&
           "Results of canonical loop analysis must be available for a loop!");
    for (auto &Induct : make_range(I, EI)) {
      if (isMemoryAccessedIn(
              DIM, std::get<const CanonicalLoopInfo *>(Induct)->getStart()) ||
          isMemoryAccessedIn(
              DIM, std::get<const CanonicalLoopInfo *>(Induct)->getEnd()) ||
          isMemoryAccessedIn(
              DIM, L, std::get<const CanonicalLoopInfo *>(Induct)->getStep()))
        return std::get<clang::ForStmt *>(Induct);
    }
    return nullptr;
  }

  /// Return true if a specified induction variable `DIM` of a loop `L` may be
  /// accessed in a specified expression `S`.
  bool isMemoryAccessedIn(const DIMemory *DIM, const Loop *L, const SCEV *S) {
    if (!S)
      return true;
    struct SCEVMemorySearch {
      SCEVMemorySearch(const Loop *Lp) : L(Lp) {}
      bool follow(const SCEV *S) {
        if (auto *AddRec{dyn_cast<SCEVAddRecExpr>(S)};
            AddRec && AddRec->getLoop() == L)
          L = nullptr;
        else if (auto *Unknown{dyn_cast<SCEVUnknown>(S)})
          Values.push_back(Unknown->getValue());
        return true;
      }
      bool isDone() {
        // Finish search if reference to a loop has been found.
        return !L;
      }
      SmallVector<Value *, 4> Values;
      const Loop *L;
    } Visitor{L};
    visitAll(S, Visitor);
    if (!Visitor.L)
      return true;
    for (auto *V : Visitor.Values)
      if (isMemoryAccessedIn(DIM, V))
        return true;
    return false;
  }

  /// Return true if a specified induction variable `DIM` of a loop `L` may be
  /// accessed in a specified value `V`.
  bool isMemoryAccessedIn(const DIMemory *DIM, Value *V) {
    if (!V)
      return true;
    if (isa<ConstantData>(V))
      return false;
    if (auto *Inst{dyn_cast<Instruction>(V)}) {
      auto &DL{Inst->getModule()->getDataLayout()};
      bool Result{false};
      auto DIN{DIM->getAliasNode()};
      for_each_memory(
          *Inst, mTLI,
          [this, DIN, &DL, &Result](Instruction &, MemoryLocation &&Loc,
                                    unsigned, AccessInfo R, AccessInfo W) {
            if (Result)
              return;
            if (R == AccessInfo::No && W == AccessInfo::No)
              return;
            auto EM{mAT.find(Loc)};
            assert(EM && "Estimate memory location must not be null!");
            auto *DIM{mDIMInfo.findFromClient(*EM->getTopLevelParent(), DL, mDT)
                          .get<Clone>()};
            Result = !DIM || !mSTR->isUnreachable(DIM->getAliasNode(), DIN);
          },
          [this, DIN, &Result](Instruction &I, AccessInfo, AccessInfo) {
            return;
            if (Result || (Result = !isa<CallBase>(I)))
              return;
            auto *DIM{mDIMInfo.findFromClient(I, mDT, DIUnknownMemory::NoFlags)
                          .get<Clone>()};
            assert(DIM && "Metadata-level memory must be available!");
            Result = !DIM || !mSTR->isUnreachable(DIM->getAliasNode(), DIN);
          });
      if (Result)
        return true;
      for (auto &Op : Inst->operands())
        if (!isa<ConstantData>(Op) && !isa<GlobalValue>(Op) &&
            isMemoryAccessedIn(DIM, Op))
          return true;
    }
    return false;
  }

  const ASTImportInfo mImportInfo;
  clang::Rewriter &mRewriter;
  clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;
  ClangGlobalInfo::RawInfo &mRawInfo;
  const GlobalOptions &mGlobalOpts;
  MemoryMatchInfo::MemoryMatcher &mMemMatcher;
  const ClangDIMemoryMatcherPass::DIMemoryMatcher &mDIMemMatcher;
  const CanonicalLoopSet &mCanonicalLoopInfo;
  PerfectLoopInfo &mPerfectLoopInfo;
  AliasTree &mAT;
  TargetLibraryInfo &mTLI;
  DominatorTree &mDT;
  DIMemoryClientServerInfo mDIMInfo;
  std::optional<SpanningTreeRelation<const DIAliasTree *>> mSTR;
  bool mIsStrict{true};
  enum Status {
    SEARCH_PRAGMA,
    TRAVERSE_STMT,
    GET_REFERENCES
  } mStatus{SEARCH_PRAGMA};
  SmallVector<std::tuple<clang::StringLiteral *, clang::Stmt *>, 4> mSwaps;
  LoopNest mInductions;
};
} // namespace

bool ClangLoopInterchange::runOnFunction(Function &F) {
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError(
        "cannot transform sources"
        ": transformation context is not available for the '" +
        F.getName() + "' function");
    return false;
  }
  auto *FD{TfmCtx->getDeclForMangledName(F.getName())};
  if (!FD)
    return false;
  ASTImportInfo ImportStub;
  const auto *ImportInfo{&ImportStub};
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  ClangLoopInterchangeVisitor(*this, F, TfmCtx, *ImportInfo).TraverseDecl(FD);
  return false;
}
