//===- tsar_loop_matcher.cpp - High and Low Level Loop Matcher---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements pass to match loops.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Function.h>
#include <cstring>
#include <queue>
#include <transparent_queue.h>
#include "tsar_loop_matcher.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"

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
INITIALIZE_PASS_BEGIN(LoopMatcherPass, "matcher",
  "High and Low Loop Matcher", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(LoopMatcherPass, "matcher",
  "High and Low Level Loop Matcher", true, true)

namespace {
/// \brief Implementation of a DenseMapInfo for DILocation *.
///
/// To generate hash value pair of line and column is used. It is possible to
/// use find_as() method with a parameter of type clang::PresumedLoc.
struct DILocationMapInfo {
  static inline DILocation * getEmptyKey() {
    return DenseMapInfo<DILocation *>::getEmptyKey();
  }
  static inline DILocation * getTombstoneKey() {
    return DenseMapInfo<DILocation *>::getTombstoneKey();
  }
  static unsigned getHashValue(const DILocation *Loc) {
    auto Line = Loc->getLine();
    auto Column = Loc->getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static unsigned getHashValue(const PresumedLoc &PLoc) {
    auto Line = PLoc.getLine();
    auto Column = PLoc.getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static bool isEqual(const DILocation *LHS, const DILocation *RHS) {
    auto TK = getTombstoneKey();
    auto EK = getEmptyKey();
    return LHS == RHS ||
      RHS != TK && LHS != TK && RHS != EK && LHS != EK &&
      LHS->getLine() == RHS->getLine() &&
      LHS->getColumn() == RHS->getColumn() &&
      LHS->getFilename() == RHS->getFilename();
  }
  static bool isEqual(const PresumedLoc &LHS, const DILocation *RHS) {
    return !isEqual(RHS, getTombstoneKey()) &&
      !isEqual(RHS, getEmptyKey()) &&
      LHS.getLine() == RHS->getLine() &&
      LHS.getColumn() == RHS->getColumn() &&
      LHS.getFilename() == RHS->getFilename();
  }
};

/// This is a base class which is inherited to match loops.
class MatchASTBase {
public:
  typedef DenseMap<
    DILocation *, bcl::TransparentQueue<Loop>, DILocationMapInfo> LocToLoopMap;

protected:
  MatchASTBase(LoopMatcherPass::LoopMatcher &LM,
     LocToLoopMap &LocMap, SourceManager &SrcMgr) :
    mMatcher(&LM), mLocToLoop(&LocMap), mSrcMgr(&SrcMgr) {}

  /// Finds low-level representation of a loop at the specified location.
  ///
  /// \return LLVM IR for a loop or `nullptr`.
  Loop * findLoopForLocation(SourceLocation Loc) {
    auto LocItr = findItrForLocation(Loc);
    if (LocItr == mLocToLoop->end())
      return nullptr;
    return LocItr->second.pop();
  }

  /// Finds low-level representation of a loop at the specified location.
  ///
  /// \return Iterator to an element in LocToLoopMap.
  LocToLoopMap::iterator findItrForLocation(SourceLocation Loc) {
    if (Loc.isInvalid())
      return mLocToLoop->end();
    PresumedLoc PLoc = mSrcMgr->getPresumedLoc(Loc, false);
    return mLocToLoop->find_as(PLoc);
  }

  LoopMatcherPass::LoopMatcher *mMatcher;
  LocToLoopMap *mLocToLoop;
  SourceManager *mSrcMgr;
};

/// This matches explicit for, while and do-while loops.
class MatchExplicitVisitor :
  public MatchASTBase, public RecursiveASTVisitor<MatchExplicitVisitor> {
public:
  typedef DenseMap<unsigned, bcl::TransparentQueue<Stmt>> LocToStmtMap;

  /// Constructor.
  ///
  /// \param LM If match is found it will be stored in a bidirectional map LM.
  /// \param LocMap is a map from loop location to loop (explicit or implicit).
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
  MatchExplicitVisitor(LoopMatcherPass::LoopMatcher &LM,
      LoopMatcherPass::LoopASTSet &Unmatched,
      LocToLoopMap &LocMap, LocToLoopMap &ImplicitMap, LocToStmtMap &MacroMap,
      SourceManager &SrcMgr) :
    MatchASTBase(LM, LocMap, SrcMgr), mUnmatched(&Unmatched),
    mLocToImplicit(&ImplicitMap), mLocToMacro(&MacroMap) {}

  /// \brief Evaluates statements expanded from a macro.
  ///
  /// Implicit loops which are expanded from macro are not going to be
  /// evaluated, because in LLVM IR these loops have locations equal to
  /// expansion location. So it is not possible to determine token in macro
  /// body where these loops starts without additional analysis of AST.
  void VisitFromMacro(Stmt *S) {
    assert(S->getLocStart().isMacroID() &&
      "Statement must be expanded from macro!");
    if (!isa<WhileStmt>(S) && !isa<DoStmt>(S) && !isa<ForStmt>(S))
      return;
    auto Loc = S->getLocStart();
    if (Loc.isInvalid())
      return;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return;
    auto Pair = mLocToMacro->insert(
      std::make_pair(Loc.getRawEncoding(), bcl::TransparentQueue<Stmt>(S)));
    if (!Pair.second)
      Pair.first->second.push(S);
    return;
  }

  bool VisitStmt(Stmt *S) {
    if (S->getLocStart().isMacroID()) {
      VisitFromMacro(S);
      return true;
    }
    if (auto *For = dyn_cast<ForStmt>(S)) {
      // To determine appropriate loop in LLVM IR it is necessary to use start
      // location of initialization instruction, if it is available.
      if (Stmt *Init = For->getInit()) {
        auto LpItr = findItrForLocation(Init->getLocStart());
        if (LpItr != mLocToLoop->end()) {
          Loop *L = LpItr->second.pop();
          mMatcher->emplace(For, L);
          ++NumMatchLoop;
          --NumNonMatchIRLoop;
          // If there are multiple for-loops in a LoopQueue it means that these
          // loops have been included from some file (macro is evaluated
          // separately). It is necessary to restore this LoopQueue with accurate
          // location (not a location of initialization instruction) in mLocToLop.
          // Otherwise when such instruction of currently evaluated loop will be
          // visited some loop from LoopQueue will be linked with the instruction.
          if (!LpItr->second.empty()) {
            auto HeadBB = L->getHeader();
            auto HeaderLoc = HeadBB ?
              HeadBB->getTerminator()->getDebugLoc().get() : nullptr;
            PresumedLoc PLoc = mSrcMgr->getPresumedLoc(S->getLocStart(), false);
            if (HeaderLoc && DILocationMapInfo::isEqual(PLoc, HeaderLoc))
              mLocToLoop->insert(
                std::make_pair(HeaderLoc, std::move(LpItr->second)));
            mLocToLoop->erase(LpItr);
          }
          return true;
        }
      }
    }
    // If there is no initialization instruction, an appropriate loop has not
    // been found or considered loop is not a for-loop try to use accurate loop
    // location.
    auto StartLoc = S->getLocStart();
    if (isa<WhileStmt>(S) || isa<DoStmt>(S) || isa<ForStmt>(S)) {
      if (Loop *L = findLoopForLocation(StartLoc)) {
        mMatcher->emplace(S, L);
        ++NumMatchLoop;
        --NumNonMatchIRLoop;
      } else {
        mUnmatched->insert(S);
        ++NumNonMatchASTLoop;
      }
    } else {
      if (Loop *L = findLoopForLocation(StartLoc)) {
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
          auto Pair =
            mLocToImplicit->insert(
              std::make_pair(HeaderLoc, bcl::TransparentQueue<Loop>(L)));
          if (!Pair.second)
            Pair.first->second.push(L);
        }
      }
    }
    return true;
  }
private:
  LocToLoopMap *mLocToImplicit;
  LocToStmtMap *mLocToMacro;
  LoopMatcherPass::LoopASTSet *mUnmatched;
};

/// This matches implicit loops.
class MatchImplicitVisitor :
  public MatchASTBase, public RecursiveASTVisitor<MatchImplicitVisitor> {
public:
  MatchImplicitVisitor(LoopMatcherPass::LoopMatcher &LM,
      LocToLoopMap &LocMap, SourceManager &SrcMgr) :
    MatchASTBase(LM, LocMap, SrcMgr), mLastLabel(nullptr) {}

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
    if (Loop *L = findLoopForLocation(S->getLocStart())) {
        mMatcher->emplace(mLastLabel, L);
      ++NumMatchLoop;
      --NumNonMatchIRLoop;
    }
    return true;
  }

private:
  LabelStmt *mLastLabel;
};
}

bool LoopMatcherPass::runOnFunction(Function &F) {
  releaseMemory();
  auto M = F.getParent();
  auto TfmCtx  = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  mFuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!mFuncDecl)
    return false;
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  MatchASTBase::LocToLoopMap LocToLoop;
  for_each(LpInfo, [&LocToLoop](Loop *L) {
    auto Loc = L->getStartLoc();
    // If an appropriate loop will be found the counter will be decreased.
    ++NumNonMatchIRLoop;
    if (Loc) {
      auto Pair = LocToLoop.insert(
        std::make_pair(Loc, bcl::TransparentQueue<Loop>(L)));
      // In some cases different loops have the same locations. For example,
      // if these loops have been produced by one loop from a file that had been
      // included multiple times. The other case is a loop defined in macro.
      if (!Pair.second)
        Pair.first->second.push(L);
    }
  });
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  MatchASTBase::LocToLoopMap LocToImplicit;
  MatchExplicitVisitor::LocToStmtMap LocToMacro;
  MatchExplicitVisitor MatchExplicit(mMatcher, mUnmatchedAST,
    LocToLoop, LocToImplicit, LocToMacro, SrcMgr);
  MatchExplicit.TraverseDecl(mFuncDecl);
  MatchImplicitVisitor MatchImplicit(mMatcher, LocToImplicit, SrcMgr);
  MatchImplicit.TraverseDecl(mFuncDecl);
  // Evaluate loops in macros.
  for (auto &InMacro : LocToMacro) {
    PresumedLoc PLoc = SrcMgr.getPresumedLoc(
      SourceLocation::getFromRawEncoding(InMacro.first), false);
    auto IRLoopItr = LocToLoop.find_as(PLoc);
    // If sizes of queues of AST and IR loops are not equal this is mean that
    // there are implicit loops in a macro. Such loops are not going to be
    // evaluated due to necessity of additional analysis of AST.
    if (IRLoopItr == LocToLoop.end() ||
        IRLoopItr->second.size() != InMacro.second.size()) {
      NumNonMatchASTLoop += InMacro.second.size();
      while (auto ASTLoop = InMacro.second.pop())
        mUnmatchedAST.insert(ASTLoop);
    } else {
      NumMatchLoop += InMacro.second.size();
      NumNonMatchIRLoop -= InMacro.second.size();
      while (auto ASTLoop = InMacro.second.pop())
        mMatcher.emplace(ASTLoop, IRLoopItr->second.pop());
    }
  }
  return false;
}

void LoopMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createLoopMatcherPass() {
  return new LoopMatcherPass();
}