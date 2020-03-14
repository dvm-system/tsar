#include "tsar/Transform/Clang/LoopSwap.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>
// required for function pass
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Support/PassProvider.h"
#include "llvm/IR/LegacyPassManager.h"
//
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Support/GlobalOptions.h"
//
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "llvm/Analysis/LoopInfo.h"
using namespace llvm;
using namespace clang;
using namespace tsar;
using namespace std;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-swap"

char ClangLoopSwapPass::ID = 0;

namespace {
using LoopSwapPassProvider =
    FunctionPassProvider<TransformationEnginePass, CanonicalLoopPass,
                         DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
                         DIDependencyAnalysisPass, DIEstimateMemoryPass,
                         GlobalOptionsImmutableWrapper,
                         MemoryMatcherImmutableWrapper, ClangPerfectLoopPass>;

class LoopSwapPassGroupInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &PM) const override {
    PM.add(createMemoryMatcherPass());
    PM.add(createDIMemoryTraitPoolStorage());
    PM.add(createDIMemoryEnvironmentStorage());
  }
};
} // namespace

INITIALIZE_PROVIDER_BEGIN(LoopSwapPassProvider, "loop-swap-provider",
                          "Loop swap provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PROVIDER_END(LoopSwapPassProvider, "loop-swap-provider",
                        "Loop swap provider")

