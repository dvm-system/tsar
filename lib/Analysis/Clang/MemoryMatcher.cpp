//===- MemoryMatcher.cpp - High and Low Level Memory Matcher ----*- C++ -*-===//
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
// This file implements pass to match memory.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/Matcher.h"
#include "tsar/Analysis/Clang/Passes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <llvm/ADT/DenseMap.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "memory-matcher"

namespace {
/// \biref This pass stores results of memory matcher pass, this pass simplify
/// access to result of memory matcher module pass from other functions passes.
///
/// Note that this is immutable pass so memory match will be freed when it
/// is destroyed only. To free the allocated memory explicitly releaseMemory()
/// method can be used.
class MemoryMatcherImmutableStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  MemoryMatcherImmutableStorage() : ImmutablePass(ID) {}

  /// Destructor, which explicitly calls releaseMemory() method.
  ~MemoryMatcherImmutableStorage() { releaseMemory(); }

  /// Returns memory matcher for the last analyzed module.
  const MemoryMatchInfo & getMatchInfo() const noexcept { return mMatchInfo; }

  /// Returns memory matcher for the last analyzed module.
  MemoryMatchInfo & getMatchInfo() noexcept { return mMatchInfo; }

  /// Releases allocated memory.
  void releaseMemory() override {
    mMatchInfo.Matcher.clear();
    mMatchInfo.UnmatchedAST.clear();
  }

private:
  MemoryMatchInfo mMatchInfo;
};

