
//=== DistributionLimits.cpp - Limitation of Distribution Checker *- C++ -*===//
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
// This file implements a pass to collect user directives which influence
// parallelization process.
//
//===----------------------------------------------------------------------===//

#include "APCContextImpl.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Frontend/Clang/ASTImportInfo.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/PassProvider.h"
#include <apc/Distribution/Array.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-directives-collector"

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
using VariableList = DenseSet<DIVariable *>;
using ClauseInfo =
    SmallVector<std::pair<ClauseId, std::variant<VariableList>>, 1>;

using APCClangDirectivesCollectorProvider =
    FunctionPassProvider<ClangDIMemoryMatcherPass>;

class APCClangDirectivesVisitor
  : public RecursiveASTVisitor<APCClangDirectivesVisitor> {
public:
  explicit APCClangDirectivesVisitor(const ASTImportInfo &ImportInfo)
      : mImportInfo(&ImportInfo) {}

  void initialize(ClangTransformationContext &TfmCtx,
                  const ClangDIMemoryMatcherPass::DIMemoryMatcher &Matcher) {
    mRewriter = &TfmCtx.getRewriter();
    mSrcMgr = &mRewriter->getSourceMgr();
    mLangOpts = &mRewriter->getLangOpts();
    mMatcher = &Matcher;
  }

  const ClauseInfo &getClauseInfo() const noexcept { return mClauseInfo; }

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    Pragma P{*S};
    llvm::SmallVector<clang::Stmt *, 2> Clauses;
    if (!findClause(P, ClauseId::ProcessPrivate, Clauses))
      return RecursiveASTVisitor::TraverseStmt(S);
    llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
    auto IsPossible{pragmaRangeToRemove(P, Clauses, *mSrcMgr, *mLangOpts,
                                        *mImportInfo, ToRemove)};
    if (!IsPossible.first)
      if (IsPossible.second & PragmaFlags::IsInMacro)
        toDiag(mSrcMgr->getDiagnostics(), Clauses.front()->getBeginLoc(),
               tsar::diag::warn_remove_directive_in_macro);
      else if (IsPossible.second & PragmaFlags::IsInHeader)
        toDiag(mSrcMgr->getDiagnostics(), Clauses.front()->getBeginLoc(),
               tsar::diag::warn_remove_directive_in_include);
      else
        toDiag(mSrcMgr->getDiagnostics(), Clauses.front()->getBeginLoc(),
               tsar::diag::warn_remove_directive);
    clang::Rewriter::RewriteOptions RemoveEmptyLine;
    /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
    /// set to true then removing (in RewriterBuffer) works incorrect.
    RemoveEmptyLine.RemoveLineIfEmpty = false;
    for (auto SR : ToRemove)
      mRewriter->RemoveText(SR, RemoveEmptyLine);
    bool IsOk{true};
    for (auto *C : Clauses) {
      mClause = {C, ClauseId::ProcessPrivate};
      IsOk &= RecursiveASTVisitor::TraverseStmt(C);
    }
    mClause = {nullptr, ClauseId::NotClause};
    return IsOk;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (!std::get<Stmt *>(mClause))
      return true;
    auto *VD{dyn_cast<VarDecl>(DRE->getDecl())};
    if (!VD) {
      //TODO: emit error
      return true;
    }
    auto Itr{mMatcher->find<AST>(VD->getCanonicalDecl())};
    if (Itr == mMatcher->end()) {
      //TODO: emit error
      return true;
    }
    auto InfoItr{find_if(mClauseInfo, [this](auto &V) {
      return V.first == std::get<ClauseId>(mClause);
    })};
    if (InfoItr == mClauseInfo.end()) {
      mClauseInfo.emplace_back();
      InfoItr = mClauseInfo.end() - 1;
      InfoItr->first = std::get<ClauseId>(mClause);
      InfoItr->second = VariableList{};
    }
    std::get<VariableList>(InfoItr->second).insert(Itr->get<MD>());
    return true;
  }