INITIALIZE_PASS_IN_GROUP_BEGIN(
    ClangLoopSwapPass, "clang-swap", "", false, false,
    tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(LoopSwapPassGroupInfo)
INITIALIZE_PASS_DEPENDENCY(LoopSwapPassProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(
    ClangLoopSwapPass, "clang-swap", "", false, false,
    tsar::TransformationQueryManager::getPassRegistry())

void ClangLoopSwapPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<LoopSwapPassProvider>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createClangLoopSwapPass() {
  return new ClangLoopSwapPass();
}

namespace {
class SwapClauseInfo {
public:
  SwapClauseInfo(clang::DeclRefExpr *Array,
               llvm::SmallVector<Expr *, 1> Dimensions, bool Strict)
      : mStrict(Strict), mArray(Array), mDimensions(Dimensions),
        mHasError(false), mDiagKind(0) {}
  llvm::SmallVector<clang::DeclRefExpr *, 1> &getDeclRefs() {
    return mDeclRefs;
  }
  clang::DeclRefExpr *getDRE() { return mArray; }
  clang::ValueDecl *getDecl() { return mArray->getDecl(); }
  llvm::SmallVector<DeclRefExpr *, 1> &getRefs() { return mDeclRefs; }
  llvm::SmallVector<Expr *, 1> &getDims() { return mDimensions; }
  llvm::Value *getValue() { return mValue; }
  void setValue(llvm::Value *Value) { mValue = Value; }
  void setDiag(clang::SourceLocation SL, unsigned DiagKind) {
    if (!mHasError) {
      mDiagSL = SL;
      mDiagKind = DiagKind;
      mHasError = true;
    }
  }
  bool hasError() { return mHasError; }
  unsigned getDiagKind() { return mDiagKind; }
  void addDRE(clang::DeclRefExpr *DRE) { mDeclRefs.push_back(DRE); }
  clang::SourceLocation &getDiagSL() { return mDiagSL; }
  void addInduction(clang::ValueDecl *Induction) {
    mInductions.push_back(Induction);
  }
  llvm::SmallVector<clang::ValueDecl *, 1> &getInductions() {
    return mInductions;
  }
  bool isStrict() { return mStrict; }

private:
  bool mStrict;
  bool mHasError;
  unsigned mDiagKind;
  clang::SourceLocation mDiagSL;
  clang::DeclRefExpr *mArray;
  llvm::Value *mValue;
  llvm::SmallVector<DeclRefExpr *, 1> mDeclRefs;
  llvm::SmallVector<Expr *, 1> mDimensions;
  llvm::SmallVector<clang::ValueDecl *, 1> mInductions;
};
// use this for collecting info about clause
class LoopSwapPragmaVisitor : public RecursiveASTVisitor<LoopSwapPragmaVisitor> {
public:
  LoopSwapPragmaVisitor() : mArray(NULL) {}
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
    if (!mArray)
      mArray = DRE;
    else
      mDimensions.push_back(DRE);
    return true;
  }
  bool VisitIntegerLiteral(clang::IntegerLiteral *IL) {
    mDimensions.push_back(IL);
    return true;
  }
  clang::DeclRefExpr *getArray() { return mArray; }
  llvm::SmallVector<Expr *, 1> getDimensions() { return mDimensions; }

private:
  llvm::SmallVector<Expr *, 1> mDimensions;
  clang::DeclRefExpr *mArray;
};

class ClangLoopSwapVisitor : public RecursiveASTVisitor<ClangLoopSwapVisitor> {
public:
  ClangLoopSwapVisitor(TransformationContext *TfmCtx, ClangGlobalInfoPass::RawInfo &RawInfo,
          ClangLoopSwapPass &Pass, llvm::Module &M)
      : mTfmCtx(TfmCtx), mRawInfo(RawInfo), mRewriter(TfmCtx->getRewriter()),
        mContext(TfmCtx->getContext()), mSrcMgr(mRewriter.getSourceMgr()),
        mLangOpts(mRewriter.getLangOpts()), mPass(Pass), mModule(M),
        mStatus(SEARCH_PRAGMA), mGO(NULL), mDIAT(NULL), mDIDepInfo(NULL),
        mLoopDepth(0), mMemMatcher(NULL), mStrictFlag(false),
        mPerfectLoopInfo(NULL), mCurrentLoops(NULL) {}
  const CanonicalLoopInfo *getLoopInfo(ForStmt *FS) {
    if (!FS)
      return NULL;
    for (auto Info : *mCurrentLoops) {
      if (Info->getASTLoop() == FS) {
        return Info;
      }
    }
    return NULL;
  }
  bool TraverseForStmt(ForStmt *FS) {
    if (!FS)
      return false;
    if (mStatus == GET_REFERENCES) {
      mLoopDepth++;
      auto *LoopInfo = getLoopInfo(FS);
      if (!LoopInfo->isCanonical()) {
        toDiag(mSrcMgr.getDiagnostics(), FS->getLocStart(),
               diag::err_expand_not_canonical);
        resetVisitor();
        return RecursiveASTVisitor::TraverseForStmt(FS);
      }

      for (auto A : mExpandClauses) {
        if (mLoopDepth < A.second->getDims().size()) {
          if (!mPerfectLoopInfo->count(LoopInfo->getLoop())) {
            A.second->setDiag(FS->getLocStart(),
                              diag::err_expand_perfect_expected);
            continue;
          }
          A.second->addInduction(
              mMemMatcher->find<IR>(LoopInfo->getInduction())->get<AST>());
        } else if (mLoopDepth == A.second->getDims().size()) {
          A.second->addInduction(
              mMemMatcher->find<IR>(LoopInfo->getInduction())->get<AST>());
        }
        // if strict then check array is private
        if (A.second->isStrict()) {
          bool PrivateFlag = false;
          std::string ArrayName = A.first->getNameAsString();
          auto *Loop = LoopInfo->getLoop()->getLoop();
          auto *LoopID = Loop->getLoopID();
          // what if there no analysis
          auto DepItr = mDIDepInfo->find(LoopID);
          auto &DIDepSet = DepItr->get<DIDependenceSet>();
          DenseSet<const DIAliasNode *> Coverage;
          accessCoverage<bcl::SimpleInserter>(DIDepSet, *mDIAT, Coverage,
                                              mGO->IgnoreRedundantMemory);
          if (!Coverage.empty()) {
            for (auto &DIAT : DIDepSet) {
              if (!Coverage.count(DIAT.getNode()))
                continue;
              if (DIAT.is<trait::Private>()) {
                for (auto DIMTR : DIAT) {
                  for (auto WTVH : *DIMTR->getMemory()) {
                    if (WTVH == A.second->getValue()) {
                      PrivateFlag = true;
                    }
                  }
                }
              }
            }
          } // else what?
          if (!PrivateFlag) {
            // change diag kind
            A.second->setDiag(FS->getLocStart(), diag::err_expand_not_private);
          }
        }
      }
      bool Ret = RecursiveASTVisitor::TraverseForStmt(FS);
      mLoopDepth--;
      return Ret;
    } else {
      return RecursiveASTVisitor::TraverseForStmt(FS);
    }
  }
  bool TraverseDecl(Decl *D) {
    if (!D)
      return RecursiveASTVisitor::TraverseDecl(D);
    if (mStatus == TRAVERSE_STMT) {
      toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
             diag::err_expand_not_forstmt);
      resetVisitor();
    }
    return RecursiveASTVisitor::TraverseDecl(D);
  }
  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    switch (mStatus) {
    case Status::SEARCH_PRAGMA: {
      Pragma P(*S);
      llvm::SmallVector<clang::Stmt *, 1> Clauses;
      // if found expand clause
      bool Found = false;
      if (findClause(P, ClauseId::Expand, Clauses)) {
        if (!handleClauses(P, Clauses, true))
          return true;
        Found = true;
      }
      Clauses.clear();
      if (findClause(P, ClauseId::ExpandNostrict, Clauses)) {
        if (!handleClauses(P, Clauses, false))
          return true;
        Found = true;
      }
      Clauses.clear();
      if (Found) {
        mLoopDepth = 0;
        mStatus = Status::TRAVERSE_STMT;
        return true;
      } else {
        return RecursiveASTVisitor::TraverseStmt(S);
      }
    }

    case Status::TRAVERSE_STMT: {
      if (!isa<ForStmt>(S)) {
        toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(),
               diag::err_expand_not_forstmt);
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // check ArrayType
      for (auto A : mExpandClauses) {
        if (!A.first->getType().getTypePtr()->isArrayType()) {
          A.second->setDiag(A.first->getLocation(), diag::err_expand_not_array);
        }
      }
      // Macro check
      bool HasMacro = false;
      for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawInfo.Macros,
                     [&HasMacro, this](clang::SourceLocation Loc) {
                       if (!HasMacro) {
                         toDiag(mContext.getDiagnostics(), Loc,
                                diag::note_assert_no_macro);
                         HasMacro = true;
                       }
                     });
      if (HasMacro) {
        mStatus = SEARCH_PRAGMA;
        resetVisitor();
        return RecursiveASTVisitor::TraverseStmt(S);
      }
      // get analisis from provider for current fucntion, if it not done already
      if (mNewAnalisysRequired) {
        Function *F = mModule.getFunction(mCurrentFD->getNameAsString());
        mProvider = &mPass.getAnalysis<LoopSwapPassProvider>(*F);
        mCurrentLoops =
            &mProvider->get<CanonicalLoopPass>().getCanonicalLoopInfo();
        mDIAT = &mProvider->get<DIEstimateMemoryPass>().getAliasTree();
        mGO = &mProvider->get<GlobalOptionsImmutableWrapper>().getOptions();
        mDIDepInfo =
            &mProvider->get<DIDependencyAnalysisPass>().getDependencies();
        mMemMatcher =
            &mProvider->get<MemoryMatcherImmutableWrapper>().get().Matcher;
        mPerfectLoopInfo =
            &mProvider->get<ClangPerfectLoopPass>().getPerfectLoopInfo();
        mNewAnalisysRequired = false;
      }
      // get IR Value* according AST VarDecl* through memorymatcher pass
      for (auto A : mExpandClauses) {
        A.second->setValue(
            mMemMatcher->find<AST>((VarDecl *)A.first)->get<IR>());
      }
      mStatus = GET_REFERENCES;
      auto res = TraverseForStmt((ForStmt *)S);
      mStatus = SEARCH_PRAGMA;

      bool Changed = false;
      for (auto A : mExpandClauses) {
        if (A.second->hasError()) {
          toDiag(mSrcMgr.getDiagnostics(), A.second->getDiagSL(),
                 A.second->getDiagKind());
          continue;
        }
        if (!A.second->getDeclRefs().size()) {
          toDiag(mSrcMgr.getDiagnostics(), A.first->getLocation(),
                 diag::err_expand_array_not_used);
          continue;
        }
        Changed = true;
        std::string ExpandName = addSuffix(A.first->getNameAsString());
        std::string Typename, OldSizeDim, ExpandSizeDim, ExpandIterDim,
            Dimension;
        SplitType(A.first->getType().getAsString(), Typename, OldSizeDim);
        bool Positive = true;
        for (auto Dim : A.second->getDims()) {
          if (isa<IntegerLiteral>(Dim)) {
            Dimension =
                to_string(((IntegerLiteral *)Dim)->getValue().getZExtValue());

          } else if (isa<DeclRefExpr>(Dim)) {
            Dimension = ((DeclRefExpr *)Dim)->getDecl()->getNameAsString();
          }
          ExpandSizeDim = (ExpandSizeDim + Twine("[") + Dimension + "]").str();
        }
        // insert definition
        mRewriter.InsertText(
            S->getLocStart(),
            (Twine(Typename) + ExpandName + ExpandSizeDim + OldSizeDim + ";\n")
                .str());
        // prepare new dimensions
        for (auto Ind : A.second->getInductions()) {
          ExpandIterDim =
              (ExpandIterDim + Twine("[") + Ind->getName() + "]").str();
        }
        // replace old array to new array with new dimensions
        std::string ArrayCall = (Twine(ExpandName) + ExpandIterDim).str();
        for (auto DRE : A.second->getDeclRefs())
          mRewriter.ReplaceText(DRE->getSourceRange(), ArrayCall);
      }
      if (Changed) {
        // insert brackets
        mRewriter.InsertTextBefore(S->getLocStart(), "{");
        mRewriter.InsertTextAfterToken(S->getLocEnd(), "}");
      }
      resetVisitor();
      return res;
    }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }
  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (!DRE)
      return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
    if (mStatus == GET_REFERENCES) {
      auto Decl = DRE->getDecl();
      auto It = mExpandClauses.find(Decl);
      if (It != mExpandClauses.end()) {
        if (mLoopDepth <= It->second->getDims().size()) {
          It->second->setDiag(DRE->getLocation(),
                              diag::err_expand_not_deep_enough);
          return true;
        }
        It->second->addDRE(DRE);
      }
    }
    return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
  }
  bool VisitFunctionDecl(FunctionDecl *FD) {
    if (!FD)
      return RecursiveASTVisitor::VisitFunctionDecl(FD);
    mCurrentFD = FD;
    mNewAnalisysRequired = true;
    return RecursiveASTVisitor::VisitFunctionDecl(FD);
  }
  /// splits type from decl to typename and dimensions
  /// for example (int[10][M] => Typename = int, OldSizeDim = [10][M])
  // maybe there is adequate analog for this function in QualType
  void SplitType(string Type, string &Typename, string &Dimensions) {
    SmallVector<StringRef, 8> fragments;
    StringRef separators = "[";
    SplitString(Type, fragments, separators);
    Typename = fragments[0];
    fragments[0] = "";
    string buf = "";
    for (auto f : fragments) {
      if (f != "")
        buf += "[";
      buf += f;
    }
    Dimensions = buf;
  }
  std::string addSuffix(std::string Name) {
    SmallString<32> Buf;
    for (unsigned Count = 0;
         mRawInfo.Identifiers.count((Name + Twine(Count)).toStringRef(Buf));
         ++Count, Buf.clear())
      ;
    StringRef NewName(Buf.data(), Buf.size());
    mRawInfo.Identifiers.insert(NewName);
    return NewName;
  }
  // returns this object parameters to initial state
  void resetVisitor() {
    mStatus = SEARCH_PRAGMA;
    for (auto A : mExpandClauses) {
      delete A.second;
    }
    mExpandClauses.clear();
    mLoopDepth = 0;
    mStrictFlag = false;
  }

