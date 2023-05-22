//===--- Inductive.cpp - Inductive variables elimination (Clang) ---------*- C++
//-*-===//
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
// This file implements a pass to perform inductive variables elimination in
// loops of C programs.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
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
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-ind"

static int SrcLocLine(const clang::SourceLocation &SL,
                      const clang::SourceManager &SM) {
  std::size_t const N1 = SL.printToString(SM).find_first_of(":");
  std::string tail = SL.printToString(SM).substr(N1 + 1);
  std::size_t const N2 = tail.find_first_of(":");
  std::string Line = tail.substr(0, N2);
  return std::stoi(Line);
}

namespace {
class ClangInductive : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangInductive() : FunctionPass(ID) {
    initializeClangInductivePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class ClangInductiveInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override;
  void addAfterPass(legacy::PassManager &Passes) const override;
};

struct LoopCounterVar {
  std::string Name;
  int BegVal;
  int EndVal;
  int Step;
};

struct IndVars {
  bool FoundInCurrentLoop = false;
  bool StartVarFlag = false;
  bool StepVarFlag = false;
  bool LocalVarCreated = false;
  bool StopExprPlaced = false;
  llvm::SmallString<64> Name;
  std::string TypeName;
  int Start;
  llvm::SmallString<64> StartVar;
  int Step;
  llvm::SmallString<64> StepVar;
  int Stop;
  int IndVarForLocation = -1;
  clang::SourceLocation OutermostForEndLoc;
  VarDecl *VD = nullptr;
};
} // namespace

void ClangInductiveInfo::addBeforePass(legacy::PassManager &Passes) const {
  addImmutableAliasAnalysis(Passes);
  addInitialTransformations(Passes);
  Passes.add(createAnalysisSocketImmutableStorage());
  Passes.add(createDIMemoryTraitPoolStorage());
  Passes.add(createGlobalsAccessStorage());
  Passes.add(createGlobalsAccessCollector());
  Passes.add(createDIEstimateMemoryPass());
  Passes.add(createDIMemoryAnalysisServer());
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createAnalysisWaitServerPass());
  Passes.add(createDIMemoryEnvironmentStorage());
}

void ClangInductiveInfo::addAfterPass(legacy::PassManager &Passes) const {
  Passes.add(createAnalysisReleaseServerPass());
  Passes.add(createAnalysisCloseConnectionPass());
}

void ClangInductive::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<CanonicalLoopPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.addRequired<ClangPerfectLoopPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<DIArrayAccessWrapper>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequiredTransitive<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.setPreservesAll();
}

char ClangInductive::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangInductive, "clang-ind",
                               "eliminate inductive variables in for loops",
                               false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangInductiveInfo)
