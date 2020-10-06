//===- SourceLocationTraverse.h - Source Location Traverse ------*- C++ -*-===//
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
// This file provides functionality to visit all SourceLocations's which are
// mentioned in AST and is associated with a specified object.
//
// Note, that some tokens in a source file have no locations in AST.
// For example, the following tokens have no locations:
// - ':' in label declaration,
// - '=' in initialization,
// - separators in attribute specifications,
// - and some other tokens.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SOURCE_LOCATION_TRAVERSE_H
#define TSAR_SOURCE_LOCATION_TRAVERSE_H

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>

namespace tsar {
/// \brief Calls a specified function for each known location in a statement.
//
/// TDOD (kaniandr@gmail.com): DeclRefExrp: in case of C++ visit
/// template keyword, qualifiers and angle brackets (getQualifierLoc(),
/// getLAngleLoc(), getRAngleLoc(), getTemplateKeywordLoc()).
///
/// TODO (kaniandr@gmail.com): add support of C++, including MemberDecl.
template<class FuncT>
void traverseSourceLocation(const clang::Stmt *S, FuncT &&F) {
  F(S->getBeginLoc());
  F(S->getEndLoc());
  if (auto Switch = llvm::dyn_cast<clang::SwitchCase>(S)) {
    F(Switch->getColonLoc());
    if (auto Case = llvm::dyn_cast<clang::CaseStmt>(S))
      F(Case->getEllipsisLoc());
  } else if (auto AS = llvm::dyn_cast<clang::AttributedStmt>(S)) {
    for (auto &A : AS->getAttrs())
      F(A->getLocation());
  } else if (auto If = llvm::dyn_cast<clang::IfStmt>(S)) {
    F(If->getElseLoc());
  } else if (auto Do = llvm::dyn_cast<clang::DoStmt>(S)) {
    F(Do->getWhileLoc());
  } else if (auto For = llvm::dyn_cast<clang::ForStmt>(S)) {
    F(For->getLParenLoc());
    F(For->getRParenLoc());
  } else if (auto IGS = llvm::dyn_cast<clang::IndirectGotoStmt>(S)) {
    F(IGS->getStarLoc());
  } else if (auto Asm = llvm::dyn_cast<clang::AsmStmt>(S)) {
    F(Asm->getAsmLoc());
  } else if (auto MSAsm = llvm::dyn_cast<clang::MSAsmStmt>(S)) {
    F(MSAsm->getLBraceLoc());
  } else if (auto E = llvm::dyn_cast<clang::Expr>(S)) {
    F(E->getExprLoc());
    if (auto Cast = llvm::dyn_cast<clang::CStyleCastExpr>(E)) {
      F(Cast->getRParenLoc());
    } else if (auto CondOp =
      llvm::dyn_cast<clang::AbstractConditionalOperator>(E)) {
      F(CondOp->getQuestionLoc());
      F(CondOp->getColonLoc());
    } else if (auto DIE = llvm::dyn_cast<clang::DesignatedInitExpr>(E)) {
      F(DIE->getEqualOrColonLoc());
      for (auto &D : DIE->designators()) {
        F(D.getBeginLoc());
        F(D.getEndLoc());
        if (D.isArrayRangeDesignator())
          F(D.getEllipsisLoc());
      }
    } else if (auto OVE = llvm::dyn_cast<clang::OpaqueValueExpr>(E)) {
      F(OVE->getLocation());
    } else if (auto DRE = llvm::dyn_cast<clang::DeclRefExpr>(E)) {
      F(DRE->getLocation());
    } else if (auto Member = llvm::dyn_cast<clang::MemberExpr>(E)) {
    } else if (auto Str = llvm::dyn_cast<clang::StringLiteral>(E)) {
      for (auto &Loc : llvm::make_range(Str->tokloc_begin(), Str->tokloc_end()))
        F(Loc);
    }
  }
}

/// Calls a specified function for each known location in a declaration.
template<class FuncT>
void traverseSourceLocation(const clang::Decl *D, FuncT &&F) {
  F(D->getBeginLoc());
  F(D->getEndLoc());
  F(D->getLocation());
  if (auto FSA = llvm::dyn_cast<clang::FileScopeAsmDecl>(D)) {
    F(FSA->getAsmLoc());
    F(FSA->getRParenLoc());
  } else if (auto Export = llvm::dyn_cast<clang::ExportDecl>(D)) {
    F(Export->getRBraceLoc());
  } else if (auto Namespace = llvm::dyn_cast<clang::NamespaceDecl>(D)) {
    F(Namespace->getRBraceLoc());
  } else if (auto DD = llvm::dyn_cast<clang::DeclaratorDecl>(D)) {
    F(DD->getInnerLocStart());
  }
}

/// Calls a specified function for each known location in a type.
template<class FuncT>
void traverseSourceLocation(clang::TypeLoc TL, FuncT &&F) {
  F(TL.getBeginLoc());
  F(TL.getEndLoc());
}
}
#endif//TSAR_SOURCE_LOCATION_TRAVERSE_H