private:
  // returns false if there is array duplicate
  bool handleClauses(Pragma &P, llvm::SmallVector<clang::Stmt *, 1> &Clauses,
                     bool Strict) {
    llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
    auto IsPossible =
        pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts, ToRemove);
    if (!IsPossible.first)
      if (IsPossible.second & PragmaFlags::IsInMacro)
        toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
               diag::warn_remove_directive_in_macro);
      else if (IsPossible.second & PragmaFlags::IsInHeader)
        toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
               diag::warn_remove_directive_in_include);
      else
        toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
               diag::warn_remove_directive);
    Rewriter::RewriteOptions RemoveEmptyLine;
    /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
    /// set to true then removing (in RewriterBuffer) works incorrect.
    RemoveEmptyLine.RemoveLineIfEmpty = false;
    for (auto SR : ToRemove)
      mRewriter.RemoveText(SR, RemoveEmptyLine);
    // gathering clause info
    for (auto C : Clauses) {
      LoopSwapPragmaVisitor PV;
      PV.TraverseStmt(C);
      auto Decl = PV.getArray()->getDecl();
      if (mExpandClauses.count(Decl)) {
        toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
               diag::err_expand_twice);
        resetVisitor();
        return false;
      }
      auto Pair =
          mExpandClauses.insert(std::pair<clang::ValueDecl *, SwapClauseInfo *>(
              Decl,
              new SwapClauseInfo(PV.getArray(), PV.getDimensions(), Strict)));
      // check integer literals positive
      for (auto Lit : Pair.first->second->getDims()) {
        if (isa<IntegerLiteral>(Lit)) {
          if (cast<IntegerLiteral>(Lit)->getValue().getZExtValue() <= 0)
            Pair.first->second->setDiag(Lit->getLocStart(),
                                        diag::err_expand_not_positive);
        }
      }
    }
    return true;
  }

  TransformationContext *mTfmCtx;
  ClangGlobalInfoPass::RawInfo &mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  // get analysis from provider only if it is required
  bool mNewAnalisysRequired;
  FunctionDecl *mCurrentFD;
  const CanonicalLoopSet *mCurrentLoops;
  ClangLoopSwapPass &mPass;
  llvm::Module &mModule;
  LoopSwapPassProvider *mProvider;
  tsar::DIDependencInfo *mDIDepInfo;
  tsar::DIAliasTree *mDIAT;
  const tsar::GlobalOptions *mGO;
  tsar::MemoryMatchInfo::MemoryMatcher *mMemMatcher;
  tsar::PerfectLoopInfo *mPerfectLoopInfo;

  bool mStrictFlag;
  // it must be reset manually
  int mLoopDepth;
  // llvm::SmallVector<Value *, 3> mInductions;
  std::map<ValueDecl *, SwapClauseInfo *> mExpandClauses;
  enum Status { SEARCH_PRAGMA, TRAVERSE_STMT, GET_REFERENCES } mStatus;
};

} // namespace

