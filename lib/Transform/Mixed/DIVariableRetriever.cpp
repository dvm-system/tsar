//=== DIVariableRetriever.cpp - Subroutine Retriever (Flang) --*- C++ -*-===//
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
// This file implements a pass which relies on Flang AST to insert metadata
// for variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Intrinsics.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/Flang/ExpressionMatcher.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Transform/Mixed/Passes.h"
#include "tsar/Support/Flang/PresumedLocationInfo.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/PassProvider.h"
#include <bcl/utility.h>
#include <flang/Parser/parse-tree.h>
#include <flang/Parser/parse-tree-visitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "di-variable-retriever"

using namespace Fortran;
using namespace llvm;
using namespace tsar;

namespace tsar {
} // namespace tsar

namespace {
using MatcherProvider = FunctionPassProvider<FlangExprMatcherPass>;

class FlangDIVariableRetrieverPass : public ModulePass,
                                     private bcl::Uncopyable {
public:
  static char ID;
  FlangDIVariableRetrieverPass() : ModulePass(ID) {
    initializeFlangDIVariableRetrieverPassPass(
        *PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace

INITIALIZE_PROVIDER(MatcherProvider, "di-variable-retrieve-provider",
                    "Variable Debug Info Retriever (Flang, Provider)")

char FlangDIVariableRetrieverPass::ID = 0;
INITIALIZE_PASS_BEGIN(FlangDIVariableRetrieverPass, "di-variable-retriever",
                "Variable Debug Info Retriever (Flang)", true, false)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MatcherProvider)
INITIALIZE_PASS_END(FlangDIVariableRetrieverPass, "di-variable-retriever",
                "Variable Debug Info Retriever (Flang)", true, false)

void FlangDIVariableRetrieverPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MatcherProvider>();
  AU.getPreservesAll();
}

static llvm::DIType *createIntrinsicType(const semantics::DeclTypeSpec *FlangT,
                                         llvm::Type &T,
                                         const llvm::DataLayout &DL,
                                         llvm::DIBuilder &DIB) {
  if (auto FlangIntrinsicT{FlangT->AsIntrinsic()})
    if (auto Size{DL.getTypeAllocSizeInBits(&T)}; !Size.isScalable()) {
      unsigned Encoding{dwarf::DW_ATE_unsigned_char};
      switch (FlangIntrinsicT->category()) {
      default:
        llvm_unreachable("Unsupported Fortran intrinsic type!");
        return nullptr;
      case common::TypeCategory::Integer:
        Encoding = dwarf::DW_ATE_signed;
        break;
      case common::TypeCategory::Real:
        Encoding = dwarf::DW_ATE_float;
        break;
      case common::TypeCategory::Logical:
        Encoding = dwarf::DW_ATE_boolean;
        break;
      case common::TypeCategory::Complex:
        Encoding = dwarf::DW_ATE_complex_float;
        break;
      case common::TypeCategory::Character:
        Encoding = dwarf::DW_ATE_signed;
        break;
      }
      return DIB.createBasicType(FlangT->AsFortran(), Size.getFixedSize(),
                                 Encoding);
    }
  return nullptr;
}

using DimensionSubranges = SmallVector<std::pair<int64_t, int64_t>, 8>;
using MaybeSize = std::optional<uint64_t>;


static std::tuple<MaybeSize, DimensionSubranges>
getDimensionSubranges(const semantics::ArraySpec& ArrayShape) {
  DimensionSubranges Subranges;
  MaybeSize Size{1};
  for (auto &ShapeSpec : ArrayShape) {
    Subranges.emplace_back(0, 1);
    if (!ShapeSpec.lbound().isExplicit() || !ShapeSpec.ubound().isExplicit()) {
      Size = std::nullopt;
      continue;
    }
    auto &LB{ShapeSpec.lbound().GetExplicit()};
    auto &UB{ShapeSpec.ubound().GetExplicit()};
    if (!LB || !UB) {
      Size = std::nullopt;
      continue;
    }
    auto LBConst{std::visit(
        common::visitors{
            [](auto &&) -> std::optional<int64_t> { return std::nullopt; },
            [](evaluate::Constant<evaluate::SubscriptInteger> C) {
              return std::optional{C.GetScalarValue()->ToInt64()};
            }},
        LB->u)};
    auto UBConst{std::visit(
        common::visitors{
            [](auto &&) -> std::optional<int64_t> { return std::nullopt; },
            [](evaluate::Constant<evaluate::SubscriptInteger> C) {
              return std::optional{C.GetScalarValue()->ToInt64()};
            }},
        UB->u)};
    if (LBConst && UBConst) {
      Subranges.back().first = *UBConst - *LBConst + 1;
      if (Size)
        *Size *= Subranges.back().first;
      Subranges.back().second = *LBConst;
    }
  }
  return std::tuple{Size, std::move(Subranges)};
}

static llvm::DIType *
createArrayType(const semantics::ArraySpec &ArrayShape,
                const semantics::DeclTypeSpec *FlangElementT,
                llvm::Type &ElementT, const llvm::DataLayout &DL,
                llvm::DIBuilder &DIB) {
  auto ArraySizes{getDimensionSubranges(ArrayShape)};
  auto DITy{createIntrinsicType(FlangElementT, ElementT, DL, DIB)};
  SmallVector<Metadata *, 8> DISubranges;
  transform(std::get<DimensionSubranges>(ArraySizes),
            std::back_inserter(DISubranges), [&DIB](auto &S) {
              return DIB.getOrCreateSubrange(S.second, S.first);
            });
  return DIB.createArrayType(std::get<MaybeSize>(ArraySizes)
                                 ? *std::get<MaybeSize>(ArraySizes) *
                                       DITy->getSizeInBits()
                                 : 0,
                             DL.getABITypeAlign(&ElementT).value(),
                             DITy, DIB.getOrCreateArray(DISubranges));
}

static void
scheduleToEraseWithOperands(Instruction &I,
                            SmallPtrSetImpl<Instruction *> &Visited,
                            SmallVectorImpl<Instruction *> &ToDelete) {
  if (!Visited.insert(&I).second)
    return;
  ToDelete.push_back(&I);
  for (auto &U : I.operands()) {
    if (!isa<Instruction>(U))
      continue;
    if (auto I{U->user_begin()}; ++I == U->user_end())
      scheduleToEraseWithOperands(*cast<Instruction>(U), Visited, ToDelete);
  }
}

bool FlangDIVariableRetrieverPass::runOnModule(llvm::Module &M) {
  DIBuilder DIB{M};
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  if (!TfmInfo)
    return false;
  auto &GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  MatcherProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GO](auto &Wrapper) { Wrapper.setOptions(&GO); });
  MatcherProvider::initialize<TransformationEnginePass>(
      [&TfmInfo](auto &Wrapper) { Wrapper.set(TfmInfo.get()); });
  auto &DL{M.getDataLayout()};
  for (auto &F: M) {
    auto *DISub{findMetadata(&F)};
    if (!DISub)
      continue;
    auto *CU{DISub->getUnit()};
    if (!CU || !isFortran(CU->getSourceLanguage()))
      continue;
    auto *TfmCtx{
        dyn_cast_or_null<FlangTransformationContext>(TfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance())
      continue;
    auto ASTSub{TfmCtx->getDeclForMangledName(F.getName())};
    if (!ASTSub || ASTSub.isDeclaration())
      continue;
    auto &Matcher{getAnalysis<MatcherProvider>(F)
                      .get<FlangExprMatcherPass>()
                      .getMatcher()};
    SmallPtrSet<Instruction *, 32> Visited;
    SmallVector<Instruction *, 32> ToDelete;
    for (auto &I: instructions(F)) {
      auto *CB{dyn_cast<CallBase>(&I)};
      if (!CB)
        continue;
      auto MatchItr{Matcher.find<IR>(CB)};
      if (MatchItr == Matcher.end() ||
          !MatchItr->get<AST>().is<parser::CallStmt *>())
        continue;
      auto FlangCS{MatchItr->get<AST>().get<parser::CallStmt *>()};
      if (!FlangCS->typedCall)
        continue;
      if (StringRef{FlangCS->typedCall->proc().GetName()}.compare_insensitive(
              getName(IntrinsicId::ast_reg_var)) != 0)
        continue;
      scheduleToEraseWithOperands(*CB, Visited, ToDelete);
      for (unsigned I = 0, EI = FlangCS->typedCall->arguments().size(); I < EI;
           ++I)
        if (auto *E{FlangCS->typedCall->UnwrapArgExpr(I)})
          if (auto EDR{evaluate::ExtractDataRef(*E)})
            if (const auto *SR{std::get_if<evaluate::SymbolRef>(&EDR->u)}) {
              const semantics::ArraySpec *ArrayShape{nullptr};
              if (auto ObjectDetails{
                      (**SR).detailsIf<semantics::ObjectEntityDetails>()};
                  ObjectDetails && ObjectDetails->IsArray() ||
                  ObjectDetails->IsCoarray())
                ArrayShape = &ObjectDetails->shape();
              auto Base{getUnderlyingObject(CB->getArgOperand(I), 0)};
              if (auto *GV{dyn_cast<GlobalVariable>(Base)}) {
                DIType *DITy{nullptr};
                if (ArrayShape) {
                  if (GV->getValueType()->isArrayTy()) {
                    auto ArrayInfo{arraySize(GV->getValueType())};
                    DITy = createArrayType(*ArrayShape, (**SR).GetType(),
                                           *std::get<llvm::Type *>(ArrayInfo),
                                           DL, DIB);
                  }
                } else {
                  DITy = createIntrinsicType((**SR).GetType(),
                                             *GV->getValueType(), DL, DIB);
                }
                if (!DITy)
                  DITy =
                      createStubType(M, GV->getType()->getAddressSpace(), DIB);
                auto *GVE{DIB.createGlobalVariableExpression(
                    DISub->getFile(), (**SR).name().ToString(), GV->getName(),
                    DISub->getFile(), DISub->getLine(), DITy, true,
                    !GV->isDeclaration())};
                GV->addDebugInfo(GVE);
              } else if (isa<PointerType>(Base->getType())) {
                Function::arg_iterator ArgItr{F.arg_end()};
                DIType *DITy{nullptr};
                if (auto *AI{dyn_cast<AllocaInst>(Base)}) {
                  if (!AI->isArrayAllocation())
                    DITy = createIntrinsicType(
                        (**SR).GetType(), *AI->getAllocatedType(), DL, DIB);
                } else if (ArgItr = find_if(
                               F.args(),
                               [Base](auto &Arg) { return &Arg == Base; }),
                           ArgItr != F.arg_end()) {
                  llvm::Type *ElementTy{getPointerElementType(*ArgItr)};
                  if (ElementTy) {
                    if (ArrayShape)
                      DITy = createArrayType(*ArrayShape, (**SR).GetType(),
                                             *ElementTy, DL, DIB);
                    else
                      DITy = createIntrinsicType((**SR).GetType(), *ElementTy,
                                                 DL, DIB);
                  }
                }
                if (!DITy)
                  DITy = createStubType(
                      M, cast<PointerType>(Base->getType())->getAddressSpace(),
                      DIB);
                DILocalVariable *DIVar{nullptr};
                Instruction *InsertBefore{
                    &*F.getEntryBlock().getFirstInsertionPt()};
                if (ArgItr != F.arg_end()) {
                  DIVar = DIB.createParameterVariable(
                      DISub, (**SR).name().ToString(), ArgItr->getArgNo() + 1,
                      DISub->getFile(), DISub->getLine(), DITy, false,
                      DINode::FlagZero);
                } else {
                  DIVar = DIB.createAutoVariable(
                      DISub, (**SR).name().ToString(), DISub->getFile(),
                      DISub->getLine(), DITy, false, DINode::FlagZero);
                  if (auto *I{dyn_cast<Instruction>(Base)})
                    InsertBefore = I->getNextNode();
                }
                DIB.insertDeclare(
                    Base, DIVar, DIExpression::get(M.getContext(), {}),
                    DILocation::get(M.getContext(), DISub->getLine(), 0, DISub),
                    InsertBefore);
              }
            }
    }
    for (auto *I : ToDelete)
      I->eraseFromParent();
  }
  return false;
}

ModulePass * llvm::createFlangDIVariableRetrieverPass() {
  return new FlangDIVariableRetrieverPass;
}