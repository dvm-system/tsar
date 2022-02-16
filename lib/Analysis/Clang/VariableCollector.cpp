//===-- VariableCollector.cpp - Variable Collector (Clang) -------*- C++ -*===//
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
// This file implements a visitor to collect variables referenced in a scope and
// to check whether metadata-level memory locations are safely represent these
// variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/VariableCollector.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Support/MetadataUtils.h"

using namespace clang;
using namespace llvm;
using namespace tsar;

static const clang::Type *getCanonicalUnqualifiedType(clang::VarDecl *VD) {
  return VD->getType()
      .getTypePtr()
      ->getCanonicalTypeUnqualified()
      ->getTypePtr();
}

/// Return number of nested pointer-like types.
static unsigned numberOfPointerTypes(const clang::Type *T) {
  if (auto PtrT = dyn_cast<clang::PointerType>(T))
    return numberOfPointerTypes(PtrT->getPointeeType().getTypePtr()) + 1;
  if (auto RefT = dyn_cast<clang::ReferenceType>(T))
    return numberOfPointerTypes(RefT->getPointeeType().getTypePtr()) + 1;
  if (auto ArrayT = dyn_cast<clang::ArrayType>(T))
    return numberOfPointerTypes(ArrayT->getElementType().getTypePtr());
  return 0;
}

bool VariableCollector::VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
  auto *ND = DRE->getFoundDecl();
  assert(ND && "Declaration must not be null!");
  if (isa<clang::VarDecl>(ND)) {
    auto *VD = cast<clang::VarDecl>(ND->getCanonicalDecl());
    auto T = getCanonicalUnqualifiedType(VD);
    unsigned PtrTpNum = numberOfPointerTypes(T);
    if (PtrTpNum == 0 && VD->getType().isConstQualified())
      return true;
    CanonicalRefs.try_emplace(VD).first->second.resize(PtrTpNum + 1);
  }
  return true;
}

bool VariableCollector::VisitDeclStmt(clang::DeclStmt *DS) {
  for (auto *D : DS->decls())
    if (auto *Var = dyn_cast<clang::VarDecl>(D->getCanonicalDecl()))
      CanonicalLocals.insert(Var);
  return true;
}

bool VariableCollector::TraverseLoopIteration(clang::ForStmt *For) {
  assert(For && "Statement must not be null!");
  for (auto *Child : For->children()) {
    if (!Child)
      continue;
    if (Child == For->getInit()) {
      if (auto DS{dyn_cast<DeclStmt>(Child)})
        if (!VisitDeclStmt(DS))
          return false;
    } else if (!TraverseStmt(Child)) {
      return false;
    }
  }
  return true;
}

std::tuple<clang::VarDecl *, DIMemory *, VariableCollector::DeclSearch>
VariableCollector::findDecl(const DIMemory &DIM,
    const DIMemoryMatcher &ASTToClient,
    const ClonedDIMemoryMatcher &ClientToServer) {
  auto *M = const_cast<DIMemory *>(&DIM);
  if (auto *DIEM = dyn_cast<DIEstimateMemory>(M)) {
    auto CSMemoryItr = ClientToServer.find<Clone>(DIEM);
    // Transformation of LLVM IR may introduce some new lower-level memory'
    // locations in the alias tree, which subsequently implies
    // new metadata-level memory locations.
    if (CSMemoryItr == ClientToServer.end())
      return std::tuple(nullptr, nullptr, Unknown);
    auto *DIVar =
        cast<DIEstimateMemory>(CSMemoryItr->get<Origin>())->getVariable();
    assert(DIVar && "Variable must not be null!");
    auto MatchItr = ASTToClient.find<MD>(DIVar);
    if (MatchItr == ASTToClient.end())
      if (isStubVariable(*DIVar))
          return std::tuple(nullptr, nullptr, Unknown);
      else
        return std::tuple(nullptr, nullptr, Invalid);
    auto ASTRefItr = CanonicalRefs.find(MatchItr->get<AST>());
    if (ASTRefItr == CanonicalRefs.end())
      return std::tuple(MatchItr->get<AST>(), CSMemoryItr->get<Origin>(),
                        Implicit);
    if (DIEM->getExpression()->getNumElements() > 0) {
      auto *Expr = DIEM->getExpression();
      auto NumDeref = llvm::count(Expr->getElements(), dwarf::DW_OP_deref);
      // At first, check that metadata-level memory is larger then allocated
      // memory. For example:
      // - metadata memory location is <X, ?>
      // - `int X`
      // Size of X will be unknown if there is a call which takes &X as
      // a parameter: bar(&X).
      if (NumDeref == 0 && ASTRefItr->second.size() == 1 &&
          Expr->isFragment() && Expr->getNumElements() == 3 &&
          !DIEM->isSized()) {
        ASTRefItr->second.front() = {DIEM, DK_Strong};
        return std::tuple(MatchItr->get<AST>(), CSMemoryItr->get<Origin>(),
                          isa<DILocalVariable>(DIVar) ? CoincideLocal
                                                      : CoincideGlobal);
      }
      auto *T = getCanonicalUnqualifiedType(ASTRefItr->first);
      auto isZeroOffsets = [](DIExpression *Expr) {
        // Now we check whether all offsets are zero. On success,
        // this means that all possible offsets are represented by
        // the template memory location DIEM.
        for (auto &Op : Expr->expr_ops())
          switch (Op.getOp()) {
          default:
            llvm_unreachable("Unsupported kind of operand!");
            return false;
          case dwarf::DW_OP_deref:
            break;
          case dwarf::DW_OP_LLVM_fragment:
          case dwarf::DW_OP_constu:
          case dwarf::DW_OP_plus_uconst:
          case dwarf::DW_OP_plus:
          case dwarf::DW_OP_minus:
            if (Op.getArg(0) != 0)
              return false;
          }
        return true;
      };
      // We want to be sure that current memory location describes all
      // possible memory locations which can be represented with a
      // corresponding variable and a specified number of its dereferences.
      // For example:
      // - <A,10> is sufficient to represent all memory defined by
      //   `int A[10]` (0 deref),
      // - <A,8> and <*A,?> are sufficient to represent all memory defined by
      //   `int (*A)[10]` (0 deref and 1 deref respectively).
      // - <A,8>, <*A,?>, <*A[?],?> are sufficient to represent all memory
      //   defined by `int **A` (0, 1 and 2 deref respectively).
      if (NumDeref < ASTRefItr->second.size()) {
        if (!DIEM->isSized()) {
          if ((NumDeref == 1 && (NumDeref == Expr->getNumElements() ||
                                Expr->isFragment() &&
                                    NumDeref == Expr->getNumElements() - 3)) ||
            (DIEM->isTemplate() && isZeroOffsets(Expr)))
          ASTRefItr->second[NumDeref] = {DIEM, DK_Strong};
        } else if (Expr->isFragment() && isZeroOffsets(Expr)) {
          ASTRefItr->second[NumDeref] = {DIEM, DK_Bounded};
        } else {
          ASTRefItr->second[NumDeref].Memory.push_back(DIEM);
          ASTRefItr->second[NumDeref].Kind = DK_Partial;
        }
      }
      return std::tuple(MatchItr->get<AST>(), CSMemoryItr->get<Origin>(),
                        Derived);
    }
    ASTRefItr->second.front() = {DIEM, DK_Strong};
    return std::tuple(MatchItr->get<AST>(), CSMemoryItr->get<Origin>(),
                      isa<DILocalVariable>(DIVar) ? CoincideLocal
                                                  : CoincideGlobal);
  }
  if (cast<DIUnknownMemory>(M)->isDistinct())
    return std::tuple(nullptr, nullptr, Unknown);
  return std::tuple(nullptr, nullptr, Useless);
}

