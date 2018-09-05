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


#ifndef TSAR_FUNCTION_INLINER_H
#define TSAR_FUNCTION_INLINER_H

#include "AnalysisWrapperPass.h"
#include "tsar_pass.h"
#include "tsar_query.h"
#include "tsar_action.h"
#include "tsar_transformation.h"

#include <set>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/PointerIntPair.h>


namespace tsar {

class FunctionInlinerQueryManager : public QueryManager {
public:
  void run(llvm::Module *M, TransformationContext *Ctx) override;
};

struct FunctionInlineInfo : private bcl::Uncopyable {
  // place data for further passes
};
}

namespace llvm {
using FunctionInlinerImmutableWrapper
  = AnalysisWrapperPass<tsar::FunctionInlineInfo>;

class FunctionInlinerImmutableStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  FunctionInlinerImmutableStorage() : ImmutablePass(ID) {}

  const tsar::FunctionInlineInfo& getFunctionInlineInfo() const noexcept {
    return mFunctionInlineInfo;
  }

  tsar::FunctionInlineInfo& getFunctionInlineInfo() noexcept {
    return mFunctionInlineInfo;
  }

private:
  tsar::FunctionInlineInfo mFunctionInlineInfo;
};

class FunctionInlinerPass :
  public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  FunctionInlinerPass() : ModulePass(ID) {
    initializeFunctionInlinerPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module& M) override;
  void getAnalysisUsage(AnalysisUsage& AU) const override;
};
}

namespace detail {

/// Contains general information which describes a function.
class Template {
public:

  /// Attention, do not use nullptr to initialize template. We use this default
  /// parameter value for convenient access to the template using
  /// std::map::operator[]. Template must already exist in the map.
  explicit Template(const clang::FunctionDecl *FD = nullptr) :
    mFunc{ { FD, false }, false } {
    assert(FD && "Function declaration must not be null!");
  }

  const clang::FunctionDecl *getFuncDecl() const {
    return mFunc.getPointer().getPointer();
  }

  void setNeedToInline(bool IsNeed) { mFunc.setInt(IsNeed); }
  bool isNeedToInline() const { mFunc.getInt(); }

  bool isSingleReturn() const { return mFunc.getPointer().getInt(); }
  void setSingleReturn(bool IsSingle) { mFunc.getPointer().setInt(IsSingle); }

  void addParmRef(const clang::ParmVarDecl* PVD,
      const clang::DeclRefExpr* DRE) {
    mParmRefs[PVD].push_back(DRE);
  }

  std::vector<const clang::DeclRefExpr*> getParmRefs(
      const clang::ParmVarDecl* PVD) const {
    auto isPVDRef = [PVD](const std::pair<const clang::ParmVarDecl*,
      std::vector<const clang::DeclRefExpr*>>&lhs) -> bool {
      return lhs.first == PVD;
    };
    if (std::find_if(std::begin(mParmRefs), std::end(mParmRefs), isPVDRef)
      == std::end(mParmRefs)) {
      return std::vector<const clang::DeclRefExpr*>();
    } else {
      return mParmRefs.at(PVD);
    }
  }

  void addRetStmt(const clang::ReturnStmt* RS) { mRSs.insert(RS); }
  std::set<const clang::ReturnStmt*> getRetStmts() const { return mRSs; }




  bool isMacroInDecl() const { return mMacroInDecl.isValid(); }
  clang::SourceLocation getMacroInDecl() const { return mMacroInDecl; }
  clang::SourceLocation getMacroSpellingHint() const {
    return mMacroSpellingHint;
  }
  void setMacroInDecl(clang::SourceLocation Loc,
      clang::SourceLocation SpellingHint = clang::SourceLocation()) {
    mMacroInDecl = Loc;
    mMacroSpellingHint = SpellingHint;
  }

private:
  std::map<const clang::ParmVarDecl*, std::vector<const clang::DeclRefExpr*>>
    mParmRefs;
  std::set<const clang::ReturnStmt*> mRSs;

  /// This is { { FunctionDecl, IsSingleReturn }, IsNeedToInline }.
  llvm::PointerIntPair<
    llvm::PointerIntPair<const clang::FunctionDecl *, 1, bool>, 1, bool> mFunc;

  /// One of statements or declarations inside function definition
  /// which is located in macro.
  clang::SourceLocation mMacroInDecl;

