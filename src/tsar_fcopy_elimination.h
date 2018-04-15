//==tsar_fcopy_propagation.h - Frontend Copy Propagation (clang) --*- C++ -*-=//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares methods necessary for source-level copy propagation.
///
//===----------------------------------------------------------------------===//

#include "tsar_pass.h"
#include "tsar_transformation.h"

#include <set>

#include <utility.h>
#include <clang/Analysis/CFG.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/Pass.h>

#ifndef TSAR_FCOPY_ELIMINATION_H
#define TSAR_FCOPY_ELIMINATION_H

namespace llvm {

class DeclRefVisitor : public clang::RecursiveASTVisitor<DeclRefVisitor> {
private:
  std::map<const clang::ValueDecl*, std::set<const clang::DeclRefExpr*>>
    mDeclRefs;
  std::set<const clang::UnaryOperator*> mUnaryOps;
public:
  bool VisitDeclRefExpr(clang::DeclRefExpr* DRE);

  bool VisitUnaryOperator(clang::UnaryOperator* UO);

  std::map<const clang::ValueDecl*, std::set<const clang::DeclRefExpr*>>
    getDeclRefs() const {
    return mDeclRefs;
  }

  std::set<const clang::UnaryOperator*> getUnaryOps() const {
    return mUnaryOps;
  }
};

class CopyEliminationPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  CopyEliminationPass() : FunctionPass(ID) {
    initializeCopyEliminationPassPass(*PassRegistry::getPassRegistry());
  }

public:
  bool runOnFunction(Function& F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  std::map<const clang::ValueDecl*, std::set<const clang::Stmt*>>
    getInC(unsigned int BlockID) const {
    return mIn[BlockID];
  }

  std::map<const clang::ValueDecl*, std::set<const clang::Stmt*>>
    getOutC(unsigned int BlockID) const {
    return mOut[BlockID];
  }

private:
  const clang::Stmt* isRedefined(const clang::ValueDecl* LHS,
    const clang::ValueDecl* RHS, clang::CFGBlock::iterator B,
    clang::CFGBlock::iterator E) const;

  std::vector<clang::Token> getRawTokens(const clang::SourceRange& SR) const;

  std::string getSourceText(const clang::SourceRange& SR) const;

  template<typename T>
  clang::SourceRange getRange(T* node) const;

private:
  tsar::TransformationContext* mTfmCtx;
  clang::ASTContext* mContext;
  clang::Rewriter* mRewriter;
  clang::SourceManager* mSourceManager;
  clang::FunctionDecl* mFuncDecl;

  /// per-block copy data grouped by identifiers
  std::vector<std::map<const clang::ValueDecl*,
    std::set<const clang::Stmt*>>> mIn;
  std::vector<std::map<const clang::ValueDecl*,
    std::set<const clang::Stmt*>>> mOut;
};
}
#endif//TSAR_FCOPY_ELIMINATION_H