bool VariableCollector::localize(DIAliasTrait &TS,
              const DIMemoryMatcher &ASTToClient,
              const ClonedDIMemoryMatcher &ClientToServer,
              SortedVarListT &VarNames, SortedVarListT &LocalVarNames,
              SortedVarMultiListT *Error) {
  bool IsOk{true};
  for (auto &T : TS)
    IsOk &= localize(*T, *TS.getNode(), ASTToClient, ClientToServer, VarNames,
                     LocalVarNames, Error);
  return IsOk;
}

bool VariableCollector::localize(DIMemoryTrait &T, const DIAliasNode &DIN,
    const DIMemoryMatcher &ASTToClient,
    const ClonedDIMemoryMatcher &ClientToServer,
    SortedVarListT &VarNames, SortedVarListT &LocalVarNames,
    SortedVarMultiListT *Error) {
  auto Res = localize(T, DIN, ASTToClient, ClientToServer);
  if (!std::get<2>(Res)) {
    if (Error && std::get<VarDecl *>(Res))
      Error->emplace(std::get<VarDecl *>(Res), std::get<DIMemory *>(Res));
    return false;
  }
  if (std::get<VarDecl *>(Res))
    if (!std::get<3>(Res))
      VarNames.emplace(std::get<VarDecl *>(Res), std::get<DIMemory *>(Res));
    else
      LocalVarNames.emplace(std::get<VarDecl *>(Res),
                            std::get<DIMemory *>(Res));
  return true;
}

std::tuple<clang::VarDecl *, DIMemory*, bool, bool> VariableCollector::localize(
    DIMemoryTrait &T, const DIAliasNode &DIN,
    const DIMemoryMatcher &ASTToClient,
    const ClonedDIMemoryMatcher &ClientToServer) {
  auto Search = findDecl(*T.getMemory(), ASTToClient, ClientToServer);
  // Do no specify traits for variables declared in a loop body
  // these variables are private by default. Moreover, these variables are
  // not visible outside the loop and could not be mentioned in clauses
  // before loop.
  if (std::get<VarDecl *>(Search) &&
      CanonicalLocals.count(std::get<VarDecl *>(Search)))
    return std::tuple(std::get<VarDecl *>(Search), std::get<DIMemory *>(Search),
                      true, true);
  if (std::get<DeclSearch>(Search) == VariableCollector::CoincideLocal) {
    return std::tuple(std::get<VarDecl *>(Search), std::get<DIMemory *>(Search),
                      true, false);
  } else if (std::get<DeclSearch>(Search) == VariableCollector::CoincideGlobal) {
    GlobalRefs.try_emplace(const_cast<DIAliasNode *>(&DIN),
                           std::get<VarDecl *>(Search));
    return std::tuple(std::get<VarDecl *>(Search), std::get<DIMemory *>(Search),
                      true, false);
  } else if (std::get<DeclSearch>(Search) != VariableCollector::Unknown) {
    return std::tuple(std::get<VarDecl *>(Search), std::get<DIMemory *>(Search),
                      false, false);
  }
  return std::tuple(nullptr, nullptr, true, false);
}
