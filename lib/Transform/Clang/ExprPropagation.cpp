//===- ExpressionPropagation.h - Expression Propagation (Clang) --*- C++ -*===//
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
// This file implements a pass to replace the occurrences of variables with
// expressions which compute their values.
// Note, that propagation of array subranges is also supported, for example
// the following assignments can be processed '(*A)[5] = B[X]', where B is
// a 3-dimensional array.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/ADT/PersistentMap.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/Matcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Tags.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include "tsar/Unparse/Utils.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar.h>
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <stack>
#include <tuple>
#include <vector>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-propagate"

namespace {
class ClangExprPropagation : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangExprPropagation() : FunctionPass(ID) {
    initializeClangExprPropagationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  /// Unparse replacement for a specified metadata-level candidate 'DIUse'.
  ///
  /// If `DIDef` is not specified then `Def` must be a constant for successful
  /// unparsing.
  bool unparseReplacement(const Value &Def, const Use &Use,
    const tsar::DIMemoryLocation *DIDef, unsigned DWLang,
    const tsar::DIMemoryLocation &DIUse, SmallVectorImpl<char> &DefStr);

  const DataLayout *mDL = nullptr;
  DominatorTree *mDT = nullptr;
  tsar::ClangTransformationContext *mTfmCtx = nullptr;
};

class ClangCopyPropagationInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &PM) const override {
    PM.add(createSROAPass());
    PM.add(createMemoryMatcherPass());
  }
};
}

char ClangExprPropagation::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangExprPropagation, "clang-propagate",
  "Expression Propagation (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangCopyPropagationInfo);
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PASS_IN_GROUP_END(ClangExprPropagation, "clang-propagate",
  "Expression Propagation (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

FunctionPass * createClangCopyPropagation() {
  return new ClangExprPropagation();
}

void ClangExprPropagation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.setPreservesAll();
}

namespace {
struct Usage {};
struct Definition {};
struct Available {};
struct Candidate {};
struct Access {};

class DefUseVisitor : public RecursiveASTVisitor<DefUseVisitor> {
public:
  /// Map from declaration to a possible replacement string and list of
  /// declarations which are used in this string.
  using ReplacementT = DenseMap<Decl *,
    std::tuple<SmallString<16>, SmallVector<NamedDecl *, 4>>,
    DenseMapInfo<Decl *>,
    TaggedDenseMapTuple<
      bcl::tagged<Decl *, Usage>,
      bcl::tagged<SmallString<16>, Definition>,
      bcl::tagged<SmallVector<NamedDecl *, 4>, Access>>>;

  /// This is a mapped value in DefLocationMap which is a map from instruction
  /// which defines memory to an instruction which use this definition.
  ///
  /// This mapped value is a map from instruction which uses a memory location
  /// to a list of candidates which can be replaced and a list of declarations
  /// which have the same value at definition and usage point. We have a list
  /// of candidates because at IR-level we do not know which of this variable
  /// has been accessed in a user.
  using DeclUseLocationMap = DenseMap<DILocation *,
    std::tuple<SmallVector<NamedDecl *, 4>, SmallPtrSet<NamedDecl *, 4>>,
    DILocationMapInfo,
    TaggedDenseMapTuple<
      bcl::tagged<DILocation *, Usage>,
      bcl::tagged<SmallVector<NamedDecl *, 4>, Candidate>,
      bcl::tagged<SmallPtrSet<NamedDecl *, 4>, Available>>>;

private:
  /// Map from instruction which uses a memory location to a definition which
  /// can be propagated to replace operand in this instruction.
  using UseLocationMap = PersistentMap<
    DILocation *, ReplacementT, DILocationMapInfo,
    TaggedDenseMapPair<
      bcl::tagged<DILocation *, Usage>,
      bcl::tagged<ReplacementT, Definition>>>;

  /// Map from instruction which defines a memory to an instruction which
  /// uses this definition.
  using DefLocationMap = DenseMap<
    DILocation *, DeclUseLocationMap,
    DILocationMapInfo,
    TaggedDenseMapPair<
      bcl::tagged<DILocation *, Definition>,
      bcl::tagged<DeclUseLocationMap, Usage>>>;

  /// Diagnostics attached to a replacement candidate.
  using DiagnosticT = DenseMap<
    Decl *, SmallVector<std::pair<SourceLocation, unsigned>, 2>,
    DenseMapInfo<Decl *>,
    TaggedDenseMapPair<
      bcl::tagged<Decl *, Usage>,
      bcl::tagged<
        SmallVector<std::pair<SourceLocation, unsigned>, 2>, Diagnostic>>>;

