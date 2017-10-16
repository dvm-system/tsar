//=== tsar_memory_matcher.h - High and Low Level Memory Matcher -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements pass to match memory.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Transforms/Utils/Local.h>
#include "tsar_transformation.h"
#include "tsar_matcher.h"
#include "tsar_memory_matcher.h"

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "memory-matcher"

STATISTIC(NumMatchMemory, "Number of matched memory units");
STATISTIC(NumNonMatchIRMemory, "Number of non-matched IR allocas");
STATISTIC(NumNonMatchASTMemory, "Number of non-matched AST variables");

char MemoryMatcherPass::ID = 0;
INITIALIZE_PASS_BEGIN(MemoryMatcherPass, "memory-matcher",
  "High and Low Memory Matcher", true, true)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(MemoryMatcherPass, "memory-matcher",
  "High and Low Level Memory Matcher", true, true)

namespace {
/// This matches allocas (IR) and variables (AST).
class MatchAllocaVisitor :
  public MatchASTBase<Value, VarDecl>,
  public RecursiveASTVisitor<MatchAllocaVisitor> {
public:
  MatchAllocaVisitor(SourceManager &SrcMgr, Matcher &MM,
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
      mMatcher->emplace(D, AI);
      ++NumMatchMemory;
      --NumNonMatchIRMemory;
    } else {
      mUnmatchedAST->insert(D);
      ++NumNonMatchASTMemory;
    }
    return true;
  }

  /// For the specified function this stores pairs (DILocation *, AllocInst *)
  /// into the mLocToIR.
  void buildAllocaMap(Function &F) {
    for (auto &BB : F)
      for (auto &I : BB) {
        if (!isa<AllocaInst>(I))
          continue;
        ++NumNonMatchIRMemory;
        DbgDeclareInst *DDI = FindAllocaDbgDeclare(&I);
        if (!DDI)
          continue;
        auto Loc = DDI->getDebugLoc();
        if (Loc) {
          auto Pair = mLocToIR->insert(
            std::make_pair(Loc, bcl::TransparentQueue<Value>(&I)));
          if (!Pair.second)
            Pair.first->second.push(&I);
        }
      }
  }
};
}

bool MemoryMatcherPass::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  for (Function &F : M) {
    if (F.empty())
      continue;
    MatchAllocaVisitor::LocToIRMap LocToAlloca;
    MatchAllocaVisitor::LocToASTMap LocToMacro;
    MatchAllocaVisitor MatchAlloca(
      SrcMgr, mMatcher, mUnmatchedAST, LocToAlloca, LocToMacro);
    MatchAlloca.buildAllocaMap(F);
    // It is necessary to build LocToAlloca map also if FuncDecl is null,
    // because a number of unmatched allocas should be calculated.
    auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
    if (!FuncDecl)
      continue;
    MatchAlloca.TraverseDecl(FuncDecl);
    MatchAlloca.matchInMacro(
      NumMatchMemory, NumNonMatchASTMemory, NumNonMatchIRMemory);
  }
  for (auto &GlobalVar : M.globals()) {
    if (auto D = TfmCtx->getDeclForMangledName(GlobalVar.getName())) {
      mMatcher.emplace(
        static_cast<VarDecl *>(D), static_cast<Value*>(&GlobalVar));
      ++NumMatchMemory;
    } else {
      ++NumNonMatchIRMemory;
    }
  }
  return false;
}

void MemoryMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}
