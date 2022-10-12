//===- ControlFlowTraits.cpp - Traits of Control Flow in Region -*- C++ -*-===//
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
// This file implements passes to investigate a control flow inside a CFG region.
//
//===----------------------------------------------------------------------===//
#include "tsar/Analysis/Clang/ControlFlowTraits.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
/// This visitor collects all labels and `goto` statements which branch to these
/// labels.
struct LabelVisitor : public RecursiveASTVisitor<LabelVisitor> {
  using LabelMap = DenseMap<LabelDecl *, SmallVector<Stmt *, 8>>;

  bool VisitGotoStmt(GotoStmt *S) {
    auto I = Labels.try_emplace(S->getLabel());
    I.first->second.push_back(S);
    return true;
  }

  LabelMap Labels;
};

/// Collects traits for function and loops control-flow.
class Visitor : public RecursiveASTVisitor<Visitor> {
public:
  /// This map from declaration to a LLVM IR function allows to access function
  /// attributes which are available in LLVM IR.
  using CalleeMap = llvm::DenseMap<clang::Decl *, llvm::Function *>;
  using LabelMap = LabelVisitor::LabelMap;

  Visitor(const CalleeMap &Callees, const LabelMap &Labels,
    ClangCFTraitsPass::RegionCFInfo &FI, ClangCFTraitsPass::LoopCFInfo &LI) :
    mCallees(&Callees), mLabels(&Labels), mFuncInfo(&FI), mLoopInfo(&LI) {}

  bool TraverseStmt(clang::Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    bool Res = false;
    if (isa<ForStmt>(S) || isa<DoStmt>(S) || isa<WhileStmt>(S) ||
        isa<SwitchStmt>(S)) {
      mScopes.push_back(S);
      Res = RecursiveASTVisitor::TraverseStmt(S);
      mScopes.pop_back();
    } else {
      Res = RecursiveASTVisitor::TraverseStmt(S);
    }
    return Res;
  }

  bool VisitBreakStmt(BreakStmt *S) {
    assert(!mScopes.empty() && "Break statement without scope!");
    if (!isa<SwitchStmt>(mScopes.back())) {
      auto Itr = mLoopInfo->try_emplace(mScopes.back()).first;
      Itr->second.insert({ S, CFFlags::Exit });
    }
    return true;
  }

  bool VisitReturnStmt(ReturnStmt *S) {
    mFuncInfo->insert({ S, CFFlags::Exit });
    for (auto *LpS : mScopes) {
      if (isa<SwitchStmt>(LpS))
        continue;
      auto Itr = mLoopInfo->try_emplace(LpS).first;
      Itr->second.insert({ S, CFFlags::Exit });
    }
    return true;
  }

  bool VisitGotoStmt(GotoStmt *S) {
    mFuncInfo->insert({ S, CFFlags::DefaultFlags });
    auto Label = S->getLabel();
    auto LabelItr = mLabels->find(Label);
    if (LabelItr == mLabels->end()) {
      for (auto *LpS : mScopes) {
        if (isa<SwitchStmt>(LpS))
          continue;
        auto Itr = mLoopInfo->try_emplace(LpS).first;
        Itr->second.insert({ S, CFFlags::Exit });
      }
      return true;
    }
    auto &LoopsWithLabel = LabelItr->second;
    for (auto *LWithL : LoopsWithLabel) {
      auto I = std::find(mScopes.begin(), mScopes.end(), LWithL);
      if (I == mScopes.end()) {
        auto Itr = mLoopInfo->try_emplace(LWithL).first;
        Itr->second.insert({ S, CFFlags::Entry });
      } else {
        auto Itr = mLoopInfo->try_emplace(LWithL).first;
        Itr->second.insert({ S, CFFlags::DefaultFlags });
      }
    }
    for (auto *LpS : mScopes) {
      if (isa<SwitchStmt>(LpS))
        continue;
      auto I = std::find(LoopsWithLabel.begin(), LoopsWithLabel.end(), LpS);
      if (I == LoopsWithLabel.end()) {
        auto Itr = mLoopInfo->try_emplace(LpS).first;
        Itr->second.insert({ S, CFFlags::Exit });
      }
    }
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    auto *Callee = CE->getCalleeDecl();
    auto Flags = CFFlags::DefaultFlags;
    if (Callee) {
      Callee = Callee->getCanonicalDecl();
      auto FuncItr = mCallees->find(Callee);
      if (FuncItr != mCallees->end()) {
        if (!hasFnAttr(*FuncItr->second, AttrKind::AlwaysReturn))
          Flags |= CFFlags::MayNoReturn;
        if (!FuncItr->second->hasFnAttribute(Attribute::NoUnwind))
          Flags |= CFFlags::MayUnwind;
        if (FuncItr->second->hasFnAttribute(Attribute::ReturnsTwice))
          Flags |= CFFlags::MayReturnTwice;
        if (!hasFnAttr(*FuncItr->second, AttrKind::NoIO))
          Flags |= CFFlags::InOut;
      }
   } else {
     Flags |= CFFlags::UnsafeCFG | CFFlags::InOut;
   }
   for (auto *LpS : mScopes) {
      if (isa<SwitchStmt>(LpS))
        continue;
      auto &Info = mLoopInfo->try_emplace(LpS).first->second;
      Info.insert({ CE, Flags });
   }
   mFuncInfo->insert_call(Callee, { CE, Flags });
   return true;
  }

private:
  const CalleeMap *mCallees;
  const LabelMap *mLabels;
  ClangCFTraitsPass::LoopCFInfo *mLoopInfo;
  ClangCFTraitsPass::RegionCFInfo *mFuncInfo;
  SmallVector<Stmt *, 8> mScopes;
};
}

bool ClangCFTraitsPass::runOnFunction(Function &F) {
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
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError("cannot transform sources"
        ": transformation context is not available for the '" +
        F.getName() + "' function");
    return false;
  }
  auto CurrD = TfmCtx->getDeclForMangledName(F.getName());
  if (!CurrD)
    return false;
  Visitor::CalleeMap Callees;
  for (auto &I : instructions(F)) {
    auto *Call = dyn_cast<CallBase>(&I);
    if (!Call)
      continue;
    auto Callee = llvm::dyn_cast<Function>(
      Call->getCalledOperand()->stripPointerCasts());
    if (!Callee)
      continue;
    auto FD = TfmCtx->getDeclForMangledName(Callee->getName());
    if (!FD)
      continue;
    Callees.try_emplace(FD, Callee);
  }
  LabelVisitor LV;
  LV.TraverseDecl(CurrD);
  Visitor V(Callees, LV.Labels, mFuncInfo, mLoopInfo);
  V.TraverseDecl(CurrD);
  return false;
}

void ClangCFTraitsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

INITIALIZE_PASS_BEGIN(ClangCFTraitsPass, "clang-control-info",
  "Control Statement Information Pass (Clang)", false, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangCFTraitsPass, "clang-control-info",
  "Control Statement Information Pass (Clang)", false, true)

char ClangCFTraitsPass::ID = 0;

FunctionPass * llvm::createClangCFTraitsPass() { return new ClangCFTraitsPass; }
