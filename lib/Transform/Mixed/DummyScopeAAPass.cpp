//=== DummyScopeAAPass.cpp - Source-level Based Dummy AA (Flang) *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a transformation pass which inserts 'noalias' and
// 'alias.scope' metadata for dummy arguments in Fortran programs.
// Fortran 2018 standard ensures that if two actual arguments aliases, the
// procedure cannot modify overlapped portions of memory.
//
// We produce the following code if possible:
//
// define void @foo(i64* %x, i64* %y, i64* %z) {
//   %0 = load i64, i64* %x, noalias !6
//   store i64 %0, i64* %z, !alias.scope !4, !noalias !5
// }
// !0 = !{0}
// !1 = !{!1, !0, !"noalias dummy x"}}
// !2 = !{!2, !0, !"noalias dummy y"}}
// !3 = !{!3, !0, !"noalias dummy z"}}
// !4 = !{!3}
// !5 = !{!1, !2}
// !6 = !{!2, !3}
//
// Note, that 'alias.scope' cannot be attached to loads because is still
// possible to read from overlapped portions of memory.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Transform/Mixed/Passes.h"
#include <bcl/utility.h>
#include <flang/Semantics/attr.h>
#include <flang/Semantics/symbol.h>
#include <flang/Semantics/type.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>

using namespace Fortran;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "dummy-no-alias"

namespace {
class FlangDummyAliasAnalysis : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  FlangDummyAliasAnalysis() : FunctionPass(ID) {
    initializeFlangDummyAliasAnalysisPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace

char FlangDummyAliasAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(FlangDummyAliasAnalysis, "flang-dummy-aa",
  "Source-level Based Dummy Alias Analysis (Flang)", true, false)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(FlangDummyAliasAnalysis, "flang-dummy-aa",
  "Source-level Based Dummy Alias Analysis (Flang)", true, false)

bool FlangDummyAliasAnalysis::runOnFunction(Function &F) {
  if (count_if(F.args(), [](const auto &Arg) {
        return isa<PointerType>(Arg.getType());
      }) < 2) {
    LLVM_DEBUG(dbgs() << "[DUMMY NOALIAS]: skip function " << F.getName()
                      << ": the number of pointer arguments is less than 2\n");
    return false;
  }
  auto *DISub{findMetadata(&F)};
  if (!DISub) {
    LLVM_DEBUG(dbgs() << "[DUMMY NOALIAS]: skip function " << F.getName()
                      << "without metadata\n");
    return false;
  }
  auto *CU{DISub->getUnit()};
  if (!isFortran(CU->getSourceLanguage())) {
    LLVM_DEBUG(dbgs() << "[DUMMY NOALIAS]: skip not Fortran function "
                      << F.getName() << "\n");
    return false;
  }
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<FlangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    LLVM_DEBUG(dbgs() << "[DUMMY NOALIAS]: skip function " << F.getName()
                      << ": transformation context is not available\n");
    return false;
  }
  auto ASTSub{TfmCtx->getDeclForMangledName(F.getName())};
  if (!ASTSub)
    return false;
  auto *Details{
      ASTSub.getSemanticsUnit()->detailsIf<semantics::SubprogramDetails>()};
  if (any_of(Details->dummyArgs(), [](auto *Dummy) {
    return Dummy->attrs().HasAny(
        {Fortran::semantics::Attr::TARGET, Fortran::semantics::Attr::POINTER});
  })) {
    LLVM_DEBUG(dbgs() << "[DUMMY NOALIAS]: skip function " << F.getName()
                      << ": a dummy has TARGET or POINTER attribute\n");
    return false;
  }
  if (Details->dummyArgs().size() != F.arg_size()) {
    LLVM_DEBUG(
        dbgs() << "[DUMMY NOALIAS]: skip function " << F.getName()
               << ": different number of dummy arguments in AST and IR\n");
    return false;
  }
  auto &Ctx = F.getContext();
  auto *Domain{MDNode::getDistinct(Ctx, {nullptr})};
  Domain->replaceOperandWith(0, Domain);
  SmallDenseMap<Value *, MDNode *, 8> Scopes;
  for (auto &Arg : F.args()) {
    if (!isa<PointerType>(Arg.getType()))
      continue;
    SmallString<32> Description;
    auto *Scope{MDNode::getDistinct(
        Ctx, {nullptr, Domain,
              MDString::get(Ctx, ("noalias dummy " + Arg.getName())
                                     .toStringRef(Description))})};
    Scope->replaceOperandWith(0, Scope);
    Scopes.try_emplace(&Arg, Scope);
  }
  const auto &DL = F.getParent()->getDataLayout();
  auto updateNoAliasMD = [&Ctx, &Scopes](Instruction &I, Value *V) {
    SmallVector<Metadata *, 8> NoAlias;
    NoAlias.reserve(Scopes.size() - 1);
    for (auto &&[Arg, Scope] : Scopes)
      if (Arg != V)
        NoAlias.push_back(Scope);
    MDNode *NoAliasMD{MDNode::get(Ctx, NoAlias)};
    if (auto *PrevNoAliasMD{I.getMetadata(LLVMContext::MD_noalias)})
      NoAliasMD = MDNode::concatenate(PrevNoAliasMD, NoAliasMD);
    I.setMetadata(LLVMContext::MD_noalias, NoAliasMD);
  };
  bool IsChanged = false;
  for (auto &I : instructions(F)) {
    if (auto SI = dyn_cast<StoreInst>(&I)) {
      auto BasePtr = getUnderlyingObject(SI->getPointerOperand(), 0);
      if (auto ArgItr = Scopes.find(BasePtr); ArgItr != Scopes.end()) {
        updateNoAliasMD(*SI, BasePtr);
        MDNode *ScopeMD{MDNode::get(Ctx, {ArgItr->second})};
        if (auto *PrevScopeMD{SI->getMetadata(LLVMContext::MD_alias_scope)})
          ScopeMD = MDNode::concatenate(PrevScopeMD, ScopeMD);
        SI->setMetadata(LLVMContext::MD_alias_scope, ScopeMD);
        IsChanged = true;
      }
    } else if (auto LI = dyn_cast<LoadInst>(&I)) {
      auto BasePtr = getUnderlyingObject(LI->getPointerOperand(), 0);
      if (auto ArgItr = Scopes.find(BasePtr); ArgItr != Scopes.end()) {
        updateNoAliasMD(*LI, BasePtr);
        IsChanged = true;
      }
    }
  }
  return IsChanged;
}

void FlangDummyAliasAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesCFG();
}

FunctionPass * llvm::createFlangDummyAliasAnalysis() {
  return new FlangDummyAliasAnalysis();
}
