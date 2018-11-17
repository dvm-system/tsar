//=== tsar_memory_matcher.h - High and Low Level Memory Matcher -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements pass to match memory.
//
//===----------------------------------------------------------------------===//

#include "tsar_memory_matcher.h"
#include "tsar_matcher.h"
#include "tsar_pass.h"
#include "tsar_transformation.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <llvm/ADT/DenseMap.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Local.h>

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
        auto DIIList = FindDbgAddrUses(&I);
        // TODO (kaniandr@gmail.com): what should we do in case of multiple
        // dbg instrinsics?
        if (DIIList.size() != 1)
          continue;
        auto Loc = DIIList.front()->getDebugLoc();
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
  auto &Storage = getAnalysis<MemoryMatcherImmutableStorage>();
  Storage.releaseMemory();
  auto &MatchInfo = Storage.getMatchInfo();
  getAnalysis<MemoryMatcherImmutableWrapper>().set(MatchInfo);
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  for (Function &F : M) {
    if (F.empty())
      continue;
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
    MatchAlloca.matchInMacro(
      NumMatchMemory, NumNonMatchASTMemory, NumNonMatchIRMemory);
  }
  for (auto &GlobalVar : M.globals()) {
    if (auto D = TfmCtx->getDeclForMangledName(GlobalVar.getName())) {
      MatchInfo.Matcher.emplace(
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
  AU.addRequired<MemoryMatcherImmutableStorage>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass * llvm::createMemoryMatcherPass() { return new MemoryMatcherPass; }