  /// If macro was found after manual raw relexing of sources and it does not
  /// mentioned in AST, this location points to it definition.
  clang::SourceLocation mMacroSpellingHint;
};

/// Represents one specific place in user source code where one of specified
/// functions (for inlining) is called
struct TemplateInstantiation {
  const clang::FunctionDecl* mFuncDecl;
  const clang::Stmt* mStmt;
  const clang::CallExpr* mCallExpr;
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
namespace detail {
/// Represents a scope which is defined by a statement or a clause.
class ScopeInfo {
public:
  ScopeInfo(clang::Stmt *S = nullptr, bool IsClause = false,
    bool IsUsed = true) : mInfo{ {S, IsClause}, IsUsed } {}

  operator clang::Stmt * () { return mInfo.getPointer().getPointer(); }
  operator const clang::Stmt * () const {
    return mInfo.getPointer().getPointer();
  }

  clang::Stmt * getStmt() { return *this; }
  const clang::Stmt * getStmt() const { return *this; }

  clang::Stmt & operator*() { return *getStmt(); }
  const clang::Stmt & operator*() const { return *getStmt(); }
  clang::Stmt * operator->() { return getStmt(); }
  const clang::Stmt * operator->() const { return getStmt(); }

  bool isClause() const { return mInfo.getPointer().getInt(); }
  void setIsClause(bool IsClause = true) { mInfo.getPointer().setInt(IsClause); }

  bool isUsed() const { return mInfo.getInt(); }
  void setIsUsed(bool IsUsed = true) { mInfo.setInt(IsUsed); }

  void reset() { *this = ScopeInfo(); }

private:
  llvm::PointerIntPair<
    llvm::PointerIntPair<clang::Stmt *, 1, bool>, 1, bool> mInfo;
};
}
}

namespace llvm {
template<typename From> struct simplify_type;

// Specialize simplify_type to allow WeakVH to participate in
// dyn_cast, isa, etc.
template<> struct simplify_type<tsar::detail::ScopeInfo> {
  using SimpleType = clang::Stmt *;
  static SimpleType getSimplifiedValue(tsar::detail::ScopeInfo &Info) {
    return Info;
  }
};
template<> struct simplify_type<const tsar::detail::ScopeInfo> {
  using SimpleType = const clang::Stmt *;
  static SimpleType getSimplifiedValue(const tsar::detail::ScopeInfo &Info) {
    return Info;
  }
};
}

namespace tsar {
/// This class provides both AST traversing and source code buffer modification
/// (through Rewriter API). Note that the only result of its work - modified
/// Rewriter (buffer) object inside passed Transformation Context.
class FInliner :
    public clang::RecursiveASTVisitor<FInliner>,
    public clang::ASTConsumer {
  /// This represents clause attached to a statement.
  struct ClauseToStmt {
    ClauseToStmt(const clang::Stmt *C, const clang::Stmt *S) :
      Clause(C), Stmt(S) {}
    const clang::Stmt *Clause;
    const clang::Stmt *Stmt;
  };

  /// This is a list of statements (for each function) which is marked
  /// with `inline` clause.
  using InlineQuery =
    llvm::DenseMap<const clang::FunctionDecl*, std::vector<ClauseToStmt>>;

  /// Prototype of a function which check whether a function can be inlined.
  using TemplateChecker = std::function<bool(const ::detail::Template &)>;

public:
  explicit FInliner(tsar::TransformationContext* TfmCtx)
    : mTransformContext(TfmCtx), mContext(TfmCtx->getContext()),
    mRewriter(TfmCtx->getRewriter()),
    mSourceManager(TfmCtx->getRewriter().getSourceMgr()) {}

  bool VisitReturnStmt(clang::ReturnStmt* RS);
  bool VisitExpr(clang::Expr* E);
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE);
  bool VisitTypeLoc(clang::TypeLoc TL);
  bool VisitDecl(clang::Decl *D);

  bool TraverseFunctionDecl(clang::FunctionDecl *FD);

  bool TraverseStmt(clang::Stmt *S);

  bool TraverseCallExpr(clang::CallExpr *Call);

  /// Traverses AST, collects necessary information using overriden methods above
  /// and applies it to source code using private methods below
  void HandleTranslationUnit(clang::ASTContext& Context);

private:
  /// Collects information of a macro in current location.
  void rememberMacroLoc(clang::SourceLocation Loc);

  /// Finds functions which should be inlined and which produces recursion.
  /// Note, that functions which are not marked for inlining will be ignored
  /// in this search.
  llvm::DenseSet<const clang::FunctionDecl *> findRecursion() const;

  /// Determines templates which can be inlined, print diagnostics, sets
  /// isNeedToInline() to `false` if a function can not be inlined.
  void checkTemplates(const llvm::SmallVectorImpl<TemplateChecker> &Checkers);

  llvm::SmallVector<TemplateChecker, 8> getTemplateCheckers() const;

