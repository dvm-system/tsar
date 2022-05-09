//===- VariableRegistration.cpp - Variabler Registration (Flang) -*- C++ -*===//
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
// This file implements source-to-source transformation to register variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Intrinsics.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/Flang/Diagnostic.h"
#include "tsar/Support/Flang/Rewriter.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Flang/Passes.h"
#include <bcl/utility.h>
#include <flang/Evaluate/tools.h>
#include <flang/Parser/parse-tree-visitor.h>
#include <flang/Parser/parse-tree.h>
#include <flang/Semantics/tools.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <vector>

using namespace Fortran;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "flang-variable-registration"

namespace {
class FlangVariableRegistrationPass : public FunctionPass,
                                      private bcl::Uncopyable {
public:
  static char ID;
  FlangVariableRegistrationPass() : FunctionPass(ID) {
    initializeFlangVariableRegistrationPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class VariableCollector {
public:
  explicit VariableCollector(const parser::CookedSource &Cooked)
      : mCooked(Cooked) {}

  template <typename T> bool Pre(T &N) { return true; }

  template <typename T> void Post(T &N) {}

  bool Pre(parser::ExecutionPart &) {
    mInExecutionPart = true;
    return true;
  }

  void Post(parser::ExecutionPart &) { mInExecutionPart = false; }

  template <typename T> bool Pre(parser::Statement<T> &S) {
    if (mInExecutionPart)
      if (auto Range{mCooked.GetProvenanceRange(S.source)})
        mLastStmt = Range->start() + Range->size();
    return true;
  }

  std::optional<parser::Provenance> getLastStmtProvenance() const {
    return mLastStmt;
  }

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

private:
  const parser::CookedSource &mCooked;
  bool mIsProcessed{false};
  bool mInExecutionPart{false};
  std::optional<parser::Provenance> mLastStmt;
};
} // namespace

char FlangVariableRegistrationPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(FlangVariableRegistrationPass,
                               "flang-variable-registration",
                               "Variable Registration (Flang)", false, false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(FlangVariableRegistrationPass,
                             "flang-variable-registration",
                             "Variable Registration (Flang)", false, false,
                             TransformationQueryManager::getPassRegistry())

bool FlangVariableRegistrationPass::runOnFunction(Function &F) {
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!isFortran(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<FlangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto ASTSub{TfmCtx->getDeclForMangledName(F.getName())};
  if (!ASTSub || ASTSub.isDeclaration())
    return false;
  if (ASTSub.getSemanticsUnit()->scope()->GetSymbols().empty())
    return false;
  auto &Cooked{TfmCtx->getParsing().cooked()};
  auto &Rewriter{TfmCtx->getRewriter()};
  VariableCollector DRV{TfmCtx->getParsing().cooked()};
  ASTSub.visit([&DRV](auto &PU, auto &S) { parser::Walk(PU, DRV); });
  // Check if execution part exists.
  if (!DRV.getLastStmtProvenance())
    return false;
  auto ToInsert{*DRV.getLastStmtProvenance()};
  std::string Register;
  bool IsFirst{true};
  for (auto &S : ASTSub.getSemanticsUnit()->scope()->GetSymbols()) {
    if (S->attrs().HasAny({Fortran::semantics::Attr::PARAMETER}))
      continue;
    if (const auto *D{S->detailsIf<semantics::ObjectEntityDetails>()}) {
      if (IsFirst) {
        IsFirst = false;
        Register += "\n";
      }
      if (TfmCtx->getOptions().isFixedForm)
        Register.append(6, ' ');
      Register += ("call " + getName(IntrinsicId::ast_reg_var) + "(" +
                   S->name().ToString() + ")")
                      .str();
      if (!TfmCtx->getOptions().isFixedForm)
        Register += ";";
      Register += "\n";
    }
  }
  if (Register.empty())
    return false;
  Register.resize(Register.size() - 1);
  Rewriter.InsertText(ToInsert, Register, true);
  return false;
}

void FlangVariableRegistrationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesCFG();
}

FunctionPass *llvm::createFlangVariableRegistrationPass() {
  return new FlangVariableRegistrationPass();
}