private:
  const ASTImportInfo *mImportInfo{nullptr};
  clang::Rewriter *mRewriter{nullptr};
  clang::SourceManager *mSrcMgr{nullptr};
  const clang::LangOptions *mLangOpts{nullptr};
  const ClangDIMemoryMatcherPass::DIMemoryMatcher *mMatcher{nullptr};
  std::tuple<clang::Stmt *, ClauseId> mClause{nullptr, ClauseId::NotClause};
  ClauseInfo mClauseInfo;
};

class APCClangDirectivesCollector : public ModulePass, bcl::Uncopyable {
public:
  static char ID;
  APCClangDirectivesCollector() : ModulePass(ID) {
    initializeAPCClangDirectivesCollectorPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

INITIALIZE_PROVIDER(APCClangDirectivesCollectorProvider,
                    "apc-directives-collector-provider",
                    "User Directives Collector (APC, Clang, Provider)")

char APCClangDirectivesCollector::ID = 0;
INITIALIZE_PASS_BEGIN(APCClangDirectivesCollector, "apc-directives-collector",
                      "User Directives Collector (APC, Clang)", true, true)
INITIALIZE_PASS_DEPENDENCY(APCClangDirectivesCollectorProvider)
INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(APCClangDirectivesCollector, "apc-directives-collector",
                    "User Directives Collector (APC, Clang)", true, true)

ModulePass *llvm::createAPCClangDirectivesCollector() {
  return new APCClangDirectivesCollector;
}

void APCClangDirectivesCollector::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<APCClangDirectivesCollectorProvider>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

bool APCClangDirectivesCollector::runOnModule(llvm::Module& M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  if (!TfmInfo) {
    M.getContext().emitError("cannot transform sources"
                             ": transformation context is not available");
    return false;
  }
  APCClangDirectivesCollectorProvider::initialize<TransformationEnginePass>(
      [&TfmInfo](TransformationEnginePass &Wrapper) { Wrapper.set(*TfmInfo); });
  auto &MemoryMatcher{getAnalysis<MemoryMatcherImmutableWrapper>()};
  APCClangDirectivesCollectorProvider::initialize<
      MemoryMatcherImmutableWrapper>(
      [&MemoryMatcher](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*MemoryMatcher);
      });
  ASTImportInfo ImportStub;
  const auto *ImportInfo{&ImportStub};
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  APCClangDirectivesVisitor Visitor{*ImportInfo};
  for (auto &F : M) {
    if (F.empty())
      continue;
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto *CU{DISub->getUnit()};
    if (!CU)
      continue;
    if (!isC(CU->getSourceLanguage()) && !isCXX(CU->getSourceLanguage()))
      continue;
    auto *TfmCtx{
        dyn_cast_or_null<ClangTransformationContext>(TfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance()) {
      F.getContext().emitError(
          "cannot transform sources"
          ": transformation context is not available for the '" +
          F.getName() + "' function");
      continue;
    }
    auto *FD{TfmCtx->getDeclForMangledName(F.getName())};
    if (!FD)
      continue;
    auto &Provider{getAnalysis<APCClangDirectivesCollectorProvider>(F)};
    auto &Matcher{Provider.get<ClangDIMemoryMatcherPass>().getMatcher()};
    Visitor.initialize(*TfmCtx, Matcher);
    Visitor.TraverseDecl(FD);
  }
  auto Itr{find_if(Visitor.getClauseInfo(), [](auto &V) {
    return V.first == ClauseId::ProcessPrivate;
  })};
  if (Itr != Visitor.getClauseInfo().end() &&
      !std::get<VariableList>(Itr->second).empty()) {
    auto &APCCtx{getAnalysis<APCContextWrapper>().get()};
    for (auto &&[Id, A] : APCCtx.mImpl->Arrays) {
      auto *S{A->GetDeclSymbol()};
      for (auto &Redecl : S->getVariable()) {
        auto *DIEM{cast<DIEstimateMemory>(Redecl.get<MD>())};
        if (std::get<VariableList>(Itr->second).count(DIEM->getVariable()))
          A->SetDistributeFlag(Distribution::SPF_PRIV);
      }
    }
  }
  return false;
}