INITIALIZE_PASS_DEPENDENCY(AnalysisClientServerMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(DIArrayAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(ClangInductive, "clang-ind",
                             "eliminate inductive variables in for loops",
                             false, false,
                             TransformationQueryManager::getPassRegistry())

namespace {

class ClangInductiveVisitor
    : public clang::RecursiveASTVisitor<ClangInductiveVisitor> {
  enum LoopKind : uint8_t {
    Ok = 0,
    NotCanonical = 1u << 0,
  };

public:
  ClangInductiveVisitor(ClangInductive &P, llvm::Function &F,
                        ClangTransformationContext *TfmCtx,
                        const ASTImportInfo &ImportInfo)
      : mImportInfo(ImportInfo), mRewriter(TfmCtx->getRewriter()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()),
        mRawInfo(
            P.getAnalysis<ClangGlobalInfoPass>().getGlobalInfo(TfmCtx)->RI),
        mDIMemMatcher(P.getAnalysis<ClangDIMemoryMatcherPass>().getMatcher()),
        mCanonicalLoopInfo(
            P.getAnalysis<CanonicalLoopPass>().getCanonicalLoopInfo()),
        mAT(P.getAnalysis<EstimateMemoryPass>().getAliasTree()),
        mDIMInfo(P.getAnalysis<DIEstimateMemoryPass>().getAliasTree(), P, F),
        mGlobalOpts(
            &P.getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()),
        mTfmCtx(TfmCtx),
        mSocket(
            P.getAnalysis<AnalysisSocketImmutableWrapper>()->getActiveSocket()),
        mDIMemoryMatcher(&P.getAnalysis<ClangDIMemoryMatcherPass>()),
        mCanonical(&P.getAnalysis<CanonicalLoopPass>()),
        mRgnInfo(&P.getAnalysis<DFRegionInfoPass>().getRegionInfo()) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    Pragma P(*S);

    if (findClause(P, ClauseId::Indvars, mClauses)) {
      mAutoFlag = true;
      auto locationForInits = S->getEndLoc();
      mIsInPragma = true;
      bool Ast = RecursiveASTVisitor::TraverseStmt(S);
      mIsInPragma = false;
      if (mErrorFlag)
        return false;
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
        mRewriter.RemoveText(SR, RemoveEmptyLine); // delete each range
      return Ast;
    }

    if (mForDepth) {
      // find inductive variable
      if (auto *VS{dyn_cast<ValueStmt>(S)})
        if (auto *Ex = VS->getExprStmt())
          if (auto *BinOper{dyn_cast<clang::BinaryOperator>(Ex)})
            if (BinOper->isAssignmentOp() || BinOper->isCompoundAssignmentOp())
              if (auto *DRE{dyn_cast<DeclRefExpr>(BinOper->getLHS())})
                if (auto *VD{dyn_cast<VarDecl>(DRE->getDecl())}) {
                  auto VarName = VD->getName();
                  std::string StepStr;
                  std::string StartStr;

                  for (auto &It : mVarVector)
                    if (It.VD && It.VD == VD) {
                      int ForLine = It.IndVarForLocation;
                      int Multiplier = 1;

                      // places <var> = <expr>
                      if (ForLine != -1 &&
                          ForLine > SrcLocLine(S->getBeginLoc(), mSrcMgr)) {
                        auto SourceLoc = S->getBeginLoc();
                        std::string ToInsert;
                        std::string Expr;
                        std::string Devide;
                        bool OneTimeFlag = true;

                        if (It.StepVarFlag)
                          StepStr = std::string(It.StepVar);
                        else
                          StepStr = std::to_string(It.Step);
                        if (It.StartVarFlag)
                          StartStr = std::string(It.StartVar);
                        else
                          StartStr = std::to_string(It.Start);
                        for (auto C = mCurrentLoopCounters.rbegin();
                             C != mCurrentLoopCounters.rend(); ++C) {
                          if (C->Step != 1)
                            Devide = " / " + std::to_string(C->Step);
                          if (OneTimeFlag) {
                            OneTimeFlag = false;
                            if (!C->BegVal)
                              Expr = "(" + C->Name + " + " +
                                     std::to_string(C->Step) + ")" + Devide;
                            else
                              Expr = "(" + C->Name + " - " +
                                     std::to_string(C->BegVal - C->Step) + ")" +
                                     Devide;
                            Devide = "";
                          } else {
                            Expr = "(" + C->Name + " - " +
                                   std::to_string(C->BegVal) + ") * " +
                                   std::to_string(Multiplier) + " * " +
                                   StepStr + Devide + " + " + Expr;
                          }
                          Multiplier *= (C->EndVal - C->BegVal) / C->Step;
                        }

                        ToInsert = ("_" + It.Name + " = " + StartStr + " + " +
                                    Expr + " * " + StepStr + ";\n// ")
                                       .str();
                        mRewriter.InsertTextAfterToken(SourceLoc, ToInsert);
                      }
                      // places "<var> = <stop>" after loop.
                      std::string TmpString;
                      if (!It.StopExprPlaced) {
                        if (!It.StartVarFlag && !It.StepVarFlag) {
                          It.Stop = It.Start + It.Step * Multiplier;
                          TmpString = std::to_string(It.Stop);
                        } else
                          TmpString = (StartStr + " + " + StepStr + " * " +
                                       std::to_string(Multiplier));
                        mRewriter.InsertTextAfterToken(
                            It.OutermostForEndLoc,
                            ("\n" + It.Name + " = " + TmpString + ";\n").str());
                        It.StopExprPlaced = true;
                      }
                    }
                }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseDeclRefExpr(clang::DeclRefExpr *Ex) {
    if (mErrorFlag)
      return false;
    llvm::StringRef VarName;
    if (mIsInPragma) {
      auto type = Ex->getDecl()->getType();
      if (type->isFunctionType() || type->isFunctionPointerType() ||
          type->isStructureOrClassType()) {
        toDiag(mSrcMgr.getDiagnostics(), Ex->getBeginLoc(),
               tsar::diag::error_not_var);
        mErrorFlag = true;
      }
      // get name of inductive variable in pragma's parameters
      if (mWaitingForName) {
        IndVars Tmp;
        if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())}) {
          mAutoFlag = false;
          VarName = Var->getName();
          auto TypeStr = Var->getType().getAsString();
          Tmp.TypeName = TypeStr;
        }
        Tmp.Name = VarName;
        mVarVector.push_back(std::move(Tmp));
        mWaitingForName = false;
        mWaitingForStart = true;
        // get start value of inductive variable in pragma's parameters if start
        // value is a variable
      } else if (mWaitingForStart) {
        // duplicate code
        if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())}) {
          mAutoFlag = false;
          VarName = Var->getName();
          mVarVector.back().StartVar = VarName;
          mWaitingForStart = false;
          mWaitingForStep = true;
          mVarVector.back().StartVarFlag = true;
        }
      } else if (mWaitingForStep) {
        if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())}) {
          VarName = Var->getName();
          mVarVector.back().StepVar = VarName;
          mWaitingForStep = false;
          mWaitingForName = true;
          mVarVector.back().StepVarFlag = true;
        }
      }
    }

    if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())})
      if (mForDepth && !mVarVector.empty()) {
        VarName = Var->getName();
        for (auto It : mVarVector)
          if (It.VD && It.VD == Var) {
            int ForLine = It.IndVarForLocation;
            // replace all instances of variable with _variable
            if (ForLine != -1 &&
                ForLine > SrcLocLine(Ex->getBeginLoc(), mSrcMgr)) {
              std::string Underscore = "_";
              SourceLocation Beg;
              SourceRange Sr;
              Sr = Ex->getSourceRange();
              Beg = Ex->getBeginLoc();
              Rewriter::RewriteOptions RemoveEmptyLine;
              RemoveEmptyLine.RemoveLineIfEmpty = false;
              /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
              /// set to true then removing (in RewriterBuffer) works incorrect.
              mRewriter.InsertTextAfterToken(
                  Beg, (std::string("_") + VarName).str());
              mRewriter.RemoveText(Sr, RemoveEmptyLine);
            }
          }
      }
    return RecursiveASTVisitor::TraverseDeclRefExpr(Ex);
  }

  bool TraverseIntegerLiteral(IntegerLiteral *IL) {
    if (mIsInPragma) {
      auto TmpIntegerVal = IL->getValue().getLimitedValue();
      if (mWaitingForStart) {
        mVarVector.back().Start = TmpIntegerVal;
        mWaitingForStart = false;
        mWaitingForStep = true;
      } else if (mWaitingForStep) {
        mVarVector.back().Step = TmpIntegerVal;
        mWaitingForStep = false;
        mWaitingForName = true;
      }
    }
    return RecursiveASTVisitor::TraverseIntegerLiteral(IL);
  }

  bool TraverseForStmt(ForStmt *F) {
    bool Ast;
    auto ForEndLoc = F->getEndLoc();
    auto ForBegLoc = F->getBeginLoc();
    LoopCounterVar LoopCounter;
    std::string ToInsert;
    clang::VarDecl *Induction = nullptr;
    std::string CounterName;
    clang::VarDecl *CounterVariable;
    int64_t BegInt, StepInt, EndInt;

    // check if loop is in canonical form
    auto CanonicalItr{find_if(mCanonicalLoopInfo, [F](auto *Info) {
      return Info->getASTLoop() == F;
    })};
    if (CanonicalItr != mCanonicalLoopInfo.end() &&
        (*CanonicalItr)->isCanonical()) {
      // get counter variable from loop
      // <type> i = <init>;
      if (auto *DS{dyn_cast_or_null<DeclStmt>(F->getInit())}) {
        Induction = cast<VarDecl>(DS->getSingleDecl());
        CounterName = Induction->getNameAsString();
        // i = <init>;
      } else if (auto *BO{
                     dyn_cast_or_null<clang::BinaryOperator>(F->getInit())}) {
        auto LHS = BO->getLHS();
        if (auto *DRE{dyn_cast<DeclRefExpr>(LHS)})
          if (auto *VD{dyn_cast<VarDecl>(DRE->getDecl())}) {
            Induction = VD;
            CounterName = Induction->getNameAsString();
          }
      }

      // Check if variable is inductive

      assert(ForStmt &&
             "Source-level representation of a loop must be available!");
      auto &SrcMgr{mTfmCtx->getRewriter().getSourceMgr()};
      auto &Diags{SrcMgr.getDiagnostics()};
      auto *L{(*CanonicalItr)->getLoop()->getLoop()};
      auto DFL = cast<DFLoop>(mRgnInfo->getRegionFor(L));
      auto CanonItr{mCanonical->getCanonicalLoopInfo().find_as(DFL)};
      auto *ForStmt = (**CanonItr).getASTLoop();
      auto *Fun = L->getHeader()->getParent();
      auto RF{
          mSocket->getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(
              *Fun)};
      assert(RF &&
             "Dependence analysis must be available for a parallel loop!");
      auto &DIAT{RF->value<DIEstimateMemoryPass *>()->getAliasTree()};
      auto &DIDepInfo{
          RF->value<DIDependencyAnalysisPass *>()->getDependencies()};
      auto RM{mSocket->getAnalysis<AnalysisClientServerMatcherWrapper,
                                   ClonedDIMemoryMatcherWrapper>()};
      assert(RM && "Client to server IR-matcher must be available!");
      auto &ClientToServer =
          **RM->value<AnalysisClientServerMatcherWrapper *>();
      auto ServerLoopID{
          cast<MDNode>(*ClientToServer.getMappedMD(L->getLoopID()))};
      auto DIDepSet1{DIDepInfo[ServerLoopID]};
      auto *ServerF = cast<Function>(ClientToServer[Fun]);
      auto *DIMemoryMatcher =
          (**RM->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF];
      assert(DIMemoryMatcher && "Cloned memory matcher must not be null!");
      auto &ASTToClient = mDIMemoryMatcher->getMatcher();
      tsar::ClangDependenceAnalyzer ASTRegionAnalysis(
          F, *mGlobalOpts, Diags, DIAT, DIDepSet1, *DIMemoryMatcher,
          ASTToClient);
      if (ASTRegionAnalysis.evaluateDependency())
        toDiag(mSrcMgr.getDiagnostics(), F->getBeginLoc(),
               tsar::diag::error_no_inductive_found);
      auto Map = ASTRegionAnalysis.getDependenceInfo().get<trait::Induction>();
      tsar::ClangDependenceAnalyzer::InductionVarListT::iterator It =
          Map.begin();
      while (It != Map.end()) {
        auto VD = It->first.get<AST>();
        auto START = It->second.get<Begin>();

        if (mAutoFlag) {
          // No arguments specified
          if (VD->getNameAsString() != CounterName) {
            IndVars tmp;
            tmp.Start = It->second.get<Begin>().getValue().getExtValue();
            tmp.Step = It->second.get<Step>().getValue().getExtValue();
            tmp.VD = VD;
            if (It->second.get<End>().hasValue())
              tmp.Stop = It->second.get<End>().getValue().getExtValue();
            else {
              toDiag(mSrcMgr.getDiagnostics(), F->getBeginLoc(),
                     tsar::diag::warn_induction_parameters);
              It++;
              continue;
            }
            tmp.TypeName = VD->getType().getAsString();
            tmp.Name = VD->getNameAsString();
            tmp.IndVarForLocation = SrcLocLine(F->getEndLoc(), mSrcMgr);
            tmp.OutermostForEndLoc = F->getEndLoc();
            mVarVector.push_back(std::move(tmp));
          }
        }
        if (VD->getNameAsString() == CounterName) {
          CounterVariable = VD;
          BegInt = It->second.get<Begin>().getValue().getExtValue();
          StepInt = It->second.get<Step>().getValue().getExtValue();
          EndInt = It->second.get<End>().getValue().getExtValue();
        }
        for (auto &VarIt : mVarVector)
          if ((VarIt.Name).str() == VD->getName()) {
            VarIt.VD = VD;
            VarIt.FoundInCurrentLoop = true;
          }
        It++;
      }
      LoopCounter.Name = CounterName;
      LoopCounter.BegVal = BegInt;
      LoopCounter.EndVal = EndInt;
      LoopCounter.Step = StepInt;
      for (auto &It : mVarVector) {
        if (It.IndVarForLocation == -1) {
          It.IndVarForLocation = SrcLocLine(ForEndLoc, mSrcMgr);
          It.OutermostForEndLoc = F->getEndLoc();
        }
        // Insert <TYPE> _VAR = <INIT> before for
        int ForLine = It.IndVarForLocation;
        if (ForLine != -1 && ForLine >= SrcLocLine(F->getBeginLoc(), mSrcMgr))
          if (!It.LocalVarCreated) {
            if (!It.StartVarFlag) {
              ToInsert = (It.TypeName + " " + "_" + It.Name + " = " +
                          std::to_string(It.Start) + ";\n")
                             .str();
            } else {
              ToInsert = (It.TypeName + " " + "_" + It.Name + " = " +
                          It.StartVar + ";\n")
                             .str();
            }
            It.LocalVarCreated = true;
            mRewriter.InsertTextBefore(ForBegLoc, ToInsert);
          }
      }
    } else {
      toDiag(mSrcMgr.getDiagnostics(), F->getBeginLoc(),
             tsar::diag::error_induction_not_canonical);
      mErrorFlag = true;
      return false;
    }
    mForDepth++;
    mCurrentLoopCounters.push_back(std::move(LoopCounter));
    Ast = RecursiveASTVisitor::TraverseForStmt(F);
    mCurrentLoopCounters.pop_back();
    if (mVarVector.size())
      mAutoFlag = false;
    else if (mAutoFlag)
      toDiag(mSrcMgr.getDiagnostics(), F->getBeginLoc(),
             tsar::diag::error_no_inductive_found);
    mForDepth--;
    return Ast;
  }