  /// Map from propagation point to a list of diagnostics.
  using DiagnosticMap = PersistentMap<
    DILocation *, DiagnosticT, DILocationMapInfo,
    TaggedDenseMapPair<
      bcl::tagged<DILocation *, Usage>,
      bcl::tagged<DiagnosticT, Diagnostic>>>;

public:
  DefUseVisitor(ClangTransformationContext &TfmCtx,
      const ASTImportInfo &ImportInfo,
      const ClangGlobalInfo::RawInfo &RawInfo,
      const GlobalInfoExtractor &GlobalInfo) :
    mTfmCtx(TfmCtx),
    mImportInfo(ImportInfo),
    mRawInfo(RawInfo),
    mGlobalInfo(GlobalInfo),
    mRewriter(TfmCtx.getRewriter()),
    mContext(TfmCtx.getContext()),
    mSrcMgr(TfmCtx.getRewriter().getSourceMgr()),
    mLangOpts(TfmCtx.getRewriter().getLangOpts()) {}

  ClangTransformationContext & getTfmContext() noexcept { return mTfmCtx; }

  /// Return set of replacements in subtrees of a tree which represents
  /// expression at a specified location (create empty set if it does not
  /// exist).
  ///
  /// Note, that replacement for a subtree overrides a replacement for a tree.
  ReplacementT & getReplacement(DebugLoc Use) {
    assert(Use && Use.get() && "Use location must not be null!");
    auto UseItr = mUseLocs.try_emplace(Use.get()).first;
    return UseItr->get<Definition>();
  }

