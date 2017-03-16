//===- tsar_pragma.cpp - SAPFOR Specific Pragma Parser ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements SAPFOR specific #pragma handlers.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/Decl.h>
#include <clang/AST/Stmt.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Instructions.h>
#include "tsar_pragma.h"

using namespace llvm;
using namespace tsar;

namespace clang {
void AnalysisPragmaHandler::HandlePragma(
    Preprocessor &PP, PragmaIntroducerKind, Token &FirstToken) {
  mTokenList.clear();
  AddToken(tok::l_brace, FirstToken.getLocation(), 1);
  mPragmaLocSet.insert(FirstToken.getLocation().getRawEncoding());
  Token Tok;
  PP.LexNonComment(Tok);
  do {
    if (!ConsumeClause(PP, Tok))
      return;
  } while (Tok.isNot(tok::eod));
  AddToken(tok::r_brace, Tok.getLocation(), 1);
#if (LLVM_VERSION_MAJOR < 4)
  Token *TokenArray = new Token[mTokenList.size()];
  std::copy(mTokenList.begin(), mTokenList.end(), TokenArray);
  PP.EnterTokenStream(TokenArray, mTOkenList.size(), false, true);
#else
  PP.EnterTokenStream(mTokenList, false);
#endif
}

inline void AnalysisPragmaHandler::AddToken(
    tok::TokenKind K, SourceLocation Loc, unsigned Len) {
  Token Tok;
  Tok.startToken();
  Tok.setKind(K);
  Tok.setLocation(Loc);
  Tok.setLength(Len);
  mTokenList.push_back(Tok);
}

void AnalysisPragmaHandler::AddClauseName(Preprocessor &PP, Token &Tok) {
  assert(Tok.is(tok::identifier) && "Token must be an identifier!");
  Token ClauseTok;
  ClauseTok.startToken();
  ClauseTok.setKind(tok::string_literal);
  SmallString<16> Name;
  raw_svector_ostream OS(Name);
  OS << '\"' << Tok.getIdentifierInfo()->getName() << '\"';
  PP.CreateString(
    OS.str(), ClauseTok, Tok.getLocation(), Tok.getLocation());
  mTokenList.push_back(ClauseTok);
}

bool AnalysisPragmaHandler::ConsumeClause(Preprocessor &PP, Token &Tok) {
  if (Tok.isNot(tok::identifier)) {
    PP.Diag(Tok.getLocation(), diag::err_expected) << "clause name";
    return false;
  }
  CheckClauseName CheckName(Tok.getIdentifierInfo()->getName());
  ASTDependencyDescriptor::for_each_available(CheckName);
  if (!CheckName.IsValid()) {
    unsigned DiagId = PP.getDiagnostics().getCustomDiagID(
      DiagnosticsEngine::Error, "unknown clause '%0'");
    PP.Diag(Tok.getLocation(), DiagId) << Tok.getIdentifierInfo()->getName();
    return false;
  }
  AddClauseName(PP, Tok);
  PP.LexNonComment(Tok);
  if (Tok.isNot(tok::l_paren)) {
    PP.Diag(Tok.getLocation(), diag::err_expected) << "'('";
    return false;
  }
  AddToken(tok::comma, Tok.getLocation(), 1);
  PP.LexNonComment(Tok);
  if (!ConsumeIdentifierList(PP, Tok))
    return false;
  if (Tok.isNot(tok::r_paren)) {
    PP.Diag(Tok.getLocation(), diag::err_expected) << "')'";
    return false;
  }
  AddToken(tok::semi, Tok.getLocation(), 1);
  PP.LexNonComment(Tok);
  return true;
}

bool AnalysisPragmaHandler::ConsumeIdentifierList(Preprocessor &PP, Token &Tok) {
  if (Tok.isNot(tok::identifier)) {
    PP.Diag(Tok.getLocation(), diag::err_expected) << "identifier";
    return false;
  }
  // Each identifier 'I' will be replace by (void)(sizeof((void)(I))).
  // This construction is necessary to disable warnings for unused expressions
  // (cast to void) and to disable generation of LLVM IR for it (sizeof).
  // Cast to void inside 'sizeof' operator is necessary in case of variable
  // length array:
  // int N;
  // double A[N];
  // (void)(sizeof(A)) // This produces LLVM IR which computes size in dynamic.
  // (void)(sizeof((void)(A))) // This does not produce LLVM IR.
  AddToken(tok::l_paren, Tok.getLocation(), 1);
  AddToken(tok::kw_void, Tok.getLocation(), 1);
  AddToken(tok::r_paren, Tok.getLocation(), 1);
  AddToken(tok::l_paren, Tok.getLocation(), 1);
  AddToken(tok::kw_sizeof, Tok.getLocation(), 1);
  AddToken(tok::l_paren, Tok.getLocation(), 1);
  AddToken(tok::l_paren, Tok.getLocation(), 1);
  AddToken(tok::kw_void, Tok.getLocation(), 1);
  AddToken(tok::r_paren, Tok.getLocation(), 1);
  AddToken(tok::l_paren, Tok.getLocation(), 1);
  mTokenList.push_back(Tok);
  AddToken(tok::r_paren, Tok.getLocation(), 1);
  AddToken(tok::r_paren, Tok.getLocation(), 1);
  AddToken(tok::r_paren, Tok.getLocation(), 1);
  PP.LexNonComment(Tok);
  if (Tok.is(tok::comma)) {
    mTokenList.push_back(Tok);
    PP.LexNonComment(Tok);
    return ConsumeIdentifierList(PP, Tok);
  }
  return true;
}

void AnalysisPragmaVisitor::VisitCompoundStmt(CompoundStmt *S) {
  SourceLocation Loc = S->getLocStart();
  if (!mHandler->isPragma(S))
    return;
  for (auto I = S->body_begin(), EI = S->body_end(); I != EI; ++I) {
    SmallVector<VarDecl *, 16> ClauseVars;
    HandleClause(S, ClauseVars);
  }
}

void AnalysisPragmaVisitor::VisitForStmt(ForStmt * S) {
  auto *DS = new ASTDependencySet;
  for (auto &Pair : mDeclToDptr)
    DS->insert(cast<VarDecl>(Pair.first), Pair.second);
  mPrivates.insert(std::make_pair(S, DS));
  mDeclToDptr.clear();
}

void AnalysisPragmaVisitor::HandleClause(
    Stmt *S, SmallVectorImpl<VarDecl *> &Vars) {
  assert(S && "Statement must not be null!");
  assert(!isa<BinaryOperator>(S) ||
    cast<BinaryOperator>(S)->getOpcode() == BO_Comma &&
    "Unexpected binary operator in clause!");
  if (!isa<BinaryOperator>(S)) {
    HandleClauseName(S, Vars);
  }
  auto CommapOp = cast<BinaryOperator>(S);
  Stmt *RHS = CommapOp->getRHS();
  // Skip cast to void.
  assert(isa<CastExpr>(RHS) &&
    cast<CastExpr>(RHS)->getCastKind() == CK_ToVoid &&
    "It must be cast to void!");
  RHS = *RHS->child_begin();
  // Skip parents: (...).
  assert(isa<ParenExpr>(RHS) && "It must be a parethesized expression!");
  RHS = *RHS->child_begin();
  // Skip sizeof.
  assert(isa<UnaryExprOrTypeTraitExpr>(RHS) &&
    cast<UnaryExprOrTypeTraitExpr>(RHS)->getKind() == UETT_SizeOf &&
    "It must be a sizeof expression!");
  RHS = *RHS->child_begin();
  // Skip parents: (...).
  assert(isa<ParenExpr>(RHS) && "It must be a parethesized expression!");
  RHS = *RHS->child_begin();
  // Skip cast to void.
  assert(isa<CastExpr>(RHS) &&
    cast<CastExpr>(RHS)->getCastKind() == CK_ToVoid &&
    "It must be cast to void!");
  RHS = *RHS->child_begin();
  // Skip implicit cast if it is exist. In case of expressions in a #pragma, for
  // example I + 1, there is no implicit cast.
  if (isa<ImplicitCastExpr>(RHS))
    RHS = *RHS->child_begin();
  // Skip parents: (...).
  assert(isa<ParenExpr>(RHS) && "It must be a parethesized expression!");
  RHS = *RHS->child_begin();
  // TODO (kaniandr@gamil.com) : At this moment only reference to a variable
  // can be placed into a #pragma. May be for some reasons more complex
  // expressions should be supported, for example to specify dependency length.
  assert(isa<DeclRefExpr>(RHS) &&
    "It must be a reference to a declared variable!");
  auto DeclRef = cast<DeclRefExpr>(RHS);
  assert(isa<VarDecl>(DeclRef->getDecl()) &&
    "It must be variable declaration!");
  auto Var= cast<VarDecl>(DeclRef->getDecl());
  Vars.push_back(Var);
  HandleClause(CommapOp->getLHS(), Vars);
}

void AnalysisPragmaVisitor::HandleClauseName(
  Stmt *S, clang::SmallVectorImpl<VarDecl *> &Vars) {
  assert(S && "Statement must not be null!");
  assert(isa<CastExpr>(S) &&
    cast<CastExpr>(S)->getCastKind() == CK_ArrayToPointerDecay &&
    "It must be cast from array to pointer!");
  auto RHS = *S->child_begin();
  assert(isa<StringLiteral>(RHS) && "Clause name must be a string literal!");
  auto Str = cast<StringLiteral>(RHS);
  ASTDependencyDescriptor::for_each_available(
    SetTraitForDecl(Str->getBytes(), Vars, mDeclToDptr));
}

void AnalysisPragmaVisitor::SanitizeIR(const DeclRefExpr *Ref) {
  assert(Ref && "Reference to a variable must not be null!");
  auto ASTLoc = Ref->getLocation();
}
}