  /// Constructs correct language declaration of \p Identifier with \p Type
  /// Uses bruteforce with linear complexity dependent on number of tokens
  /// in \p Type where token is non-whitespace character or special sequence.
  /// \p Context is a string containing declarations used in case of referencing
  /// in \p Type.
  /// \returns vector of tokens which can be transformed to text string for
  /// insertion into source code
  /// SLOW!
  std::vector<std::string> construct(
    const std::string& Type, const std::string& Identifier,
    const std::string& Context,
    std::map<std::string, std::string>& Replacements);

  /// Does instantiation of \p TI using \p Args generating non-collidable
  /// identifiers/labels if necessary. Since instantiation is recursive,
  /// collects all visible and newly created named declarations in \p Decls
  /// to avoid later possible collisions.
  /// \returns text of instantiated function body and result identifier
  std::pair<std::string, std::string> compile(
    const ::detail::TemplateInstantiation& TI,
    const std::vector<std::string>& Args,
    std::set<std::string>& Decls);

  std::string getSourceText(const clang::SourceRange& SR) const;

  /// get all identifiers which have declarations (names only)
  /// traverses tag declarations
  std::set<std::string> getIdentifiers(const clang::Decl* D) const;
  std::set<std::string> getIdentifiers(const clang::TagDecl* TD) const;

  /// Returns source range for a specified node.
  /// Note, T must provide getSourceRange() method
  template<class T> clang::SourceRange getRange(T *Node) const;

  clang::SourceLocation getLoc(clang::SourceLocation SL) const;

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
  bool requiresBraces(const clang::FunctionDecl* FD, const clang::Stmt* S);

  /// Local matcher to find correct node in AST during construct()
  class : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& MatchResult) {
      const clang::VarDecl* VD
        = MatchResult.Nodes.getNodeAs<clang::VarDecl>("varDecl");
      if (!VD) {
        return;
      }
      if (VD->getName() == mIdentifier
        && mProcessor(VD->getType().getAsString()) == mType) {
        ++mCount;
      }
      return;
    }
    void setParameters(const std::string& Type, const std::string& Identifier,
      const std::function<std::string(const std::string&)>& Processor) {
      mType = Type;
      mIdentifier = Identifier;
      mProcessor = Processor;
      return;
    }
    int getCount(void) const {
      return mCount;
    }
    void initCount(void) {
      mCount = 0;
      return;
    }
  private:
    std::string mType;
    std::string mIdentifier;
    std::function<std::string(const std::string&)> mProcessor;
    int mCount;
  } VarDeclHandler;

  // [C99 6.7.2, 6.7.3]
  const std::vector<std::string> mKeywords = { "register",
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Bool", "_Complex", "struct", "union", "enum",
    "typedef", "const", "restrict", "volatile" };
  const std::string mIdentifierPattern = "[[:alpha:]_]\\w*";

  tsar::TransformationContext* mTransformContext;

  InlineQuery mInlineStmts;

  /// This is a stack of scopes with a function definition at the bottom.
  /// Note, that pragma is also considered as a scope.
  std::vector<detail::ScopeInfo> mScopes;

  /// The last visited clause.
  detail::ScopeInfo mActiveClause;

  /// Root of subtree which contains currently visited statement
  /// (or declaration) and which is located in macro.
  clang::SourceLocation mStmtInMacro;

  clang::ASTContext& mContext;
  clang::SourceManager& mSourceManager;
  clang::Rewriter& mRewriter;

  /// last seen function decl (with body we are currently in)
  clang::FunctionDecl* mCurrentFD = nullptr;

  /// for statements - for detecting call expressions which can be inlined
  std::vector<const clang::Stmt*> mFSs;

  /// globally declared names
  std::set<std::string> mGlobalIdentifiers;

  /// local/global referenced names per function
  std::map<const clang::FunctionDecl*, std::set<std::string>>
    mExtIdentifiers, mIntIdentifiers;


  /// external declarations per function
  std::map<const clang::FunctionDecl*, std::set<const clang::Decl*>>
    mForwardDecls;

  /// unreachable statements per function
  /// (currently only returns are later analyzed)
  std::map<const clang::FunctionDecl*, std::set<const clang::Stmt*>>
    mUnreachableStmts;

  /// all expressions found in specific functions
  std::map<const clang::FunctionDecl*, std::set<const clang::Expr*>>
    mExprs;

  /// template mapping
  std::map<const clang::FunctionDecl*, ::detail::Template> mTs;

  /// template instantiations' mapping
  std::map<const clang::FunctionDecl*,
    std::vector<::detail::TemplateInstantiation>> mTIs;
};

}

#endif//TSAR_FUNCTION_INLINER_H