private:
  bool mAutoFlag = false;
  bool mErrorFlag = false;
  bool mIsInPragma = false;
  bool mWaitingForName = true;
  bool mWaitingForStart = false;
  bool mWaitingForStep = false;
  int mForDepth = 0;

  const ASTImportInfo &mImportInfo;
  Rewriter &mRewriter;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  ClangGlobalInfo::RawInfo &mRawInfo;
  const ClangDIMemoryMatcherPass::DIMemoryMatcher &mDIMemMatcher;
  const CanonicalLoopSet &mCanonicalLoopInfo;
  AliasTree &mAT;
  DIMemoryClientServerInfo mDIMInfo;
  SmallVector<Stmt *, 1> mClauses;
  std::vector<IndVars> mVarVector;
  std::vector<LoopCounterVar> mCurrentLoopCounters;
  std::vector<llvm::SmallString<64>> mVarNames;
  const GlobalOptions *mGlobalOpts = nullptr;
  ClangTransformationContext *mTfmCtx = nullptr;
  AnalysisSocket *mSocket = nullptr;
  ClangDIMemoryMatcherPass *mDIMemoryMatcher = nullptr;
  CanonicalLoopPass *mCanonical = nullptr;
  DFRegionInfo *mRgnInfo = nullptr;
};
} // namespace

bool ClangInductive::runOnFunction(Function &F) {
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
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
  ClangInductiveVisitor(*this, F, TfmCtx, *ImportInfo).TraverseDecl(FD);
  return false;
}