/// This pass matches variables and allocas (or global variables).
class MemoryMatcherPass :
  public ModulePass, private bcl::Uncopyable {
public:

  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  MemoryMatcherPass() : ModulePass(ID) {
    initializeMemoryMatcherPassPass(*PassRegistry::getPassRegistry());
  }

  /// Matches different memory locations.
  bool runOnModule(llvm::Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

char MemoryMatcherImmutableStorage::ID = 0;
INITIALIZE_PASS(MemoryMatcherImmutableStorage, "memory-matcher-is",
  "High and Low Memory Matcher (Immutable Storage)", true, true)

template<> char MemoryMatcherImmutableWrapper::ID = 0;
INITIALIZE_PASS(MemoryMatcherImmutableWrapper, "memory-matcher-iw",
  "High and Low Memory Matcher (Immutable Wrapper)", true, true)

char MemoryMatcherPass::ID = 0;
INITIALIZE_PASS_BEGIN(MemoryMatcherPass, "memory-matcher",
  "High and Low Memory Matcher", false , true)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableStorage)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_END(MemoryMatcherPass, "memory-matcher",
  "High and Low Level Memory Matcher", false, true)

STATISTIC(NumMatchMemory, "Number of matched memory units");
STATISTIC(NumNonMatchIRMemory, "Number of non-matched IR allocas");
STATISTIC(NumNonMatchASTMemory, "Number of non-matched AST variables");

namespace {
/// This matches allocas (IR) and variables (AST).
class MatchAllocaVisitor
    : public ClangMatchASTBase<MatchAllocaVisitor,
                               Value *, VarDecl *, DILocation *,
                               DILocationMapInfo, unsigned,
                               DenseMapInfo<unsigned>,
                               MemoryMatchInfo::MemoryMatcher>,
      public RecursiveASTVisitor<MatchAllocaVisitor> {
public:
  MatchAllocaVisitor(SourceManager &SrcMgr, Matcher &MM,
    UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
      ClangMatchASTBase(SrcMgr, MM, Unmatched, LocMap, MacroMap) {}

  /// Evaluates declarations expanded from a macro and stores such
  /// declaration into location to macro map.
  void VisitFromMacro(VarDecl *D) {
    assert(D->getBeginLoc().isMacroID() &&
      "Declaration must be expanded from macro!");
    auto Loc = D->getBeginLoc();
    if (Loc.isInvalid())
      return;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return;
    auto Itr{mLocToMacro->try_emplace(Loc.getRawEncoding()).first};
    Itr->second.push_back(D);
  }

  bool VisitVarDecl(VarDecl *D) {
    if (D->getBeginLoc().isMacroID()) {
      VisitFromMacro(D);
      return true;
    }
    auto VarLoc = D->getLocation();
    if (auto *AI = findIRForLocation(VarLoc)) {
      mMatcher->emplace(D->getCanonicalDecl(), AI);
      ++NumMatchMemory;
      --NumNonMatchIRMemory;
    } else {
      mUnmatchedAST->insert(D->getCanonicalDecl());
      ++NumNonMatchASTMemory;
    }
    return true;
  }

  /// For the specified function this stores pairs (DILocation *, AllocInst *)
  /// into the mLocToIR.
  void buildAllocaMap(Function &F) {
    DenseMap<const Value *, StringRef> ValueToName;
    for (auto &BB : F)
      for (auto &I : BB) {
        if (!isa<AllocaInst>(I))
          continue;
        ++NumNonMatchIRMemory;
        auto DIIList = FindDbgAddrUses(&I);
        // TODO (kaniandr@gmail.com): what should we do in case of multiple
        // dbg instrinsics?
        if (DIIList.size() != 1)
          continue;
        auto Var = DIIList.front()->getVariable();
        auto Loc = DIIList.front()->getDebugLoc();
        if (Var && Loc) {
          auto Itr{mLocToIR->try_emplace(Loc).first};
          Itr->second.push_back(&I);
          ValueToName.try_emplace(&I, Var->getName());
        }
      }
    for (auto &Pair : *mLocToIR)
      llvm::sort(Pair.second.begin(), Pair.second.end(),
                 [&ValueToName](const Value *LHS, const Value *RHS) {
                   return ValueToName[LHS] < ValueToName[RHS];
                 });
  }
};
}

bool MemoryMatcherPass::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto &Storage = getAnalysis<MemoryMatcherImmutableStorage>();
  Storage.releaseMemory();
  auto &MatchInfo = Storage.getMatchInfo();
  getAnalysis<MemoryMatcherImmutableWrapper>().set(MatchInfo);
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  if (!TfmInfo)
    return false;
  DenseMap<Value *, TinyPtrVector<VarDecl *>> Globals;
  for (Function &F : M) {
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
    if (!TfmCtx || !TfmCtx->hasInstance())
      continue;
    auto &SrcMgr{TfmCtx->getRewriter().getSourceMgr()};
    MatchAllocaVisitor::LocToIRMap LocToAlloca;
    MatchAllocaVisitor::LocToASTMap LocToMacro;
    MatchAllocaVisitor MatchAlloca(SrcMgr,
      MatchInfo.Matcher, MatchInfo.UnmatchedAST, LocToAlloca, LocToMacro);
    MatchAlloca.buildAllocaMap(F);
    // It is necessary to build LocToAlloca map also if FuncDecl is null,
    // because a number of unmatched allocas should be calculated.
    auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
    if (!FuncDecl)
      continue;
    MatchAlloca.TraverseDecl(FuncDecl);
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
      }    }
    MatchAlloca.matchInMacro(
      NumMatchMemory, NumNonMatchASTMemory, NumNonMatchIRMemory);
    for (auto &GlobalVar : M.globals()) {
      if (auto D = TfmCtx->getDeclForMangledName(GlobalVar.getName())) {
        Globals.try_emplace(&GlobalVar)
            .first->second.push_back(cast<VarDecl>(D->getCanonicalDecl()));
        ++NumMatchMemory;
      } else {
        ++NumNonMatchIRMemory;
      }
    }
  }
  tsar::ListBimap<
    bcl::tagged<clang::VarDecl*, tsar::AST>,
    bcl::tagged<llvm::Value *, tsar::IR>> MM;
  for (auto &&[V, Decls] : Globals)
    MatchInfo.Matcher.emplace(std::move(Decls), V);
  return false;
}

void MemoryMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableStorage>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass * llvm::createMemoryMatcherPass() { return new MemoryMatcherPass; }