bool ClangLoopSwapPass::runOnModule(llvm::Module &M) {
  errs() << "Start loop swap pass\n";
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  // set provider's wrappers
  LoopSwapPassProvider::initialize<TransformationEnginePass>(
      [&M, &TfmCtx](TransformationEnginePass &TEP) {
        TEP.setContext(M, TfmCtx);
      });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  LoopSwapPassProvider::initialize<MemoryMatcherImmutableWrapper>(
      [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*MMWrapper);
      });
  auto &DIMEW = getAnalysis<DIMemoryEnvironmentWrapper>();
  LoopSwapPassProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEW](DIMemoryEnvironmentWrapper &Wrapper) { Wrapper.set(*DIMEW); });
  auto &DIMTPW = getAnalysis<DIMemoryTraitPoolWrapper>();
  LoopSwapPassProvider::initialize<DIMemoryTraitPoolWrapper>(
      [&DIMTPW](DIMemoryTraitPoolWrapper &Wrapper) { Wrapper.set(*DIMTPW); });
  auto &mGlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  LoopSwapPassProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&mGlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&mGlobalOpts);
      });
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  ClangLoopSwapVisitor vis(TfmCtx, GIP.getRawInfo(), *this, M);
  vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  errs() << "Finish loop swap pass\n";
  return false;
}