  DefLocationMap::value_type & getDeclReplacement(DebugLoc Def) {
    assert(Def && Def.get() && "Def location must not be null!");
    return *mDefLocs.try_emplace(Def.get()).first;
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    enterInScope(nullptr);
    auto Res = RecursiveASTVisitor::TraverseFunctionDecl(FD);
    exitFromScope(nullptr);
    return Res;
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    Pragma P(*S);
    if (P) {
      // Search for propagate clause and disable renaming in other pragmas.
      if (findClause(P, ClauseId::Propagate, mClauses)) {
        llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
        auto IsPossible = pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts,
                                              mImportInfo, ToRemove);
        if (!IsPossible.first)
          if (IsPossible.second & PragmaFlags::IsInMacro)
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
              tsar::diag::warn_remove_directive_in_macro);
          else if (IsPossible.second & PragmaFlags::IsInHeader)
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
              tsar::diag::warn_remove_directive_in_include);
          else
            toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
              tsar::diag::warn_remove_directive);
        Rewriter::RewriteOptions RemoveEmptyLine;
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
        /// set to true then removing (in RewriterBuffer) works incorrect.
        RemoveEmptyLine.RemoveLineIfEmpty = false;
        for (auto SR : ToRemove)
          mRewriter.RemoveText(SR, RemoveEmptyLine);
      }
      return true;
    }
    excludeIfAssignment(S);
    auto Loc = isa<Expr>(S) ? cast<Expr>(S)->getExprLoc() : S->getBeginLoc();
    bool Res = false;
    if (Loc.isValid() && Loc.isFileID()) {
      auto PLoc = mSrcMgr.getPresumedLoc(Loc);
      auto UseItr = mUseLocs.find_as(PLoc);
      if (UseItr != mUseLocs.end()) {
        LLVM_DEBUG(
            dbgs() << "[PROPAGATION]: traverse propagation target at ";
            Loc.dump(mSrcMgr); dbgs() << "\n");
        mReplacement.push(UseItr);
        // Create list of diagnostics if it is not exist.
        auto DiagItr = mDiags.try_emplace(UseItr->get<Usage>()).first;
        mReplacementDiag.push(DiagItr);
        Res = RecursiveASTVisitor::TraverseStmt(S);
        mReplacement.pop();
        mReplacementDiag.pop();
      } else {
        Res = RecursiveASTVisitor::TraverseStmt(S);
      }
    } else {
      Res = RecursiveASTVisitor::TraverseStmt(S);
    }
    return Res;
  }

  bool TraverseCompoundStmt(CompoundStmt *S) {
    bool Res = false;
    auto StashPropagateScope = enterInScope(S);
    if (mClauses.empty()) {
      Res = RecursiveASTVisitor::TraverseCompoundStmt(S);
    } else {
      mClauses.clear();
      bool StashPropagateState = mActivePropagate;
      if (!mActivePropagate) {
        if (hasMacro(S)) {
          Res = RecursiveASTVisitor::TraverseCompoundStmt(S);
          exitFromScope(StashPropagateScope);
          return Res;
        }
        mActivePropagate = true;
        SmallString<64> NoMacroPragma;
        getPragmaText(ClauseId::AssertNoMacro, NoMacroPragma);
        mRewriter.InsertTextBefore(S->getBeginLoc(), NoMacroPragma);
      }
      auto Res = RecursiveASTVisitor::TraverseCompoundStmt(S);
      mActivePropagate = StashPropagateState;
    }
    exitFromScope(StashPropagateScope);
    return Res;
  }

  bool TraverseForStmt(ForStmt *S) {
    auto StashPropagateScope = enterInScope(S);
    bool Res = RecursiveASTVisitor::TraverseForStmt(S);
    exitFromScope(StashPropagateScope);
    return Res;
  }

  bool TraverseDoStmt(DoStmt *S) {
    auto StashPropagateScope = enterInScope(S);
    bool Res = RecursiveASTVisitor::TraverseDoStmt(S);
    exitFromScope(StashPropagateScope);
    return Res;
  }

  bool TraverseWhileStmt(WhileStmt *S) {
    auto StashPropagateScope = enterInScope(S);
    bool Res = RecursiveASTVisitor::TraverseWhileStmt(S);
    exitFromScope(StashPropagateScope);
    return Res;
  }

  bool TraverseIfStmt(IfStmt *S) {
    auto StashPropagateScope = enterInScope(S);
    bool Res = RecursiveASTVisitor::TraverseIfStmt(S);
    exitFromScope(StashPropagateScope);
    return Res;
  }

  bool TraverseSwitchStmt(SwitchStmt *S) {
    auto StashPropagateScope = enterInScope(S);
    bool Res = RecursiveASTVisitor::TraverseSwitchStmt(S);
    exitFromScope(StashPropagateScope);
    return Res;
  }

  bool TraverseBinaryOperator(clang::BinaryOperator *Expr) {
    if (!Expr || !Expr->isAssignmentOp())
      return RecursiveASTVisitor::TraverseBinaryOperator(Expr);
    auto PLoc = mSrcMgr.getPresumedLoc(Expr->getRHS()->getExprLoc());
    auto DefItr = mDefLocs.find_as(PLoc);
    if (DefItr == mDefLocs.end())
      return RecursiveASTVisitor::TraverseBinaryOperator(Expr);
    auto Res = TraverseStmt(Expr->getLHS());
    bool StashCollectDecls;
    auto DeclRefIdx = startCollectDeclRef(StashCollectDecls);
    Res &= TraverseStmt(Expr->getRHS());
    checkAssignmentRHS(Expr->getRHS(), *DefItr, DeclRefIdx);
    restoreCollectDeclRef(StashCollectDecls);
    return Res;
  }

  bool VisitStmt(Stmt *S) {
    if (mClauses.empty())
      return RecursiveASTVisitor::VisitStmt(S);
    if (auto *DS = dyn_cast<DeclStmt>(S)) {
      bool HasNamedDecl = false;
      for (auto *D : DS->decls())
        if (auto *ND = dyn_cast<NamedDecl>(D)) {
          HasNamedDecl = true;
          mDeclsToPropagate.insert(ND);
        }
      if (!HasNamedDecl)
        toDiag(mContext.getDiagnostics(), mClauses.front()->getBeginLoc(),
          tsar::diag::warn_unexpected_directive);
      assert(mDeclPropagateScope.first && "Top level scope must not be null!");
      if (hasMacro(mDeclPropagateScope.first)) {
        mDeclsToPropagate.clear();
        return RecursiveASTVisitor::VisitStmt(S);
      }
      SmallString<64> NoMacroPragma("\n");
      getPragmaText(ClauseId::AssertNoMacro, NoMacroPragma);
      mRewriter.InsertTextBefore(
        mDeclPropagateScope.first->getBeginLoc(), NoMacroPragma);
      if (!mDeclPropagateScope.second) {
        // Propagation scope is a function body, add extra {} around a pragma.
        mRewriter.InsertTextBefore(
          mDeclPropagateScope.first->getBeginLoc(), "{");
        mRewriter.InsertTextAfter(mDeclPropagateScope.first->getEndLoc(), "}");
      }
    } else if (!isa<CompoundStmt>(S)) {
      toDiag(mContext.getDiagnostics(), mClauses.front()->getBeginLoc(),
        tsar::diag::warn_unexpected_directive);
    }
    mClauses.clear();
    return RecursiveASTVisitor::VisitStmt(S);
  }

  bool VisitNamedDecl(NamedDecl *ND) {
    auto Pair =
      mNameToVisibleDecl.try_emplace(ND->getDeclName(), mVisibleDecls.size());
    if (Pair.second) {
      mVisibleDecls.emplace_back();
    }
    mVisibleDecls[Pair.first->second].push_back(ND);
    LLVM_DEBUG(dbgs() << "[PROPAGATION]: push declaration to stack "
                      << Pair.first->second << ": "; ND->getDeclName().dump());
    assert(!mDeclsInScope.empty() && "At least one scope must exist!");
    mDeclsInScope.top().push_back(Pair.first->second);
    return true;
  }

  bool TraverseVarDecl(VarDecl *VD) {
    if (isa<ParmVarDecl>(VD) || !VD->hasInit())
      return RecursiveASTVisitor::TraverseVarDecl(VD);
    auto *InitExpr = VD->getInit();
    auto PLoc = mSrcMgr.getPresumedLoc(InitExpr->getExprLoc());
    auto DefItr = mDefLocs.find_as(PLoc);
    if (DefItr == mDefLocs.end())
      return RecursiveASTVisitor::TraverseVarDecl(VD);
    bool StashCollectDecls;
    auto DeclRefIdx = startCollectDeclRef(StashCollectDecls);
    auto Res = TraverseStmt(InitExpr);
    checkAssignmentRHS(InitExpr, *DefItr, DeclRefIdx);
    restoreCollectDeclRef(StashCollectDecls);
    return Res && WalkUpFromVarDecl(VD);
  }

  bool VisitDeclRefExpr(DeclRefExpr *Ref) {
    //TODO (kaniandr@gmail.com): add `assert nomacro` after propagation
    storeDeclRef(Ref);
    if (mReplacement.empty() || mNotPropagate.count(Ref))
      return true;
    auto ND = Ref->getFoundDecl();
    if (!mDeclsToPropagate.count(ND) && !mActivePropagate)
      return true;
    auto ReplacementItr = mReplacement.top()->get<Definition>().find(ND);
    if (ReplacementItr == mReplacement.top()->get<Definition>().end()) {
      auto DiagItr = mReplacementDiag.top()->get<Diagnostic>().find(ND);
      if (DiagItr != mReplacementDiag.top()->get<Diagnostic>().end()) {
        toDiag(mContext.getDiagnostics(), Ref->getLocation(),
          tsar::diag::warn_disable_propagate);
        for (auto &Diag : DiagItr->get<Diagnostic>())
          toDiag(mSrcMgr.getDiagnostics(), Diag.first, Diag.second);
      }
      return true;
    }
    auto Loc = mSrcMgr.getDecomposedExpansionLoc(Ref->getLocation());
    auto IncludeToFID = mSrcMgr.getDecomposedIncludedLoc(Loc.first).first;
    if (IncludeToFID.isValid() && !mImportInfo.MainFiles.count(Loc.first)) {
      toDiag(mSrcMgr.getDiagnostics(), Ref->getLocation(),
        tsar::diag::warn_disable_propagate_in_include);
      return true;
    }
    for (auto *AccessDecl : ReplacementItr->get<Access>()) {
      auto Itr = mNameToVisibleDecl.find(AccessDecl->getDeclName());
      if ((Itr == mNameToVisibleDecl.end() ||
           mVisibleDecls[Itr->second].empty() ||
           mVisibleDecls[Itr->second].back() != AccessDecl) &&
          !mGlobalInfo.findOutermostDecl(AccessDecl)) {
        toDiag(mContext.getDiagnostics(), Ref->getLocation(),
          tsar::diag::warn_disable_propagate);
        toDiag(mContext.getDiagnostics(), AccessDecl->getLocation(),
          tsar::diag::note_propagate_hidden_dep);
        LLVM_DEBUG(dbgs() << "[PROPAGATION]: disable propagate due to "
                          << "hidden declaration of ";
                   AccessDecl->getDeclName().dump());
        return true;
      }
    }
    LLVM_DEBUG(dbgs() << "[PROPAGATION]: replace variable in [";
               Ref->getBeginLoc().dump(mSrcMgr); dbgs() << ", ";
               Ref->getEndLoc().dump(mSrcMgr);
               dbgs() << "] with '" << ReplacementItr->get<Definition>()
               << "'\n");
    mRewriter.ReplaceText(
      SourceRange(Ref->getBeginLoc(), Ref->getEndLoc()),
      ReplacementItr->get<Definition>());
    return true;
  }

