//=== InstructionMatcher.h - High and Low Level Instruction Matcher -- C++ ===//
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
// Classes and functions from this file match AST statements in a high-level
// code and appropriate instructions in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/InstructionMatcher.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>

#include "tsar/Analysis/Clang/Matcher.h"

using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "instruction-matcher"

char InstructionMatcherPass::ID = 0;
INITIALIZE_PASS_BEGIN(InstructionMatcherPass, "instruction-matcher",
  "High and Low Instruction Matcher", false, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(InstructionMatcherPass, "instruction-matcher",
  "High and Low Level Instruction Matcher", false, true)

using namespace clang;

namespace
{
  class StatementVisitor :
    public tsar::MatchASTBase<Instruction *, Stmt *>,
    public RecursiveASTVisitor<StatementVisitor> {
  public:
    /// \brief Constructor.
    ///
    /// \param SrcMgr Clang source manager to deal with locations.
    /// \param LM Representation of match.
    /// \param Unmatched Storage for unmatched ast entities.
    /// \param LocMap Map from entity location to a queue
    /// of IR entities.
    /// \param MacroMap A map form entity expansion location to a queue
    /// of AST entities. All entities explicitly (not implicit loops) defined in
    /// macros is going to store in this map. The key in this map is a raw
    /// encoding for expansion location.
    StatementVisitor(SourceManager& SrcMgr,
      Matcher& LM, UnmatchedASTSet& Unmatched,
      LocToIRMap& LocMap, LocToASTMap& MacroMap) :
      MatchASTBase(SrcMgr, LM, Unmatched, LocMap, MacroMap) {
    }

    bool VisitStmt(Stmt *S) {
      if (!S)
        return true;
      S->getBeginLoc().dump(*mSrcMgr);
      auto Itr = findItrForLocation(S->getBeginLoc());
      if (Itr == mLocToIR->end())
        return true;
      auto* Instruction = Itr->getSecond().front();
      Instruction = Instruction == nullptr
        ? findIRForLocation(S->getEndLoc())
        : Instruction;
      if (!Instruction)
        return true;
      dbgs() << "Found store instr for statement.\n";
      S->dump();
      Instruction->dump();
      return true;
    }

    bool VisitExpr(Expr* E) {
      if (!E)
        return true;
      auto Itr = findItrForLocation(E->getBeginLoc());
      if (Itr == mLocToIR->end())
        return true;
      auto* Instruction = Itr->getSecond().front();
      Instruction = Instruction == nullptr
        ? findIRForLocation(E->getEndLoc())
        : Instruction;
      if (!Instruction)
        return true;
      dbgs() << "Found store instr for expression.\n";
      E->dump();
      Instruction->dump();
      return true;
    }
  };
}

bool InstructionMatcherPass::runOnFunction(Function& F) {
  releaseMemory();
  auto& TfmInfo = getAnalysis<TransformationEnginePass>();
  if (!TfmInfo)
    return false;
  auto *TfmCtx = TfmInfo->getContext(*F.getParent());
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto *FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto& SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  StatementVisitor::LocToIRMap LocMap;
  // TODO: Fill in LocMap. Also rename LocMap.
  for (auto &BB : F) {
    for (auto &Instr : BB) {
      auto* StoreInstr = dyn_cast<StoreInst>(&Instr);
      if (!StoreInstr)
        continue;
      StoreInstr->dump();
      auto Loc = StoreInstr->getDebugLoc();
      if (!Loc)
        continue;
      Loc.dump();
      const auto Inserted = LocMap.insert(
        std::make_pair(Loc, TinyPtrVector<Instruction*>(&Instr))).second;
      assert(Inserted && "Dublicate of store instruction!");
    }
  }
  /*
  for (auto& Pair : LocToLoop)
    std::reverse(Pair.second.begin(), Pair.second.end());
  */
  StatementVisitor::LocToASTMap LocToMacro;
  StatementVisitor SV(SrcMgr, mMatcher, mUnmatchedAST, LocMap, LocToMacro);
  SV.TraverseDecl(FuncDecl);
  return false;
}

void InstructionMatcherPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass* llvm::createInstructionMatcherPass() {
  return new InstructionMatcherPass();
}