//===- LoopMatcher.cpp -- High and Low Level Loop Matcher -------*- C++ -*-===//
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
// This file implements pass to match loops.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/Matcher.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/IRUtils.h"
#include <bcl/transparent_queue.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <cstring>
#include <queue>

using namespace clang;
using namespace llvm;
using namespace tsar;
using ::llvm::Module;

#undef DEBUG_TYPE
#define DEBUG_TYPE "loop-matcher"

STATISTIC(NumMatchLoop, "Number of matched loops");
STATISTIC(NumNonMatchIRLoop, "Number of non-matched IR loops");
STATISTIC(NumNonMatchASTLoop, "Number of non-matched AST loops");

char LoopMatcherPass::ID = 0;
INITIALIZE_PASS_BEGIN(LoopMatcherPass, "loop-matcher",
  "High and Low Loop Matcher", true, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(LoopMatcherPass, "loop-matcher",
  "High and Low Level Loop Matcher", true, false)

namespace {
/// This matches explicit for, while and do-while loops.
class MatchExplicitVisitor :
  public ClangMatchASTBase<MatchExplicitVisitor, Loop *, Stmt *>,
  public RecursiveASTVisitor<MatchExplicitVisitor> {
public:

  /// Constructor.
  ///
  /// \param LM If match is found it will be stored in a bidirectional map LM.
  /// \param LocMap It is a map from loop location to loop (explicit or implicit).
  /// \param ImplicitMap If a loop in the LocMap map is recognized as implicit
  /// the appropriate high-level representation of this loop will not be
  /// determined (it could be determined later in MatchImplicitVisitor) but
  /// low-level representation will be stored in ImplicitMap. This map is also a
  /// map from location to loop but
  /// a key is always location of terminator in a loop header.
  /// \param MacroMap All explicit loops defined in macros is going to store
  /// in this map. These loops will not inserted in LM map and must be evaluated
  /// further. The key in this map is a raw encoding for expansion location.
  /// To decode it use SourceLocation::getFromRawEncoding() method.
  MatchExplicitVisitor(SourceManager &SrcMgr, Matcher &LM,
                       UnmatchedASTSet &Unmatched, LocToIRMap &LocMap,
                       LocToIRMap &ImplicitMap, LocToASTMap &MacroMap)
      : ClangMatchASTBase(SrcMgr, LM, Unmatched, LocMap, MacroMap),
        mLocToImplicit(&ImplicitMap) {}

  /// \brief Evaluates statements expanded from a macro.
  ///
  /// Implicit loops which are expanded from macro are not going to be
  /// evaluated, because in LLVM IR these loops have locations equal to
  /// expansion location. So it is not possible to determine token in macro
  /// body where these loops starts without additional analysis of AST.
  void VisitFromMacro(Stmt *S) {
    assert(S->getBeginLoc().isMacroID() &&
      "Statement must be expanded from macro!");
    if (!isa<WhileStmt>(S) && !isa<DoStmt>(S) && !isa<ForStmt>(S))
      return;
    auto Loc = S->getBeginLoc();
    if (Loc.isInvalid())
      return;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return;
    auto Itr{mLocToMacro->try_emplace(Loc.getRawEncoding()).first};
    Itr->second.push_back(S);
  }

  bool VisitStmt(Stmt *S) {
    if (S->getBeginLoc().isMacroID()) {
      VisitFromMacro(S);
      return true;
    }
    if (auto *For = dyn_cast<ForStmt>(S)) {
      // To determine appropriate loop in LLVM IR it is necessary to use start
      // location of initialization instruction, if it is available.
      if (Stmt *Init = For->getInit()) {
        auto LpItr = findItrForLocation(Init->getBeginLoc());
        if (LpItr != mLocToIR->end()) {
          Loop *L = LpItr->second.back();
          LpItr->second.pop_back();
          mMatcher->emplace(For, L);
          ++NumMatchLoop;
          --NumNonMatchIRLoop;
          // If there are multiple for-loops in a LoopQueue it means that these
          // loops have been included from some file (macro is evaluated
          // separately). It is necessary to restore this LoopQueue with
          // accurate location (not a location of initialization instruction) in
          // mLocToLoop. Otherwise when such instruction of currently evaluated
          // loop will be visited some loop from LoopQueue will be linked with
          // the instruction.
          if (!LpItr->second.empty()) {
            auto HeadBB = L->getHeader();
            auto HeaderLoc = HeadBB ?
              HeadBB->getTerminator()->getDebugLoc().get() : nullptr;
            PresumedLoc PLoc = mSrcMgr->getPresumedLoc(S->getBeginLoc(), false);
            auto Tmp = std::move(LpItr->second);
            mLocToIR->erase(LpItr);
            if (HeaderLoc && DILocationMapInfo::isEqual(PLoc, HeaderLoc))
              mLocToIR->insert(std::make_pair(HeaderLoc, std::move(Tmp)));
          }
          return true;
        }
      }
    }
    // If there is no initialization instruction, an appropriate loop has not
    // been found or considered loop is not a for-loop try to use accurate loop
    // location.
    auto StartLoc = S->getBeginLoc();
    if (isa<WhileStmt>(S) || isa<DoStmt>(S) || isa<ForStmt>(S)) {
      if (Loop *L = findIRForLocation(StartLoc)) {
        mMatcher->emplace(S, L);
        ++NumMatchLoop;
        --NumNonMatchIRLoop;
      } else {
        mUnmatchedAST->insert(S);
        ++NumNonMatchASTLoop;
      }
    } else {
      if (Loop *L = findIRForLocation(StartLoc)) {
        // We do not use Loop::getStartLoc() method for implicit loops because
        // it try to find start location in a pre-header but this location is
        // not suitable for such loops. The following example demonstrates this:
        // 1: goto l;
        // 2: ...
        // 3: l: ...
        // The loop starts at 3 but getStartLoc() returns 1.
        auto HeadBB = L->getHeader();
        auto HeaderLoc = HeadBB ?
          HeadBB->getTerminator()->getDebugLoc().get() : nullptr;
        if (HeaderLoc) {
          auto Itr{mLocToImplicit->try_emplace(HeaderLoc).first};
          Itr->second.push_back(L);
        }
      }
    }
    return true;
  }
private:
  LocToIRMap *mLocToImplicit;
};

/// This matches implicit loops.
class MatchImplicitVisitor :
  public ClangMatchASTBase<MatchImplicitVisitor, Loop *, Stmt *>,
  public RecursiveASTVisitor<MatchImplicitVisitor> {
public:
  MatchImplicitVisitor(SourceManager &SrcMgr, Matcher &LM,
                       UnmatchedASTSet &Unmatched, LocToIRMap &LocMap,
                       LocToASTMap &MacroMap)
      : ClangMatchASTBase(SrcMgr, LM, Unmatched, LocMap, MacroMap),
        mLastLabel(nullptr) {}

  bool VisitStmt(Stmt *S) {
    // We try to find a label which is a start of the loop header.
    if (isa<WhileStmt>(S) || isa<DoStmt>(S) || isa<ForStmt>(S))
      return true;
    if (isa<LabelStmt>(S)) {
      mLastLabel = cast<LabelStmt>(S);
      return true;
    }
    if (!mLastLabel)
      return true;
    if (Loop *L = findIRForLocation(S->getBeginLoc())) {
      mMatcher->emplace(mLastLabel, L);
      ++NumMatchLoop;
      --NumNonMatchIRLoop;
      updateMetadata(L);
    }
    return true;
  }

  /// Returns true some of !llvm.loop metadata have been changed.
  bool isDILoopChanged() const noexcept { return mDILoopChanged; }

private:
  /// Updates metadata for L if the metadata have been already set and loop
  /// is matched.
  ///
  /// Location of loop label will be set as a loop start location.
  /// If metadata will be updated isDILoopChanged() returns true.
  void updateMetadata(Loop *L) {
    assert(L && "Loop must not be null!");
    assert(mLastLabel && "Label must not be null!");
    if (auto LoopID = L->getLoopID()) {
      SmallVector<Metadata *, 3> MDs(1);
      auto HeadBB = L->getHeader();
      DILocation *DILoopLoc;
      auto LabelLoc = mLastLabel->getBeginLoc();
      auto HeaderLoc = HeadBB->getTerminator()->getDebugLoc().get();
      // The following assert should not fail because the condition has
      // been checked in MatchExplicitVisitor.
      assert(HeaderLoc && "Location must not be null!");
      if (LabelLoc.isInvalid()) {
        DILoopLoc = HeaderLoc;
      } else {
        auto PLoc = mSrcMgr->getPresumedLoc(LabelLoc, false);
        DILoopLoc = DILocation::get(HeadBB->getContext(),
          PLoc.getLine(), PLoc.getColumn(),
          HeaderLoc->getScope(), HeaderLoc->getInlinedAt());
      }
      MDs.push_back(DILoopLoc);
      for (unsigned I = 1, EI = LoopID->getNumOperands(); I < EI; ++I) {
        MDNode *Node = cast<MDNode>(LoopID->getOperand(I));
        if (isa<DILocation>(Node))
          continue;
        MDs.push_back(Node);
      }
      auto NewLoopID = MDNode::get(L->getHeader()->getContext(), MDs);
      NewLoopID->replaceOperandWith(0, NewLoopID);
      L->setLoopID(NewLoopID);
      mDILoopChanged |= true;
    }
  }

  LabelStmt *mLastLabel;
  bool mDILoopChanged = false;
};
}

bool LoopMatcherPass::runOnFunction(Function &F) {
  releaseMemory();
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
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  mFuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!mFuncDecl)
    return false;
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  MatchExplicitVisitor::LocToIRMap LocToLoop;
  for_each_loop(LpInfo, [&LocToLoop](Loop *L) {
    auto Loc = L->getStartLoc();
    // If an appropriate loop will be found the counter will be decreased.
    ++NumNonMatchIRLoop;
    if (Loc) {
      // In some cases different loops have the same locations. For example,
      // if these loops have been produced by one loop from a file that had been
      // included multiple times. A loop defined in macro is another case.
      auto Itr{LocToLoop.try_emplace(Loc).first};
      Itr->second.push_back(L);
    }
  });
  // Matcher uses back() to extract element from the list. So, we change
  // order of loops to preserver traverse order (loop is visited before
  // children).
  for (auto &Pair : LocToLoop)
    std::reverse(Pair.second.begin(), Pair.second.end());
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  MatchExplicitVisitor::LocToIRMap LocToImplicit;
  MatchExplicitVisitor::LocToASTMap LocToMacro;
  MatchExplicitVisitor MatchExplicit(SrcMgr, mMatcher, mUnmatchedAST,
    LocToLoop, LocToImplicit, LocToMacro);
  MatchExplicit.TraverseDecl(mFuncDecl);
  MatchImplicitVisitor MatchImplicit(SrcMgr, mMatcher, mUnmatchedAST,
    LocToImplicit, LocToMacro);
  for (auto &Pair: LocToImplicit)
    std::reverse(Pair.second.begin(), Pair.second.end());
  MatchImplicit.TraverseDecl(mFuncDecl);
  for (auto &Pair : LocToMacro)
    std::reverse(Pair.second.begin(), Pair.second.end());
  MatchExplicit.matchInMacro(
    NumMatchLoop, NumNonMatchASTLoop, NumNonMatchIRLoop);
  return MatchImplicit.isDILoopChanged();
}

void LoopMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createLoopMatcherPass() {
  return new LoopMatcherPass();
}