private:
  /// Returns true and emit warning if there is macro in specified statement.
  bool hasMacro(Stmt *S) {
    bool HasMacro = false;
    for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawInfo.Macros,
      [&HasMacro, this](clang::SourceLocation Loc) {
        if (!HasMacro) {
          toDiag(mContext.getDiagnostics(), Loc,
            tsar::diag::warn_propagate_macro_prevent);
          HasMacro = true;
      }
    });
    return HasMacro;
  }

  /// Set flag to collect declaration references, return number of already
  /// collected declarations.
  std::size_t startCollectDeclRef(bool &StashCollectDecls) {
    StashCollectDecls = mCollectDecls;
    mCollectDecls = true;
    return mDeclRefs.size();
  }

  /// Restore flag from a stashed value and clear list of collected declarations
  /// if stashed value is 'false'.
  void restoreCollectDeclRef(bool StashCollectDecls) {
    mCollectDecls = StashCollectDecls;
    if (!mCollectDecls)
      mDeclRefs.clear();
  }

  /// Remember referenced declaration.
  void storeDeclRef(DeclRefExpr *Expr) {
    if (mCollectDecls)
      mDeclRefs.push_back(Expr->getFoundDecl());
  }

  /// Disable propagation for declaration references which obtain new value
  /// in a simple assignment-like statements.
  ///
  /// Do not replace variables in increment/decrement because this operators
  /// change an accessed variable:
  ///`X = I; ++X; return I;` is not equivalent `X = I; ++I; return I`
  /// However, if binary operators or array subscripts expressions is used in
  /// left-hand side of assignment to compute reference to the assigned memory,
  /// PROPAGATION is still possible.
  void excludeIfAssignment(Stmt *S) {
    if ((isa<clang::BinaryOperator>(S) &&
         cast<clang::BinaryOperator>(S)->isAssignmentOp()) ||
        (isa<clang::UnaryOperator>(S)) &&
         cast<clang::UnaryOperator>(S)->isIncrementDecrementOp()) {
      DeclRefExpr *AssignDeclRef = nullptr;
      Stmt *Curr = S;
      for (auto Itr = Curr->child_begin(); Itr != Curr->child_end();
           Curr = *Itr, Itr = Curr->child_begin()) {
        if (auto *Ref = dyn_cast<DeclRefExpr>(*Itr))
          AssignDeclRef = Ref;
        else if (isa<clang::BinaryOperator>(*Itr) ||
                 isa<ArraySubscriptExpr>(*Itr) ||
                 isa<CallExpr>(*Itr))
          return;
      }
      assert(AssignDeclRef && "Target of assignment must not be null!");
      mNotPropagate.insert(AssignDeclRef);
      LLVM_DEBUG(dbgs() << "[PROPAGATION]: disable PROPAGATION in "
                           "left-hand side of assignment at ";
                 AssignDeclRef->getBeginLoc().dump(mSrcMgr); dbgs() << "\n");
    }
  }

  /// Determine `RHS`-based replacement to substitute use in DefToUse pair.
  ///
  /// \param [in] RHS Right-hand side of assignment or variable initialization.
  /// \param [in] DefToUse Pair of definition and usage which is a PROPAGATION
  /// point.
  /// \param [in] DeclRefIdx First declaration in mDeclRefs which is used in RHS.
  ///
  /// \pre RHS is located at DefToUse->get<Definition>() source code location.
  /// \post If all declarations accessed in RHS are available at PROPAGATION
  /// point then update mUseLocs map and set RHS as a possible replacement for
  /// candidates mentioned in DefToUse pair.
  void checkAssignmentRHS(Expr *RHS, DefLocationMap::value_type &DefToUse,
      std::size_t DeclRefIdx) {
    LLVM_DEBUG(dbgs() << "[PROPAGATION]: find definition at ";
               RHS->getExprLoc().dump(mSrcMgr); dbgs() << "\n");
    auto DefSR = RHS->getSourceRange();
    SmallString<16> DefStr;
    ("(" + Lexer::getSourceText(
      CharSourceRange::getTokenRange(DefSR), mSrcMgr,mLangOpts) + ")")
        .toStringRef(DefStr);
    bool IsAllDeclRefAvailable = true;
    SmallPtrSet<NamedDecl *, 8> RHSDecls;
    for (auto &U : DefToUse.get<Usage>()) {
      for (auto IdxE = mDeclRefs.size(); DeclRefIdx < IdxE; ++DeclRefIdx) {
        auto *ND = cast<NamedDecl>(mDeclRefs[DeclRefIdx]->getCanonicalDecl());
        RHSDecls.insert(ND);
        if (isa<EnumConstantDecl>(ND))
          continue;
        if (!U.get<Available>().count(ND)) {
          IsAllDeclRefAvailable = false;
          auto UsageDiagItr = mDiags.try_emplace(U.get<Usage>()).first;
          for (auto *D : U.get<Candidate>()) {
            auto DiagInfo = UsageDiagItr->get<Diagnostic>().try_emplace(D);
            if (!DiagInfo.second)
              continue;
            DiagInfo.first->get<Diagnostic>().emplace_back(
              RHS->getExprLoc(), tsar::diag::note_propagate_not_available);
            DiagInfo.first->get<Diagnostic>().emplace_back(
              ND->getLocation(), tsar::diag::note_propagate_new_value);
          }
          break;
        }
      }
      if (IsAllDeclRefAvailable) {
        auto &Candidates =
          mUseLocs.try_emplace(U.get<Usage>()).first->get<Definition>();
        for (auto *D : U.get<Candidate>()) {
          if (RHSDecls.count(D)) {
            auto UsageDiagItr = mDiags.try_emplace(U.get<Usage>()).first;
            auto DiagInfo = UsageDiagItr->get<Diagnostic>().try_emplace(D);
            if (DiagInfo.second)
              DiagInfo.first->get<Diagnostic>().emplace_back(
                RHS->getExprLoc(), tsar::diag::note_propagate_recurrence);
            continue;
          }
          auto Info = Candidates.try_emplace(D);
          Info.first->get<Definition>() = DefStr;
          Info.first->get<Access>().assign(RHSDecls.begin(), RHSDecls.end());
        }
      }
    }
  }

  Stmt * enterInScope(Stmt *S) {
    LLVM_DEBUG(dbgs() << "[PROPAGATION]: enter in scope\n");
    mDeclsInScope.push({});
    auto *StashPropagateScope = mDeclPropagateScope.second;
    if (mDeclsToPropagate.empty()) {
      mDeclPropagateScope.second = mDeclPropagateScope.first;
      mDeclPropagateScope.first = S;
    }
    return StashPropagateScope;
  }

  void exitFromScope(Stmt *StashPropagateScope) {
    assert(!mDeclsInScope.empty() && "At least one scope must exist!");
    LLVM_DEBUG(dbgs() << "[PROPAGATION]: exit from scope\n");
    for (auto Idx : mDeclsInScope.top()) {
      LLVM_DEBUG(dbgs() << "[PROPAGATION]: pop declaration from stack "
                        << Idx << ": ";
                 mVisibleDecls[Idx].back()->getDeclName().dump());
      mVisibleDecls[Idx].pop_back();
    }
    mDeclsInScope.pop();
    if (StashPropagateScope != mDeclPropagateScope.second) {
      // `mDeclPropagateScope` has been changed in `enterInScope()` method.
      mDeclPropagateScope.first = mDeclPropagateScope.second;
      mDeclPropagateScope.second = StashPropagateScope;
    }
  }

  ClangTransformationContext &mTfmCtx;
  const ASTImportInfo &mImportInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  const ClangGlobalInfo::RawInfo &mRawInfo;
  const GlobalInfoExtractor &mGlobalInfo;
  UseLocationMap mUseLocs;
  DefLocationMap mDefLocs;
  DiagnosticMap mDiags;

  /// Top of the stack contains definitions which can be used to replace
  /// references in a currently processed statement.
  std::stack<UseLocationMap::persistent_iterator> mReplacement;
  std::stack<DiagnosticMap::persistent_iterator> mReplacementDiag;

  /// Collection of stacks of declarations with the same name. A top declaration
  /// is currently visible.
  std::vector<TinyPtrVector<NamedDecl *>> mVisibleDecls;

  /// Map from declaration name to index in mVisibleDecls container.
  DenseMap<DeclarationName, unsigned> mNameToVisibleDecl;

  /// Collection of entities declared in a scope.
  ///
  /// Top of the stack is a list of indexes in mVisibleDecls container. A top
  /// declaration in mVisibleDecls with some of this indexes is declared in
  /// scope at the top of mDeclsInScope stack.
  std::stack<SmallVector<unsigned, 1>> mDeclsInScope;

  /// Already visited references to declarations.
  SmallVector<NamedDecl *, 8> mDeclRefs;
  /// If 'true' declarations from DeclRefExpr should be stored in mDeclRefs.
  bool mCollectDecls = false;

  /// References which should not be propagated.
  DenseSet<DeclRefExpr *> mNotPropagate;

  SmallVector<Stmt *, 1> mClauses;

  /// Declarations which is marked wit 'propagate' clause.
  SmallPtrSet<NamedDecl *, 16> mDeclsToPropagate;

  /// Innermost scope which contains declarations with attached 'propagate'
  /// clause and a parent scope (in case of function body parent scope is null).
  std::pair<Stmt *, Stmt *> mDeclPropagateScope = { nullptr, nullptr };
  bool mActivePropagate = false;
};

