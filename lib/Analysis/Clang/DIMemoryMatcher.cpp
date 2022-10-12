//=== DIMemoryMatcher.cpp  High and Metadata Level Memory Matcher *- C++ -*===//
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
// This file implements a pass to match variable in a source high-level code
// and appropriate metadata-level representations of variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/Matcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "di-memory-matcher"

STATISTIC(NumMatchMemory, "Number of matched variables");
STATISTIC(NumNonMatchASTMemory, "Number of non-matched AST variables");
STATISTIC(NumNonMatchDIMemory, "Number of non-matched DI variables");

char ClangDIMemoryMatcherPass::ID = 0;

INITIALIZE_PASS_BEGIN(ClangDIMemoryMatcherPass, "di-memory-matcher",
  "High and Metadata Memory Matcher (Clang)", true, true)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(ClangDIMemoryMatcherPass, "di-memory-matcher",
  "High and Metadata Memory Matcher (Clang)", true, true)

FunctionPass *llvm::createDIMemoryMatcherPass() {
  return new ClangDIMemoryMatcherPass;
}

void ClangDIMemoryMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

namespace {
struct DILocalScopeMapInfo {
  static std::pair<unsigned, unsigned> getLineColumn(
      const DILocalScope *Scope) {
    if (auto *S = dyn_cast<DISubprogram>(Scope))
      return { S->getScopeLine(), 0u };
    if (auto *S = dyn_cast<DILexicalBlock>(Scope)) {
      return { S->getLine(), S->getColumn() };
    }
    return { 0u, 0u };
  }
  static inline DILocalScope * getEmptyKey() {
    return DenseMapInfo<DILocalScope *>::getEmptyKey();
  }
  static inline DILocalScope * getTombstoneKey() {
    return DenseMapInfo<DILocalScope *>::getTombstoneKey();
  }
  static unsigned getHashValue(const DILocalScope *Scope) {
    auto LineColumn = getLineColumn(Scope);
    assert(LineColumn.first && "Line must be known!");
    return DenseMapInfo<decltype(LineColumn)>::getHashValue(LineColumn);
  }
  static unsigned getHashValue(const clang::PresumedLoc &PLoc) {
    auto Line = PLoc.getLine();
    auto Column = PLoc.getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static bool isEqual(const DILocalScope *LHS, const DILocalScope *RHS) {
    if (LHS == RHS)
      return true;
    auto TK = getTombstoneKey();
    auto EK = getEmptyKey();
    if (RHS == TK || LHS == TK || RHS == EK || LHS == EK)
      return false;
    auto LineColumnLHS = getLineColumn(LHS);
    auto LineColumnRHS = getLineColumn(RHS);
    sys::fs::UniqueID LHSId, RHSId;
    SmallString<128> LHSPath, RHSPath;
    return LineColumnLHS == LineColumnRHS &&
           !sys::fs::getUniqueID(tsar::getAbsolutePath(*LHS, LHSPath), LHSId) &&
           !sys::fs::getUniqueID(tsar::getAbsolutePath(*RHS, RHSPath), RHSId) &&
           LHSId == RHSId;
  }
  static bool isEqual(const clang::PresumedLoc &LHS, const DILocalScope *RHS) {
    if (isEqual(RHS, getTombstoneKey()) || isEqual(RHS, getEmptyKey()))
      return false;
    auto LineColumn = getLineColumn(RHS);
    sys::fs::UniqueID LHSId, RHSId;
    SmallString<128> LHSPath, RHSPath;
    return (!LineColumn.first || LHS.getLine() == LineColumn.first) &&
           (!LineColumn.second || LHS.getColumn() == LineColumn.second) &&
           !sys::fs::getUniqueID(LHS.getFilename(), LHSId) &&
           !sys::fs::getUniqueID(tsar::getAbsolutePath(*RHS, RHSPath), RHSId) &&
           LHSId == RHSId;
  }
};

class MatchDIVisitor
    : public ClangMatchASTBase<MatchDIVisitor, DIVariable *, VarDecl *,
                               DILocalScope *, DILocalScopeMapInfo, unsigned,
                               DenseMapInfo<unsigned>,
                               ClangDIMemoryMatcherPass::DIMemoryMatcher,
                               ClangDIMemoryMatcherPass::MemoryASTSet>,
      public RecursiveASTVisitor<MatchDIVisitor> {
public:
  MatchDIVisitor(SourceManager &SrcMgr, Matcher &MM,
      UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
    ClangMatchASTBase(SrcMgr, MM, Unmatched, LocMap, MacroMap) {}

  bool VisitVarDecl(VarDecl *D) {
    mVisitedVars.push_back(D->getCanonicalDecl());
    return true;
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    if (FD->doesThisDeclarationHaveABody()) {
      mIsEntry = true;
      mFuncDecl = FD;
      mVisitedVars.clear();
    }
    return RecursiveASTVisitor::TraverseFunctionDecl(FD);
  }

  bool TraverseStmt(Stmt *S) {
    if (!S || !isa<CompoundStmt>(S) && !isa<ForStmt>(S) && !isa<WhileStmt>(S) &&
                  !isa<IfStmt>(S))
      return RecursiveASTVisitor::TraverseStmt(S);
    // Parameters were visited before the body.
    auto StashSize = mIsEntry ? 0 : mVisitedVars.size();
    // Location of function declaration is used instead of location of body
    // to map top-level local variables.
    auto ScopeLoc = mIsEntry ? mFuncDecl->getLocation() : S->getBeginLoc();
    mIsEntry = false;
    auto Res = RecursiveASTVisitor::TraverseStmt(S);
    if (Res && mVisitedVars.size() > StashSize) {
      llvm::sort(mVisitedVars.begin() + StashSize, mVisitedVars.end(),
                 [](const VarDecl *LHS, const VarDecl *RHS) {
                   return LHS->getName() < RHS->getName();
                 });
      LLVM_DEBUG(dbgs() << "[DI MEMORY MATCHER]: visited "
                        << mVisitedVars.size() - StashSize
                        << " variables in scope at ";
                 ScopeLoc.print(dbgs(), *mSrcMgr); dbgs() << "\n");
      if (ScopeLoc.isInvalid()) {
        mUnmatchedAST->insert(mVisitedVars.begin() + StashSize,
                              mVisitedVars.end());
        NumNonMatchASTMemory += mVisitedVars.size() - StashSize;
        mVisitedVars.resize(StashSize);
        return true;
      }
      if (ScopeLoc.isMacroID()) {
        ScopeLoc = mSrcMgr->getExpansionLoc(ScopeLoc);
        auto Pair = mLocToMacro->try_emplace(ScopeLoc.getRawEncoding());
        assert(Pair.second && "Unable to insert list of variables!");
        Pair.first->second.insert(Pair.first->second.end(),
          mVisitedVars.begin() + StashSize, mVisitedVars.end());
      } else {
        auto I = findItrForLocation(ScopeLoc);
        if (I != mLocToIR->end()) {
          auto SearchFromItr = mVisitedVars.begin() + StashSize;
          for (unsigned Idx = 0, IdxE = I->second.size(); Idx < IdxE; ++Idx) {
            auto DIV = I->second[Idx];
            // We search variables for promoted locations only. Other variables
            // are processing further.
            auto Itr = std::find_if(SearchFromItr, mVisitedVars.end(),
                                    [DIV](const VarDecl *D) {
                                      return DIV->getName() == D->getName();
                                    });
            if (Itr == mVisitedVars.end())
              continue;
            mMatcher->emplace(*Itr, DIV);
            ++NumMatchMemory;
            --NumNonMatchDIMemory;
            NumNonMatchASTMemory += std::distance(SearchFromItr, Itr);
            SearchFromItr = Itr + 1;
          }
          I->second.clear();
        } else {
          mUnmatchedAST->insert(mVisitedVars.begin() + StashSize,
                                mVisitedVars.end());
          NumNonMatchASTMemory += mVisitedVars.size() - StashSize;
        }
      }
      mVisitedVars.resize(StashSize);
    }
    return Res;
  }
private:
  std::vector<VarDecl *> mVisitedVars;
  SmallVector<SourceLocation, 8> mScopes;
  FunctionDecl *mFuncDecl = nullptr;
  bool mIsEntry = false;
};
}

bool ClangDIMemoryMatcherPass::runOnFunction(Function &F) {
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
  auto *FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  MatchDIVisitor::LocToIRMap LocToDIVar;
  MatchDIVisitor::LocToASTMap LocToMacro;
  MatchDIVisitor MatchDIVar(SrcMgr,
    mMatcher, mUnmatchedAST, LocToDIVar, LocToMacro);
  SmallPtrSet<DIVariable *, 32> VisitedDIVars;
  for (auto &I : instructions(F)) {
    auto *DbgValue = dyn_cast<DbgValueInst>(&I);
    if (!DbgValue)
      continue;
    auto *DIVar = DbgValue->getVariable();
    if (DIVar && VisitedDIVars.insert(DIVar).second) {
      ++NumNonMatchDIMemory;
      if (auto *S = dyn_cast_or_null<DILocalScope>(DIVar->getScope())) {
        auto Pair =
            isa<DISubprogram>(S)
                ? LocToDIVar.insert_as(
                      std::make_pair(S, TinyPtrVector<DIVariable *>(DIVar)),
                      SrcMgr.getPresumedLoc(
                          SrcMgr.getExpansionLoc(FuncDecl->getLocation())))
                : LocToDIVar.insert(
                      std::make_pair(S, TinyPtrVector<DIVariable *>(DIVar)));
        if (!Pair.second)
          Pair.first->second.push_back(DIVar);
        LLVM_DEBUG(dbgs() << "[DI MEMORY MATCHER]: remember metadata for '"
                          << DIVar->getName() << "' at ";
                   auto LineColumn = DILocalScopeMapInfo::getLineColumn(S);
                   dbgs() << LineColumn.first << ":" << LineColumn.second
                          << "\n");
      }
    }
  }
  for (auto &Pair : LocToDIVar)
    llvm::sort(Pair.second.begin(), Pair.second.end(),
               [](const DIVariable *LHS, const DIVariable *RHS) {
                 return LHS->getName() < RHS->getName();
               });
  MatchDIVar.TraverseDecl(FuncDecl);
  for (auto &Pair : LocToMacro) {
    llvm::sort(Pair.second.begin(), Pair.second.end(),
               [](const VarDecl *LHS, const VarDecl *RHS) {
                 return LHS->getName() < RHS->getName();
               });
    for (auto I = Pair.second.begin() + 1, EI = Pair.second.end(); I < EI;
         ++I) {
      if ((*(I - 1))->getName() == (*I)->getName()) {
        // Unable to distinguish locations with the same name inside a macro.
        Pair.second.clear();
        break;
      }
    }
  }
  MatchDIVar.matchInMacro(
    NumMatchMemory, NumNonMatchASTMemory, NumNonMatchDIMemory);
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &MemInfo = getAnalysis<MemoryMatcherImmutableWrapper>().get();
  for (auto &Match : MemInfo.Matcher) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    if (auto Inst{dyn_cast<Instruction>(Match.get<IR>().front())};
        Inst && Inst->getFunction() != &F)
      continue;
    auto MD = findMetadata(Match.get<IR>().front(), DILocs, &DT,
      MDSearch::AddressOfVariable);
    if (MD && MD->isValid() && MD->Expr->getNumElements() == 0) {
      auto Itr{find_if(Match.get<AST>(), [TfmCtx](const VarDecl *VD) {
        return &VD->getASTContext() == &TfmCtx->getContext();
      })};
      assert(Itr != Match.get<AST>().end() &&
             "Matched variable must be presented in a current AST context.");
      mMatcher.emplace(*Itr, MD->Var);
      for (auto *D : Match.get<AST>())
        if (mUnmatchedAST.erase(D))
          --NumNonMatchASTMemory;
      ++NumMatchMemory;
    } else {
      auto Itr{find_if(Match.get<AST>(), [TfmCtx](const VarDecl *VD) {
        return &VD->getASTContext() == &TfmCtx->getContext();
      })};
      assert(Itr != Match.get<AST>().end() &&
             "Matched variable must be presented in a current AST context.");
      mUnmatchedAST.insert(*Itr);
      ++NumNonMatchASTMemory;
    }
  }
  for (auto *D : MemInfo.UnmatchedAST)
    if (mMatcher.find<AST>(D) == mMatcher.end()) {
      mUnmatchedAST.insert(D->getCanonicalDecl());
      ++NumNonMatchASTMemory;
    }
  return false;
}

char ClangDIGlobalMemoryMatcherPass::ID = 0;

INITIALIZE_PASS_BEGIN(ClangDIGlobalMemoryMatcherPass, "di-global-memory-matcher",
  "High and Metadata Global Memory Matcher (Clang)", true, true)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_END(ClangDIGlobalMemoryMatcherPass, "di-global-memory-matcher",
  "High and Metadata Global Memory Matcher (Clang)", true, true)

ModulePass *llvm::createDIGlobalMemoryMatcherPass() {
  return new ClangDIGlobalMemoryMatcherPass;
}

void ClangDIGlobalMemoryMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

bool ClangDIGlobalMemoryMatcherPass::runOnModule(llvm::Module &M) {
  auto &MemInfo = getAnalysis<MemoryMatcherImmutableWrapper>().get();
  for (auto &Match : MemInfo.Matcher) {
    if (!isa<GlobalVariable>(Match.get<IR>().front()))
      continue;
    SmallVector<DIMemoryLocation, 1> DILocs;
    findGlobalMetadata(cast<GlobalVariable>(Match.get<IR>().front()), DILocs);
    Optional<DIMemoryLocation> MD;
    if (DILocs.size() == 1 && (MD = DILocs.back()) &&
        MD->isValid() && MD->Expr->getNumElements() == 0) {
      for (auto *D : Match.get<AST>())
        mMatcher.emplace(D, MD->Var);
      for (auto *D : Match.get<AST>())
        if (mUnmatchedAST.erase(D))
          --NumNonMatchASTMemory;
      ++NumMatchMemory;
    } else {
      for (auto *D: Match.get<AST>())
        mUnmatchedAST.insert(D);
      ++NumNonMatchASTMemory;
    }
  }
  for (auto *D : MemInfo.UnmatchedAST) {
    if (D->isLocalVarDeclOrParm())
      continue;
    if (mMatcher.find<AST>(D) == mMatcher.end()) {
      mUnmatchedAST.insert(D->getCanonicalDecl());
      ++NumNonMatchASTMemory;
    }
  }
  return false;
}
