
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
public:
  bool VisitDeclRefExpr(clang::DeclRefExpr* DRE);

  std::map<const clang::ValueDecl*, std::set<const clang::DeclRefExpr*>>
    getDeclRefs() const {
    return mDeclRefs;
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

  const clang::Stmt* isRedefined(const clang::ValueDecl* LHS,
    const clang::ValueDecl* RHS, clang::CFGBlock::iterator B,
    clang::CFGBlock::iterator E) const;

  std::vector<clang::Token> getRawTokens(const clang::SourceRange& SR) const;

  std::string getSourceText(const clang::SourceRange& SR) const;

  template<typename T>
  clang::SourceRange getRange(T* node) const;

  bool runOnFunction(Function& F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  tsar::TransformationContext* mTfmCtx;
  clang::ASTContext* mContext;
  clang::Rewriter* mRewriter;
  clang::SourceManager* mSourceManager;
  clang::FunctionDecl* mFuncDecl;
};
}
#endif//TSAR_FCOPY_ELIMINATION_H