/// Find declarations which is used in `DI` and which is available in `UI`.
///
/// \post Store result in `UseItr` container. Note, that if there is an
/// instruction which prevents PROPAGATION of DI into UI list of available
/// declarations are cleaned.
void findAvailableDecls(Instruction &DI, Instruction &UI,
    const ClangDIMemoryMatcherPass::DIMemoryMatcher &DIMatcher,
    unsigned DWLang, const DominatorTree &DT,
    ClangTransformationContext &TfmCtx,
    DefUseVisitor::DeclUseLocationMap::iterator &UseItr) {
  // Add Def instruction to list of operands because if it is a call
  // we should check that it has no side effect.
  SmallPtrSet<Value *, 16> Ops({ &DI});
  SmallVector<Value *, 16> OpWorkList({ &DI});
  while (!OpWorkList.empty()) {
    if (auto *CurrOp = dyn_cast<User>(OpWorkList.pop_back_val()))
      for (auto &Op : CurrOp->operands())
        if (Ops.insert(Op).second)
          OpWorkList.push_back(Op);
  }
  for (auto *Op : Ops) {
    auto *Call = dyn_cast<CallBase>(Op);
    if ((Call && !Call->onlyReadsMemory() && !Call->doesNotAccessMemory()) ||
        (Call && !Call->doesNotThrow())) {
      LLVM_DEBUG(dbgs() << "[PROPAGATION]: disable due to ";
        TSAR_LLVM_DUMP(Op->dump()));
      // Call may have side effect and prevent PROPAGATION.
      UseItr->get<Available>().clear();
      break;
    }
    if (auto *F = dyn_cast<Function>(Op)) {
      auto *FD = TfmCtx.getDeclForMangledName(F->getName());
      if (FD) {
        LLVM_DEBUG(dbgs() << "[PROPAGATION]: assignment may use available"
                             " function '" << F->getName() << "'\n");
        auto *CFD = FD->getCanonicalDecl();
        if (CFD && isa<NamedDecl>(CFD))
          UseItr->get<Available>().insert(cast<NamedDecl>(CFD));
      }
      continue;
    }
    SmallVector<DIMemoryLocation, 4> DIOps;
    if (auto *GV = dyn_cast<GlobalVariable>(Op)) {
      // If type is pointer then a global variable may be reassigned before
      // propagation point (user instruction).
      if (!GV->getValueType()->isPointerTy())
        findGlobalMetadata(GV, DIOps);
    } else {
      findMetadata(Op, makeArrayRef(&UI), DT, DIOps);
    }
    for (auto &DIOp : DIOps) {
      SmallString<16> OpStr;
      if (DIOp.isValid()) {
        auto DIToDeclItr = DIMatcher.find<MD>(DIOp.Var);
        if (DIToDeclItr == DIMatcher.end())
          continue;
        UseItr->get<Available>().insert(DIToDeclItr->get<AST>());
        LLVM_DEBUG(dbgs() << "[PROPAGATION]: assignment may use available"
                             " location ";
                   printDILocationSource(DWLang, DIOp, dbgs());
                   dbgs() << " declared at line " << DIOp.Var->getLine()
                          << " in " << DIOp.Var->getFilename() << "\n");
      }
    }
  }
}

