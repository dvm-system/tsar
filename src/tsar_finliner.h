//===--- tsar_finliner.h - Frontend Inliner (clang) -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares classes and methods necessary for function source-level 
/// inlining.
///
//===----------------------------------------------------------------------===//

#if !defined(TSAR_FINLINER_H)
#define TSAR_FINLINER_H

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"


namespace detail {
/// Contains information required for correct and complete function body
/// instantiation and access methods to it.
class Template {
public:
  clang::FunctionDecl* getFuncDecl() const {
    return mFuncDecl;
  }

  void setFuncDecl(clang::FunctionDecl* FD) {
    mFuncDecl = FD;
    return;
  }

  void addParmRef(clang::ParmVarDecl* PVD, clang::DeclRefExpr* DRE) {
    mParmRefs[PVD].push_back(DRE);
    return;
  }

  std::vector<clang::DeclRefExpr*> getParmRefs(clang::ParmVarDecl* PVD) const {
    auto pr = [PVD](const std::pair<clang::ParmVarDecl*,
      std::vector<clang::DeclRefExpr*>>&lhs) -> bool {
      return lhs.first == PVD;
    };
    if (std::find_if(std::begin(mParmRefs), std::end(mParmRefs), pr)
      == std::end(mParmRefs)) {
      return std::vector<clang::DeclRefExpr*>();
    } else {
      return mParmRefs.at(PVD);
    }
  }

  void addRetStmt(clang::ReturnStmt* RS) {
    mRSs.push_back(RS);
    return;
  }

  std::vector<clang::ReturnStmt*> getRetStmts() const {
    return mRSs;
  }

private:
  clang::FunctionDecl* mFuncDecl;
  std::map<clang::ParmVarDecl*, std::vector<clang::DeclRefExpr*>> mParmRefs;
  std::vector<clang::ReturnStmt*> mRSs;
};

/// Represents one specific place in user source code where one of specified
/// functions (for inlining) is called.
class TemplateInstantiation {
public:
  TemplateInstantiation(
    clang::FunctionDecl* FD, clang::Stmt* S, clang::CallExpr* CE)
    : mFuncDecl(FD), mStmt(S), mCallExpr(CE), mTemplate(nullptr) {
  }

  clang::FunctionDecl* getFuncDecl() const {
    return mFuncDecl;
  }

  void setFuncDecl(clang::FunctionDecl* FD) {
    mFuncDecl = FD;
    return;
  }

  clang::Stmt* getStmt() const {
    return mStmt;
  }

  clang::CallExpr* getCallExpr() const {
    return mCallExpr;
  }

  Template* getTemplate() const {
    return mTemplate;
  }

  void setTemplate(Template* T) {
    mTemplate = T;
    return;
  }

private:
  clang::FunctionDecl* mFuncDecl;
  clang::Stmt* mStmt;
  clang::CallExpr* mCallExpr;
  Template* mTemplate;
};
}
using namespace detail;

/// This class provides both AST traversing and source code buffer modification
/// (through Rewriter). Note that the only result of its work - modified
/// Rewriter (buffer) object passed by reference to its constructor.
class FInliner :
  public clang::RecursiveASTVisitor<FInliner>,
  public clang::ASTConsumer {
public:
  explicit FInliner(
    clang::CompilerInstance& Compiler, clang::Rewriter& Rewriter)
    : mCompiler(Compiler), mContext(mCompiler.getASTContext()),
    mRewriter(Rewriter) {
  }

  bool VisitFunctionDecl(clang::FunctionDecl* FD);

  bool VisitDeclRefExpr(clang::DeclRefExpr* DRE);

  bool VisitReturnStmt(clang::ReturnStmt* RS);

  bool VisitLabelStmt(clang::LabelStmt* LS);

  bool VisitNamedDecl(clang::NamedDecl* ND);

  /// Traverses AST, collects necessary information using overriden methods above
  /// and applies it to source code using private methods below
  void HandleTranslationUnit(clang::ASTContext& Context);

private:
  /// Constructs correct language declaration of \p identifier with \p type
  /// Uses bruteforce with linear complexity dependent on number of tokens
  /// in \p type where token is word or non-whitespace character.
  /// \returns vector of tokens which can be transformed to text string for
  /// insertion into source code
  std::vector<std::string> construct(
      const std::string& type, const std::string& identifier);

  /// Does instantiation of \p TI using \p args generating non-collidable
  /// identifiers/labels if necessary.
  /// \returns text of instantiated function body and result identifier
  std::pair<std::string, std::string> compile(
      const TemplateInstantiation& TI, const std::vector<std::string>& args,
      std::set<std::string>& identifiers, std::set<std::string>& labels);

  std::string getSourceText(const clang::SourceRange& SR) const;

  /// Merges \p _Cont of tokens to string using \p delimiter between each pair
  /// of tokens.
  template<typename _Container>
  std::string join(const _Container& _Cont, const std::string& delimiter) const;

  /// Exchanges contents of passed objects - useful if specified objects haven't
  /// necessary operators available (e.g. private operator=, etc).
  /// Used only for turning off llvm::errs() during bruteforce in construct() -
  /// each variant is attempted for parsing into correct AST (only one variant
  /// gives correct AST) with multiple warning and errors.
  template<typename T>
  void swap(T& lhs, T& rhs) const;

  /// Appends numeric suffix to the end of \p prefix, avoids collision using
  /// \p identifiers
  /// \returns new identifier (which is already inserted into identifiers)
  std::string addSuffix(
      const std::string& prefix, std::set<std::string>& identifiers) const;

  /// Splits string \p s into tokens using pattern \p p
  std::vector<std::string> tokenize(std::string s, std::string p) const;

  /// Replaces all undefined words in \p tokens with "int", removes special
  /// declaration specifiers. Allows to neglect original context of entity
  /// which is "encoded" in \p tokens. \p keywords are context-independent
  /// so it can be left
  /// \returns positions of removed declaration specifiers (for recovery)
  std::map<std::string, std::vector<int>> preprocess(
      std::vector<std::string>& tokens,
      const std::vector<std::string>& keywords) const;

  /// Method is opposite to previous - recovers removed declaration
  /// specifiers using \p positions
  void postprocess(
      std::vector<std::string>& tokens,
      std::map<std::string, std::vector<int>> positions) const;

  /// Local matcher to find correct node in AST during construct()
  class : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& MatchResult) {
      const clang::VarDecl* VD
        = MatchResult.Nodes.getNodeAs<clang::VarDecl>("varDecl");
      if (processor(VD->getType().getAsString()) == type) {
        ++count;
      }
      return;
    }
    void setParameters(
        const std::string& type,
        const std::function<std::string(const std::string&)>& processor) {
      this->type = type;
      this->processor = processor;
      return;
    }
    int getCount(void) const {
      return count;
    }
    void initCount(void) {
      this->count = 0;
      return;
    }
  private:
    std::string type;
    std::function<std::string(const std::string&)> processor;  // combination of tokenize, preprocess and join
    int count;
  } varDeclHandler;

  clang::CompilerInstance& mCompiler;
  clang::ASTContext& mContext;
  clang::Rewriter& mRewriter;

  clang::FunctionDecl* mCurrentFD;

  std::map<clang::FunctionDecl*, Template> mTs;
  std::map<clang::FunctionDecl*, std::vector<TemplateInstantiation>> mTIs;

  std::map<clang::FunctionDecl*, std::vector<clang::LabelStmt*>> mLSs;  // labeled statements in each function definition
  std::map<clang::FunctionDecl*, std::vector<clang::NamedDecl*>> mNDs;  // named declarations in each function definition
};

#endif
