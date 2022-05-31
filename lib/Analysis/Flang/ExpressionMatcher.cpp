//=== ExpressionMatcher.cpp - High and Low Level Matcher (Flang) *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// Classes and functions from this file match expressions in Flang AST and
// appropriate expressions in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Flang/ExpressionMatcher.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Flang/Matcher.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include <flang/Parser/parse-tree-visitor.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "flang-expr-matcher"

using namespace Fortran;
using namespace llvm;
using namespace tsar;

STATISTIC(NumMatchExpr, "Number of matched expressions");
STATISTIC(NumNonMatchIRExpr, "Number of non-matched IR expressions");
STATISTIC(NumNonMatchASTExpr, "Number of non-matched AST expressions");

namespace {
class MatchExprVisitor : public FlangMatchASTBase<MatchExprVisitor, Value *,
                                                  FlangExprMatcherPass::NodeT> {
public:
  MatchExprVisitor(parser::AllCookedSources &AllCooked, Matcher &MM,
    UnmatchedASTSet &Unmatched, LocToIRMap &LocMap, LocToASTMap &MacroMap) :
      FlangMatchASTBase(AllCooked, MM, Unmatched, LocMap, MacroMap) {}

  template <typename T> bool Pre(T &N) { return true; }
  template <typename T> void Post(T &N) {}

  bool Pre(parser::ProgramUnit &PU) {
    if (mIsProcessed)
      return false;
    return mIsProcessed = true;
  }

  bool Pre(parser::InternalSubprogram &IS) {
    if (mIsProcessed)
      return false;
    return mIsProcessed = true;
  }

  bool Pre(parser::ModuleSubprogram &MS) {
    if (mIsProcessed)
      return false;
    return mIsProcessed = true;
  }

  template<typename T> void Post(parser::Statement<T>& S) {
    LLVM_DEBUG(if (auto Range{mAllCooked->GetProvenanceRange(S.source)}) {
      if (auto PLoc{
              mAllCooked->allSources().GetSourcePosition(Range->start())}) {
        using PLI = PresumedLocationInfo<parser::SourcePosition>;
        dbgs() << "[EXPR MATCHER]: try to match at " << PLI::getFilename(*PLoc)
               << ":" << PLI::getLine(*PLoc) << ":" << PLI::getColumn(*PLoc)
               << "\n";
        dbgs() << "[EXPR MATCHER]: call stack size " << mASTCallStack.size()
               << "\n";
        if (mAmbiguousCallStack)
          dbgs() << "[EXPR MATCHER]: call stack is ambiguous\n";
      }
    });
    // TODO (kaniandr@gmail.com): match multiple calls at the same level.
    // Debug locations for calls inside a single statement are equal at the
    // moment, so we cannot match nested calls if at a some level there are
    // multiple calls. For example, we cannot match calls which are arguments
    // of another call.
    if (auto Range{mAllCooked->GetProvenanceRange(S.source)})
      if (auto I{findItrForLocation(Range->start())}; I != mLocToIR->end())
        if (!mAmbiguousCallStack && I->second.size() == mASTCallStack.size()) {
          NumMatchExpr += I->second.size();
          NumNonMatchASTExpr -= I->second.size();
          for (auto N : reverse(mASTCallStack)) {
            mMatcher->emplace(N, I->second.back());
            I->second.pop_back();
          }
          mASTCallStack.clear();
        } else {
          I->second.clear();
        }
    for (auto N : mASTCallStack)
      mUnmatchedAST->insert(N);
    NumNonMatchASTExpr += mASTCallStack.size();
    mASTCallStack.clear();
    mAmbiguousCallStack = false;
    mWasCallOnLevel = false;
  }

  bool Pre([[maybe_unused]] parser::Call &) {
    mStashWasCallOnLevel = mWasCallOnLevel;
    mWasCallOnLevel = false;
    return true;
  }

  void Post([[maybe_unused]] parser::Call&) {
    mWasCallOnLevel = mStashWasCallOnLevel;
  }

  bool Pre(parser::CallStmt &CS) {
    mASTCallStack.emplace_back(&CS);
    if (mWasCallOnLevel)
      mAmbiguousCallStack = true;
    return true;
  }

  bool Pre(parser::FunctionReference &FR) {
    mASTCallStack.emplace_back(&FR);
    if (mWasCallOnLevel)
      mAmbiguousCallStack = true;
    return true;
  }

private:
  bool mIsProcessed{false};
  bool mWasCallOnLevel{false}, mStashWasCallOnLevel{false};
  bool mAmbiguousCallStack{false};
  SmallVector<FlangExprMatcherPass::NodeT, 4> mASTCallStack;
  SmallVector<Value *, 4> mIRCallStack;
};

struct PrintFunctor {
  template <typename T> void operator()() {
    if (auto *N{Node.dyn_cast<T>()})
      OS << N->v.source.ToString();
  }
  const FlangExprMatcherPass::NodeT &Node;
  llvm::raw_ostream &OS;
};
} // namespace

