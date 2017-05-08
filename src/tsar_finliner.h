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

#ifdef FINLINER

#if !defined(TSAR_FINLINER_H)
#define TSAR_FINLINER_H

#include "tsar_action.h"
#include "tsar_transformation.h"

#include <set>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/IR/Module.h>

namespace detail {

/// Contains information required for correct and complete function body
/// instantiation and access methods to it
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
  /// mFuncDecl == nullptr <-> instantiation is disabled for all calls
  clang::FunctionDecl* mFuncDecl;
  std::map<clang::ParmVarDecl*, std::vector<clang::DeclRefExpr*>> mParmRefs;
  std::vector<clang::ReturnStmt*> mRSs;
};

/// Represents one specific place in user source code where one of specified
/// functions (for inlining) is called
struct TemplateInstantiation {
  clang::FunctionDecl* mFuncDecl;
  clang::Stmt* mStmt;
  clang::CallExpr* mCallExpr;
  /// mTemplate == nullptr <-> instantiation is disabled for this call
  Template* mTemplate;
};

inline bool operator==(
  const TemplateInstantiation& lhs, const TemplateInstantiation& rhs) {
  return lhs.mFuncDecl == rhs.mFuncDecl
    && lhs.mStmt == rhs.mStmt
    && lhs.mCallExpr == rhs.mCallExpr
    && lhs.mTemplate == rhs.mTemplate;
}

}

namespace tsar {

/// This class provides both AST traversing and source code buffer modification
/// (through Rewriter API). Note that the only result of its work - modified
/// Rewriter (buffer) object inside passed Transformation Context.
class FInliner :
  public clang::RecursiveASTVisitor<FInliner>,
  public clang::ASTConsumer {
public:
  explicit FInliner(
      clang::CompilerInstance& CI, llvm::StringRef InFile,
      tsar::TransformationContext* TfmCtx, tsar::QueryManager* QM)
      : mCompiler(CI), mContext(mCompiler.getASTContext()),
      mSourceManager(mContext.getSourceManager()),
      mLLVMContext(new llvm::LLVMContext),
      mGen(clang::CreateLLVMCodeGen(CI.getDiagnostics(), InFile,
        CI.getHeaderSearchOpts(), CI.getPreprocessorOpts(),
        CI.getCodeGenOpts(), *mLLVMContext)), mTransformContext(TfmCtx),
      mQueryManager(QM) {
    assert(mTransformContext && "Transformation context must not be null!");
    mTransformContext->reset(mContext, *mGen);
    mRewriter = &TfmCtx->getRewriter();
  }

  bool VisitFunctionDecl(clang::FunctionDecl* FD);

  bool VisitForStmt(clang::ForStmt* FS);

  bool VisitDeclRefExpr(clang::DeclRefExpr* DRE);

  bool VisitReturnStmt(clang::ReturnStmt* RS);

  bool VisitExpr(clang::Expr* E);

  /// Traverses AST, collects necessary information using overriden methods above
  /// and applies it to source code using private methods below
  void HandleTranslationUnit(clang::ASTContext& Context);

private:
  /// Constructs correct language declaration of \p identifier with \p type
  /// Uses bruteforce with linear complexity dependent on number of tokens
  /// in \p type where token is non-whitespace character or special sequence.
  /// \p context is a string containing declarations used in case of referencing
  /// in \p type.
  /// \returns vector of tokens which can be transformed to text string for
  /// insertion into source code
  std::vector<std::string> construct(
    const std::string& type, const std::string& identifier,
    const std::string& context, std::map<std::string, std::string>& replacements);

  /// Does instantiation of \p TI using \p args generating non-collidable
  /// identifiers/labels if necessary. Since instantiation is recursive,
  /// collects all visible and newly created named declarations in \p decls
  /// to avoid later possible collisions.
  /// \returns text of instantiated function body and result identifier
  std::pair<std::string, std::string> compile(
    const detail::TemplateInstantiation& TI,
    const std::vector<std::string>& args,
    std::set<std::string>& decls);

  std::string getSourceText(const clang::SourceRange& SR) const;

  /// T must provide getSourceRange() method
  template<typename T>
  clang::SourceRange getSpellingRange(T* node) const;

  /// Merges \p _Cont of tokens to string using \p delimiter between each pair
  /// of tokens.
  template<typename _Container>
  std::string join(const _Container& _Cont, const std::string& delimiter) const;

  /// Exchanges contents of passed objects - useful if specified objects haven't
  /// necessary operators available (e.g. private operator=, etc).
  /// Used only for turning off llvm::errs() during bruteforce in construct() -
  /// each variant is attempted for parsing into correct AST (only one variant
  /// gives correct AST) with multiple warnings and errors.
  template<typename T>
  void swap(T& lhs, T& rhs) const;

  /// Appends numeric suffix to the end of \p prefix, avoids collision using
  /// \p identifiers
  /// \returns new identifier (which is already inserted into identifiers)
  std::string addSuffix(
    const std::string& prefix, std::set<std::string>& identifiers) const;

  /// Splits string \p s into tokens using pattern \p p
  std::vector<std::string> tokenize(std::string s, std::string p) const;

  /// if \p S is declaration statement we shouldn't place braces if
  /// declarations were referenced outside it
  bool requiresBraces(clang::FunctionDecl* FD, clang::Stmt* S);

  /// Local matcher to find correct node in AST during construct()
  class : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& MatchResult) {
      const clang::VarDecl* VD
        = MatchResult.Nodes.getNodeAs<clang::VarDecl>("varDecl");
      if (VD->getName() == identifier
        && processor(VD->getType().getAsString()) == type) {
        ++count;
      }
      return;
    }
    void setParameters(const std::string& type, const std::string& identifier,
        const std::function<std::string(const std::string&)>& processor) {
      this->type = type;
      this->identifier = identifier;
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
    std::string identifier;
    std::function<std::string(const std::string&)> processor; 
    int count;
  } varDeclHandler;
  
