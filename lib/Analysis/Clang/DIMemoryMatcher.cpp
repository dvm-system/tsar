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
#include "tsar_memory_matcher.h"
#include "tsar_matcher.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Dominators.h>

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
using MatchDIVisitorBase = MatchASTBase<DIVariable, VarDecl,
  DILocation *, DILocationMapInfo,
  unsigned, DenseMapInfo<unsigned>,
  ClangDIMemoryMatcherPass::DIMemoryMatcher,
  ClangDIMemoryMatcherPass::MemoryASTSet>;

class MatchDIVisitor :
    public MatchDIVisitorBase,
    public RecursiveASTVisitor<MatchDIVisitor> {
public:
  MatchDIVisitor(SourceManager &SrcMgr, Matcher &MM,
      UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
    MatchASTBase(SrcMgr, MM, Unmatched, LocMap, MacroMap) {}

  /// Evaluates declarations expanded from a macro and stores such
  /// declaration into location to macro map.
  void VisitFromMacro(VarDecl *D) {
    assert(D->getLocStart().isMacroID() &&
      "Declaration must be expanded from macro!");
    auto Loc = D->getLocStart();
    if (Loc.isInvalid())
      return;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return;
    auto Pair = mLocToMacro->insert(
      std::make_pair(Loc.getRawEncoding(), bcl::TransparentQueue<VarDecl>(D)));
    if (!Pair.second)
      Pair.first->second.push(D);
  }

  bool VisitVarDecl(VarDecl *D) {
    if (D->getLocStart().isMacroID()) {
      VisitFromMacro(D);
      return true;
    }
    auto VarLoc = D->getLocation();
    if (auto *AI = findIRForLocation(VarLoc)) {
      mMatcher->emplace(D->getCanonicalDecl(), AI);
      ++NumMatchMemory;
      --NumNonMatchDIMemory;
    } else {
      mUnmatchedAST->insert(D->getCanonicalDecl());
      ++NumNonMatchASTMemory;
    }
    return true;
  }
};
}

bool ClangDIMemoryMatcherPass::runOnFunction(Function &F) {
  releaseMemory();
  auto M = F.getParent();
  auto TfmCtx  = getAnalysis<TransformationEnginePass>().getContext(*M);
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
    auto DbgLoc = I.getDebugLoc();
    auto *DIVar = DbgValue->getVariable();
    if (DIVar && VisitedDIVars.insert(DIVar).second) {
      ++NumNonMatchDIMemory;
      if (DbgLoc) {
        auto Pair = LocToDIVar.insert(
          std::make_pair(DbgLoc, bcl::TransparentQueue<DIVariable>(DIVar)));
        if (!Pair.second)
          Pair.first->second.push(DIVar);
      }
    }
  }
  MatchDIVar.TraverseDecl(FuncDecl);
  MatchDIVar.matchInMacro(
    NumMatchMemory, NumNonMatchASTMemory, NumNonMatchDIMemory);
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &MemInfo = getAnalysis<MemoryMatcherImmutableWrapper>().get();
  for (auto &Match : MemInfo.Matcher) {
    auto SearchType = isa<GlobalVariable>(Match.get<IR>()) ?
      MDSearch::ValueOfVariable : MDSearch::AddressOfVariable;
    SmallVector<DIMemoryLocation, 1> DILocs;
    auto MD = findMetadata(Match.get<IR>(), DILocs, &DT, SearchType);
    if (MD && MD->isValid() && MD->Expr->getNumElements() == 0) {
      mMatcher.emplace(Match.get<AST>(), MD->Var);
      if (mUnmatchedAST.erase(Match.get<AST>()))
        --NumNonMatchASTMemory;
      ++NumMatchMemory;
    } else {
      mUnmatchedAST.insert(Match.get<AST>());
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