/// If `Def` may be an assignment in a source code then check is it possible
/// to perform PROPAGATION.
///
/// (1) This function calculate candidates which can be replaced with this
/// assignment.
/// (2) This function determine declarations which can be used in a PROPAGATION
/// point (available variable).
/// (3) Determined values are stored in a `Visitor` for further processing.
void rememberPossibleAssignment(Value &Def, Instruction &UI,
    ArrayRef<DIMemoryLocation> DILocs,
    const ClangDIMemoryMatcherPass::DIMemoryMatcher &DIMatcher,
    unsigned DWLang, const DominatorTree &DT,
    DefUseVisitor &Visitor) {
  auto *Inst = dyn_cast<Instruction>(&Def);
  if (!Inst || !Inst->getDebugLoc())
    return;
  LLVM_DEBUG(dbgs() << "[PROPAGATION]: remember possible assignment at ";
             TSAR_LLVM_DUMP(Inst->getDebugLoc().dump()); dbgs() << "\n");
  auto &DeclToReplace = Visitor.getDeclReplacement(Inst->getDebugLoc());
  auto UseItr = DeclToReplace.get<Usage>().
    try_emplace(UI.getDebugLoc().get()).first;
  for (auto &DILoc : DILocs) {
    if (!DILoc.isValid() || DILoc.Template || DILoc.Expr->getNumElements() != 0)
      continue;
    auto DIToDeclItr = DIMatcher.find<MD>(DILoc.Var);
    if (DIToDeclItr == DIMatcher.end())
      continue;
    UseItr->get<Candidate>().push_back(DIToDeclItr->get<AST>());
    LLVM_DEBUG(dbgs() << "[PROPAGATION]: may replace ";
               printDILocationSource(DWLang, DILoc, dbgs());
               dbgs() << "\n");
  }
  if (UseItr->get<Candidate>().empty())
    return;
  findAvailableDecls(*Inst, UI,
    DIMatcher, DWLang, DT, Visitor.getTfmContext(), UseItr);
}
}

