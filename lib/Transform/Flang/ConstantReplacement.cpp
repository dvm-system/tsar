//=== ConstantReplacement.cpp - Constant Elimination Pass (Flang) *- C++ -*===//
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
// This file implement a pass to transform Fortran programs and to replace
// constants with variables that store corresponding values. This pass
// replaces constants in subscript expressions for arrays with known sizes.
// This transformation allows us to distinguish accesses to different arrays
// in LLVM IR, which are allocated in a single pool of memory,
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Flang/Diagnostic.h"
#include "tsar/Support/Flang/Rewriter.h"
#include "tsar/Transform/Flang/Passes.h"
#include <bcl/utility.h>
#include <flang/Evaluate/tools.h>
#include <flang/Parser/parse-tree.h>
#include <flang/Parser/parse-tree-visitor.h>
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
#define DEBUG_TYPE "flang-const-replacement"

namespace {
class FlangConstantReplacementPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  FlangConstantReplacementPass() : FunctionPass(ID) {
    initializeFlangConstantReplacementPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class ConstantCollector {
  using ConstantToProvenanceMap =
      StringMap<SmallVector<parser::ProvenanceRange, 4>>;
  using KindToConstantMap = StringMap<ConstantToProvenanceMap>;
public:
  explicit ConstantCollector(const parser::CookedSource &Cooked)
      : mCooked(Cooked) {}

  template <typename T> bool Pre(T &N) {
    PushBackIfArray(N);
    return true;
  }

  template <typename T> void Post(T &N) {
    if (!mNestedRefs.empty() && &N == mNestedRefs.back().first)
      mNestedRefs.pop_back();
  }

  bool Pre(parser::ExecutionPart &) {
    // We want to remember position of the first executable statement.
    // So, reset it at the beginning of execution part.
    mFirstStmt = std::nullopt;
    return true;
  }

  template <typename T> bool Pre(parser::Statement<T> &S) {
    if (!mFirstStmt)
      if (auto Range{mCooked.GetProvenanceRange(S.source)})
        mFirstStmt = Range->start();
    return true;
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

  bool Pre(parser::Name &N) {
    // We are looking for parameter constants.
    if (!mNestedRefs.empty() && mNestedRefs.back().second)
      if (const auto *S{N.symbol})
        if(S->attrs().HasAny({Fortran::semantics::Attr::PARAMETER})) {
          auto *Type{S->GetType()};
          if (auto *Intrinsic{Type->AsIntrinsic()}; Intrinsic &&
              Intrinsic->category() == common::TypeCategory::Integer)
            if (auto Range{mCooked.GetProvenanceRange(N.source)}) {
              auto KindItr{mConstantsToEliminate
                               .try_emplace(Intrinsic->kind().AsFortran())
                               .first};
              auto I{KindItr->second.try_emplace(N.source.ToString()).first};
              I->second.push_back(*Range);
            }
        }
    return true;
  }

  bool Pre(parser::KindParam &K) {
    // Do not replace kind of literal constant. The whole constant must be
    // replaced.
    return false;
  }

  bool Pre(parser::Expr &E) {
    if (PushBackIfArray(E) ||
        mNestedRefs.empty() || !mNestedRefs.back().second)
      return true;
    if (auto *L{std::get_if<parser::LiteralConstant>(&E.u)})
      if (auto *C{std::get_if<parser::IntLiteralConstant>(&L->u)})
        if (auto Range{mCooked.GetProvenanceRange(E.source)}) {
          auto KindItr{mConstantsToEliminate.end()};
          if (auto &Kind{std::get<std::optional<parser::KindParam>>(C->t)}) {
            std::string Param{std::visit(
                common::visitors{
                    [](auto &K) { return K.thing.thing.thing.ToString(); },
                    [](std::uint64_t K) { return std::to_string(K); }},
                Kind->u)};
            KindItr = mConstantsToEliminate.try_emplace(Param).first;
          } else {
            KindItr = mConstantsToEliminate.try_emplace("").first;
          }
          auto I{KindItr->second.try_emplace(E.source.ToString()).first};
          I->second.push_back(*Range);
        }
    return true;
  }

  const auto &getToEliminate() const noexcept { return mConstantsToEliminate; }
  std::optional<parser::Provenance> getFirstStmtProvenance() const {
    return mFirstStmt;
  }

private:
  template<typename T> bool PushBackIfArray(T &N) {
    if (auto *E{semantics::GetExpr(nullptr, N)})
      if (auto EDR{evaluate::ExtractDataRef(*E)})
        if (const auto *AR{std::get_if<evaluate::ArrayRef>(&EDR->u)}) {
          const semantics::Symbol &A{AR->GetLastSymbol()};
          if (const auto *D{A.detailsIf<semantics::ObjectEntityDetails>()})
            mNestedRefs.emplace_back(
                &N, D->shape().IsExplicitShape() ? AR : nullptr);
          return true;
        }
    return false;
  }

  const parser::CookedSource &mCooked;
  bool mIsProcessed{false};
  SmallVector<std::pair<void *, const evaluate::ArrayRef *>, 4> mNestedRefs;
  KindToConstantMap mConstantsToEliminate;
  std::optional<parser::Provenance> mFirstStmt;
};
}

char FlangConstantReplacementPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(FlangConstantReplacementPass,
  "flang-const-replacement", "Constant Replacement (Flang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(FlangConstantReplacementPass,
 "flang-const-replacement", "Constant Replacement (Flang)", false, false,
  TransformationQueryManager::getPassRegistry())

bool FlangConstantReplacementPass::runOnFunction(Function &F) {
  auto *DISub{findMetadata(&F)};
  if (!DISub) {
    LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: skip function " << F.getName()
                      << "without metadata\n");
    return false;
  }
  auto *CU{DISub->getUnit()};
  if (!isFortran(CU->getSourceLanguage())) {
    LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: skip not Fortran function "
                      << F.getName() << "\n");
    return false;
  }
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<FlangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: skip function " << F.getName()
                      << ": transformation context is not available\n");
    return false;
  }
  auto ASTSub{TfmCtx->getDeclForMangledName(F.getName())};
  if (!ASTSub || ASTSub.isDeclaration()) {
    LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: skip function " << F.getName()
                      << ": parse tree is not available\n");
    return false;
  }
  auto &Cooked{TfmCtx->getParsing().cooked()};
  auto &Rewriter{TfmCtx->getRewriter()};
  ConstantCollector DRV{TfmCtx->getParsing().cooked()};
  ASTSub.visit([&DRV](auto & PU, auto & S) { parser::Walk(PU, DRV); });
  if (DRV.getToEliminate().empty())
    return false;
  if (!DRV.getFirstStmtProvenance()) {
    toDiag(TfmCtx->getContext(), ASTSub.getSemanticsUnit()->name(),
           tsar::diag::warn_fortran_no_execution_part);
    return false;
  }
  auto ToInsert{*DRV.getFirstStmtProvenance()};
  if (TfmCtx->getOptions().isFixedForm) {
    // If the file has fixed form, we search the line end before the first
    // executable statement.
    const char *Start{Cooked.GetCharBlock(ToInsert)->begin()};
    do {
      --Start;
    } while (*Start != '\n');
    ToInsert = Cooked.GetProvenanceRange(Start)->start();
  }
  StringSet Replacement;
  auto findSymbol = [&GS = TfmCtx->getContext().globalScope(),
                     &LS = *ASTSub.getSemanticsUnit()->scope(),
                     &Replacement](StringRef Name) -> bool {
    if (auto *S{GS.FindSymbol(parser::CharBlock{Name.data(), Name.size()})})
      return true;
    if (auto *S{LS.FindSymbol(parser::CharBlock{Name.data(), Name.size()})})
      return true;
    return Replacement.count(Name);
  };
  auto addSuffix = [&findSymbol, &Replacement](const Twine &Prefix,
                                               SmallVectorImpl<char> &Out) {
    for (unsigned Count{0};
         findSymbol((Prefix + "_" + Twine{Count}).toStringRef(Out));
         ++Count, Out.clear())
      ;
    Replacement.insert(StringRef{Out.data(), Out.size()});
  };
  for (auto &KindToConst : DRV.getToEliminate()) {
    SmallString<256> Declaration;
    if (TfmCtx->getOptions().isFixedForm) {
      Declaration += "\n";
      Declaration.append(6, ' ');
    }
    // TODO (kaniandr@gmail.com): add option to choose between lower and upper
    // case
    Declaration += "integer";
    if (!KindToConst.getKey().empty()) {
      Declaration += "(kind=";
      Declaration += KindToConst.getKey();
      Declaration += ")";
    }
    Declaration += " ";
    for (auto &Const : KindToConst.second) {
      SmallString<16> VarName;
      addSuffix("i" + Const.getKey(), VarName);
      LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: replace " << Const.getKey()
                        << " with " << VarName << "\n");
      for (auto &Range : Const.second)
        Rewriter.ReplaceText(Range, VarName);
      if (Declaration.back() != ' ')
        Declaration += ", ";
      Declaration += VarName;
      SmallString<64> InitStmt;
      if (TfmCtx->getOptions().isFixedForm) {
        InitStmt += "\n";
        InitStmt.append(6, ' ');
      }
      InitStmt += VarName;
      InitStmt += " = ";
      InitStmt += Const.getKey();
      if (!TfmCtx->getOptions().isFixedForm)
        InitStmt += ";";
      LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: insert initialization "
                        << InitStmt << "\n");
      Rewriter.InsertText(ToInsert, InitStmt);
    }
    if (!TfmCtx->getOptions().isFixedForm)
      Declaration += ";";
    LLVM_DEBUG(dbgs() << "[CONST ELIMINATION]: insert declaration "
                      << Declaration << "\n");
    Rewriter.InsertText(ToInsert, Declaration, false);
  }
  return false;
}

void FlangConstantReplacementPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesCFG();
}

FunctionPass * llvm::createFlangConstantReplacementPass() {
  return new FlangConstantReplacementPass();
}
