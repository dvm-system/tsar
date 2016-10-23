//===--- tsar_pragma.h - SAPFOR Specific Pragma Parser ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines SAPFOR specific #pragma handlers.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRAGMA_H
#define TSAR_PRAGMA_H

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Lex/Pragma.h>
#include <llvm/ADT/SmallVector.h>
#include <set>
#include "tsar_ast_trait.h"

namespace clang {
class Preprocessor;
class Token;

/// This preliminary evaluates #pragma sapfor analysis ... , note that
/// subsequent evaluation should be performed after generation of AST.
class AnalysisPragmaHandler : public PragmaHandler {
  /// This set contains locations of each handled pragma.
  typedef std::set<unsigned> PragmaLocSet;

  /// This functor check that a specified name is a valid name of a some clause,
  /// which describes dependencies in a loop.
  struct CheckClauseName {
    CheckClauseName(StringRef ClauseName) :
      mIsValid(false), mClauseName(ClauseName) {}
    template<class Trait> void operator()() {
      std::string TraitStr = Trait::toString();
      TraitStr.erase(
        std::remove(TraitStr.begin(), TraitStr.end(), ' '), TraitStr.end());
      if (TraitStr == mClauseName)
        mIsValid = true;
    }
    bool IsValid() const { return mIsValid; }
  private:
    bool mIsValid;
    StringRef mClauseName;
  };

public:
  /// Creates handler.
  AnalysisPragmaHandler() : PragmaHandler("analysis") {}

  /// \brief Handles a #pragma sapfor ... .
  ///
  /// This checks syntax of a pragma and converts it to a sequence of tokens
  /// which will be parsed and evaluated later. For example,
  /// `#pragma sapfor induction(I)` will be converted to
  /// `{ "induction", (void)(sizeof((void)(I))); }`. This sequence of tokens is
  /// going to be represented as a compound statement in AST. For each pragma
  /// start location will be stored so it is possible to check whether some
  /// compound statement represents a pragma. To do this it is possible to use
  /// isPragma() method.
  void HandlePragma(Preprocessor &PP,
      PragmaIntroducerKind Introducer, Token &FirstToken) override;

  /// Returns true if a specified statement represent a #pragma sapfor ... .
  bool isPragma(Stmt *S) const {
    assert(S && "Statement must not be null!");
    auto Loc = S->getLocStart();
    return Loc.isValid() && mPragmaLocSet.count(Loc.getRawEncoding()) > 0;
  }

private:
  /// Inserts new token in the list of tokens.
  void AddToken(tok::TokenKind K, SourceLocation Loc, unsigned Len);

  /// Inserts an identifier in the list of tokens as a string literal.
  void AddClauseName(Preprocessor &PP, Token &Tok);

  /// Assumes that a current token is a clause name, otherwise produces
  /// an error diagnostic.
  bool ConsumeClause(Preprocessor &PP, Token &Tok);

  /// Assumes that a current token is the first identifier in a list of
  /// identifiers and traverses all such identifiers.
  bool ConsumeIdentifierList(Preprocessor &PP, Token &Tok);

  llvm::SmallVector<Token, 32> mTokenList;
  PragmaLocSet mPragmaLocSet;
};

/// This extracts user specified analysis information from #pragma
/// representation in AST.
class AnalysisPragmaVisitor :
  public RecursiveASTVisitor<AnalysisPragmaVisitor> {
private:
  /// Information about privatizability of variables for the analyzed region.
  typedef llvm::DenseMap<
    clang::Decl *, tsar::ASTDependencyDescriptor> DeclToDptrMap;

  /// This functor set trait identified by a clause name for all variables in
  /// a specified vector.
  struct SetTraitForDecl {
    SetTraitForDecl(StringRef ClauseName, const SmallVectorImpl<VarDecl *> &Vars,
      DeclToDptrMap &DeclToDptr) :
      mClauseName(ClauseName), mVars(Vars), mDeclToDptr(DeclToDptr) {}
    template<class Trait> void operator()() {
      std::string TraitStr = Trait::toString();
      TraitStr.erase(
        std::remove(TraitStr.begin(), TraitStr.end(), ' '), TraitStr.end());
      if (TraitStr != mClauseName)
        return;
      for (auto Decl : mVars) {
        tsar::ASTDependencyDescriptor TmpDptr;
        auto I = mDeclToDptr.insert(std::make_pair(Decl, TmpDptr)).first;
        I->second.set<Trait>();
      }
    }
  private:
    StringRef mClauseName;
    const SmallVectorImpl<VarDecl *> &mVars;
    AnalysisPragmaVisitor::DeclToDptrMap &mDeclToDptr;
  };
public:
  typedef llvm::DenseMap<
    clang::ForStmt *, tsar::ASTDependencySet *> ASTPrivateInfo;

  /// Creates visitor.
  explicit AnalysisPragmaVisitor(AnalysisPragmaHandler *H) : mHandler(H) {
    assert(H && "Pragma handler must not be null!");
  }

  /// Extracts analysis information from #pragma represented as a compound
  /// statement.
  void VisitCompoundStmt(CompoundStmt *S);

  /// Stores traits obtained from all #pragma constructions preceding the loop.
  void VisitForStmt(ForStmt *S);

private:
  /// \brief Handles a clause.
  ///
  /// This is a recursive method which visit all expressions concluded in a
  /// clause. This supposes that a clause comprises of a name of a trait and
  /// list of variables which satisfy it. All variables mentioned in a clause
  /// is going to be stored in a vector Vars. At the end mDeclToDptr map is
  /// going to be updated, so for all variables from Vars the appropriate
  /// trait will be set in this map.
  void HandleClause(Stmt *S, clang::SmallVectorImpl<VarDecl *> &Vars);

  /// \brief Handles name of a clause.
  ///
  /// This method is called by HandleClause() when name of clause is reached.
  void HandleClauseName(Stmt *S, SmallVectorImpl<VarDecl *> &Vars);

  /// Removes instructions from IR which corresponds appearance of a specified
  /// variable in a #pragma.
  void SanitizeIR(const DeclRefExpr *Ref);

  AnalysisPragmaHandler *mHandler;
  DeclToDptrMap mDeclToDptr;
  ASTPrivateInfo mPrivates;
};
}

#endif//TSAR_PRAGMA_H