bool ClangExprPropagation::unparseReplacement(
    const Value &Def, const Use &Use, const DIMemoryLocation *DIDef,
    unsigned DWLang, const DIMemoryLocation &DIUse,
    SmallVectorImpl<char> &DefStr) {
  auto unparseArrayAccess = [DWLang, &Use, &DefStr](Optional<DIMemoryLocation> &MD) {
    if (MD && MD->isValid() && !MD->Template) {
      if (unparseToString(DWLang, *MD, DefStr, false)) {
        unsigned NumberOfDims = 0;
        if (auto *UseGEP{dyn_cast<GEPOperator>(Use.getUser())};
            UseGEP &&
            GEPOperator::getPointerOperandIndex() == Use.getOperandNo())
          NumberOfDims = 1 + dimensionsNum(UseGEP->getSourceElementType());
        else if (auto *UseLI{dyn_cast<LoadInst>(Use.getUser())};
                 UseLI &&
                 LoadInst::getPointerOperandIndex() == Use.getOperandNo())
          NumberOfDims = 1 + dimensionsNum(UseLI->getType());
        else if (auto *UseSI{dyn_cast<StoreInst>(Use.getUser())};
                 UseSI &&
                 StoreInst::getPointerOperandIndex() == Use.getOperandNo())
          NumberOfDims = 1 + dimensionsNum(UseSI->getValueOperand()->getType());
        else
          return false;
        for (; NumberOfDims > 0 && DefStr.size() > 3; --NumberOfDims) {
          auto Size = DefStr.size();
          if (DefStr[Size - 1] != ']' || DefStr[Size - 2] != '0' ||
              DefStr[Size - 3] != '[')
            return false;
          DefStr.resize(Size - 3);
        }
        return NumberOfDims == 0;
      }
    }
    return false;
  };
  if (auto *C = dyn_cast<Constant>(&Def)) {
    if (auto *CF = dyn_cast<Function>(&Def)) {
      auto *D = mTfmCtx->getDeclForMangledName(CF->getName());
      auto *ND = dyn_cast_or_null<NamedDecl>(D);
      if (!ND)
        return false;
      DefStr.assign(ND->getName().begin(), ND->getName().end());
    } else if (auto *CFP = dyn_cast<ConstantFP>(&Def)) {
      CFP->getValueAPF().toString(DefStr);
    } else if (auto *CInt = dyn_cast<ConstantInt>(&Def)) {
      auto *Ty = dyn_cast_or_null<DIBasicType>(
        DIUse.Var->getType());
      if (!Ty)
        return false;
      DefStr.clear();
      if (Ty->getEncoding() == dwarf::DW_ATE_signed)
        CInt->getValue().toStringSigned(DefStr);
      else if (Ty->getEncoding() == dwarf::DW_ATE_unsigned)
        CInt->getValue().toStringUnsigned(DefStr);
      else
        return false;
    } else if (auto *GEP = dyn_cast<GEPOperator>(&Def)) {
      auto MD = buildDIMemory(MemoryLocation::getAfter(GEP), GEP->getContext(),
                              *mDL, *mDT);
      return unparseArrayAccess(MD);
    } else if (auto *GV = dyn_cast<GlobalVariable>(&Def)) {
      auto MD = buildDIMemory(MemoryLocation::getAfter(GV), GV->getContext(),
                              *mDL, *mDT);
      return unparseArrayAccess(MD);
    } else {
      return false;
    }
    return true;
  }
  if (!DIDef || !DIDef->isValid() || DIDef->Template)
    return false;
  if (!unparseToString(DWLang, *DIDef, DefStr, false))
    return false;
  return true;
}