  // [C99 6.7.2, 6.7.3]
  const std::vector<std::string> mKeywords = { "register",
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Bool", "_Complex", "struct", "union", "enum",
    "const", "restrict", "volatile" };
  const std::string mIdentifierPattern = "[[:alpha:]_]\\w*";
  
  tsar::TransformationContext* mTransformContext;
  tsar::QueryManager* mQueryManager;

  clang::CompilerInstance& mCompiler;
  clang::ASTContext& mContext;
  clang::SourceManager& mSourceManager;
  clang::Rewriter* mRewriter;

  std::unique_ptr<llvm::LLVMContext> mLLVMContext;
  std::unique_ptr<clang::CodeGenerator> mGen;

  /// last seen function decl (with body we are currently in)
  clang::FunctionDecl* mCurrentFD;

  /// for statements - for detecting call expressions which can be inlined
  std::vector<clang::Stmt*> mFSs;

  std::map<clang::FunctionDecl*, std::set<clang::Type*>> mTypeDecls;
  std::map<clang::FunctionDecl*, std::set<clang::Type*>> mTypeRefs;

  std::map<clang::FunctionDecl*, std::set<clang::DeclRefExpr*>> mDeclRefs;
  
  std::map<clang::FunctionDecl*, detail::Template> mTs;
  std::map<clang::FunctionDecl*, std::vector<detail::TemplateInstantiation>> mTIs;
};

class FInlinerAction : public tsar::ActionBase {
public:
  FInlinerAction(std::vector<std::string> CL, tsar::QueryManager* QM);

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance& CI, llvm::StringRef InFile);

  static std::string createProjectFile(const std::vector<std::string>& sources);

  /// reformats content of file \p FID with LLVM style
  bool format(clang::Rewriter& Rewriter, clang::FileID FID) const;

  /// overwrites changed files and reformats them for readability
  void EndSourceFileAction();

private:
  static std::vector<std::string> mSources;

  clang::CompilerInstance* mCompilerInstance;
  std::unique_ptr<tsar::TransformationContext> mTfmCtx;
};

}

#endif

#endif