void FlangExprMatcherPass::print(raw_ostream &OS, const llvm::Module *M) const {
  if (mMatcher.empty() || !mTfmCtx || !mTfmCtx->hasInstance())
    return;
  auto &GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  for (auto &Match : mMatcher) {
    tsar::print(OS, cast<Instruction>(Match.get<IR>())->getDebugLoc(),
                GO.PrintFilenameOnly);
    OS << " ";
    NodeInfoT::ListT::for_each_type(PrintFunctor{Match.get<AST>(), OS});
    Match.get<IR>()->print(OS);
    OS << "\n";
  }
}

bool FlangExprMatcherPass::runOnFunction(Function &F) {
  releaseMemory();
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!isFortran(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  mTfmCtx = TfmInfo ? dyn_cast_or_null<FlangTransformationContext>(
                          TfmInfo->getContext(*CU))
                    : nullptr;
  if (!mTfmCtx || !mTfmCtx->hasInstance())
    return false;
  auto &AllCooked{mTfmCtx->getParsing().allCooked()};
  MatchExprVisitor::LocToIRMap LocToExpr;
  MatchExprVisitor::LocToASTMap LocToMacro;
  MatchExprVisitor MatchExpr(AllCooked,
    mMatcher, mUnmatchedAST, LocToExpr, LocToMacro);
  for (auto &I: instructions(F)) {
    if (auto II = llvm::dyn_cast<IntrinsicInst>(&I);
        II && (isDbgInfoIntrinsic(II->getIntrinsicID()) ||
               isMemoryMarkerIntrinsic(II->getIntrinsicID())))
      continue;
    if (!isa<CallBase>(I))
      continue;
    ++NumNonMatchIRExpr;
    auto Loc = I.getDebugLoc();
    if (Loc) {
      LLVM_DEBUG(dbgs() << "[EXPR MATCHER]: remember instruction ";
                 I.print(dbgs()); dbgs() << " at "; Loc.print(dbgs());
                 dbgs() << "\n");
      auto Itr{ LocToExpr.try_emplace(Loc).first };
      Itr->second.push_back(&I);
    }
  }
  for (auto &Pair : LocToExpr)
    std::reverse(Pair.second.begin(), Pair.second.end());
  // It is necessary to build LocToExpr map even if AST representation is
  // unknown, because a number of unmatched expressions should be calculated.
  auto ASTSub{mTfmCtx->getDeclForMangledName(F.getName())};
  if (!ASTSub || ASTSub.isDeclaration())
    return false;
  ASTSub.visit(
      [&MatchExpr](auto &PU, auto &S) { parser::Walk(PU, MatchExpr); });
  // TODO (kaniandr@gmail.com): collect expressions from macros, note that
  // there is no debug locations for such expressions at the moment.
  MatchExpr.matchInMacro(NumMatchExpr, NumNonMatchASTExpr, NumNonMatchIRExpr,
                         true);
  return false;
}

void FlangExprMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

char FlangExprMatcherPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(FlangExprMatcherPass, "flang-expr-matcher",
  "High and Low Expression Matcher (Flang)", false , true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_IN_GROUP_END(FlangExprMatcherPass, "flang-expr-matcher",
  "High and Low Level Expression Matcher (Flang)", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

FunctionPass * llvm::createFlangExprMatcherPass() {
  return new FlangExprMatcherPass;
}