bool ClangExprPropagation::runOnFunction(Function &F) {
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  mTfmCtx = TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                          TfmInfo->getContext(*CU))
                    : nullptr;
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    F.getContext().emitError("cannot transform sources"
                              ": transformation context is not available");
    return false;
  }
  auto FuncDecl = mTfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto DWLang = getLanguage(F);
  if (!DWLang)
    return false;
  auto &SrcMgr = mTfmCtx->getRewriter().getSourceMgr();
  if (SrcMgr.getFileCharacteristic(FuncDecl->getBeginLoc()) != SrcMgr::C_User)
    return false;
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto *M = F.getParent();
  mDL = &M->getDataLayout();
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &DIMatcher = getAnalysis<ClangDIMemoryMatcherPass>().getMatcher();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto *GI{GIP.getGlobalInfo(mTfmCtx)};
  assert(GI && "Global information must not be null!");
  DefUseVisitor Visitor(*mTfmCtx, *ImportInfo, GI->RI, GI->GIE);
  DenseSet<Value *> WorkSet;
  // Search for PROPAGATION candidates.
  for (auto &I : instructions(F)) {
    auto DbgVal = dyn_cast<DbgValueInst>(&I);
    if (!DbgVal)
      continue;
    auto *Def = DbgVal->getValue();
    if (!Def || isa<UndefValue>(Def))
      continue;
    if (!WorkSet.insert(Def).second)
      continue;
    for (auto &U : Def->uses()) {
      if (!isa<Instruction>(U.getUser()))
        break;
      auto *UI = cast<Instruction>(U.getUser());
      if (!UI->getDebugLoc())
        continue;
      SmallVector<DIMemoryLocation, 4> DILocs;
      auto DIDef = findMetadata(Def, makeArrayRef(UI), *mDT, DILocs);
      auto DIDefToDeclItr = DIDef ?
        DIMatcher.find<MD>(DIDef->Var) : DIMatcher.end();
      if (DIDef && DIDefToDeclItr == DIMatcher.end())
        continue;
      LLVM_DEBUG(dbgs() << "[PROPAGATION]: remember instruction " << *UI
                        << " as a root for replacement at ";
                 UI->getDebugLoc().print(dbgs()); dbgs() << "\n");
      rememberPossibleAssignment(
        *Def, *UI, DILocs, DIMatcher, *DWLang, *mDT, Visitor);
      if (DILocs.empty())
        continue;
      auto &Candidates = Visitor.getReplacement(UI->getDebugLoc());
      for (auto &DILoc : DILocs) {
        //TODO (kaniandr@gmail.com): it is possible to propagate not only
        // variables, for example, accesses to members of a structure can be
        // also propagated. However, it is necessary to update processing of
        // AST in DefUseVisitor for members.
        if (DILoc.Template || DILoc.Expr->getNumElements() != 0)
          continue;
        auto DIToDeclItr = DIMatcher.find<MD>(DILoc.Var);
        if (DIToDeclItr == DIMatcher.end())
          continue;
        SmallString<16> DefStr, UseStr;
        if (!unparseReplacement(*Def, U, DIDef ? &*DIDef : nullptr,
              *DWLang, DILoc, DefStr))
          continue;
        if (DefStr == DILoc.Var->getName())
          continue;
        LLVM_DEBUG(dbgs() << "[PROPAGATION]: find source-level definition "
                          << DefStr << " for " << *Def << " to replace ";
                   printDILocationSource(*DWLang, DILoc, dbgs());
                   dbgs() << "\n");
        auto Info = Candidates.try_emplace(DIToDeclItr->get<AST>());
        assert((Info.second || Info.first->get<Definition>() == DefStr) &&
          "It must be new replacement!");
        Info.first->get<Definition>() = DefStr;
        if (DIDef)
          Info.first->get<Access>().push_back(DIDefToDeclItr->get<AST>());
      }
    }
  }
  Visitor.TraverseDecl(FuncDecl);
  return false;
}
