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
#include "GlobalInfoExtractor.h"
#include "NamedDeclMapInfo.h"
#include "tsar_transformation.h"

#include <set>
#include <llvm/ADT/BitmaskEnum.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Module.h>

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
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Contains general information which describes a function.
class Template {
  enum Flags : uint8_t {
    DefaultFlags = 0,
    IsNeedToInline = 1u << 0,
    IsKnownMayForwardDecls = 1u << 1,
    LLVM_MARK_AS_BITMASK_ENUM(IsKnownMayForwardDecls)
  };
public:
  /// Set of declarations, name is used to build hash.
  using DeclSet = llvm::DenseSet<
    const tsar::GlobalInfoExtractor::OutermostDecl *,
    tsar::GlobalInfoExtractor::OutermostDeclNameMapInfo>;

  /// List of reference to a declaration.
  using DeclRefList = std::vector<const clang::DeclRefExpr *>;

  /// Set of statements.
  using StmtSet = llvm::DenseSet<const clang::Stmt *>;

  /// Attention, do not use nullptr to initialize template. We use this default
  /// parameter value for convenient access to the template using
  /// std::map::operator[]. Template must already exist in the map.
  explicit Template(const clang::FunctionDecl *FD = nullptr) : mFuncDecl(FD) {
    assert(FD && "Function declaration must not be null!");
  }

  const clang::FunctionDecl *getFuncDecl() const { return mFuncDecl; }

  bool isNeedToInline() const { return mFlags & IsNeedToInline; }
  void setNeedToInline() { mFlags |= IsNeedToInline; }
  void disableInline() { mFlags &= ~IsNeedToInline; }

  bool isKnownMayForwardDecls() const { return mFlags & IsKnownMayForwardDecls;}
  void setKnownMayForwardDecls() { mFlags |= IsKnownMayForwardDecls; }

  void addParmRef(const clang::ParmVarDecl* PVD,
      const clang::DeclRefExpr* DRE) {
    mParmRefs[PVD].push_back(DRE);
  }
  const DeclRefList & getParmRefs(const clang::ParmVarDecl* PVD) const {
    return mParmRefs[PVD];
  }

  void addRetStmt(const clang::ReturnStmt* RS) { mRSs.insert(RS); }
  std::set<const clang::ReturnStmt*> getRetStmts() const { return mRSs; }

  void setLastStmt(const clang::Stmt *S) noexcept { mLastStmt = S; }
  const clang::Stmt * getLastStmt() const noexcept { return mLastStmt; }

  void addForwardDecl(const tsar::GlobalInfoExtractor::OutermostDecl *D) {
    mForwardDecls.insert(D);
  }
  const DeclSet & getForwardDecls() const noexcept { return mForwardDecls; }

  void addUnreachableStmt(const clang::Stmt *S) { mUnreachable.insert(S); }
  const StmtSet & getUnreachableStmts() const noexcept { return mUnreachable; }

  void addMayForwardDecl(const tsar::GlobalInfoExtractor::OutermostDecl *D) {
    mMayForwardDecls.insert(D);
  }
  const DeclSet & getMayForwardDecls() const noexcept {
    assert(isKnownMayForwardDecls() && "May forward declarations is unknown!");
    return mMayForwardDecls;
  }

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
  mutable llvm::DenseMap<const clang::ParmVarDecl*, DeclRefList> mParmRefs;
  std::set<const clang::ReturnStmt*> mRSs;
  const clang::FunctionDecl *mFuncDecl = nullptr;
  Flags mFlags = DefaultFlags;

  /// The last statement at the top level of a function body.
  const clang::Stmt *mLastStmt = nullptr;

  /// One of statements or declarations inside function definition
  /// which is located in macro.
  clang::SourceLocation mMacroInDecl;

  /// If macro was found after manual raw relexing of sources and it does not
  /// mentioned in AST, this location points to it definition.
  clang::SourceLocation mMacroSpellingHint;

  DeclSet mForwardDecls;
  DeclSet mMayForwardDecls;
  StmtSet mUnreachable;
};

/// Represents one specific place in user source code where one of specified
/// functions (for inlining) is called
struct TemplateInstantiation {
  TemplateInstantiation() = delete;
  enum Flags : uint8_t {
    DefaultFlags = 0,
    IsNeedBraces = 1u << 0,
    LLVM_MARK_AS_BITMASK_ENUM(IsNeedBraces)
  };
  const clang::FunctionDecl* mFuncDecl;
  const clang::Stmt* mStmt;
  const clang::CallExpr* mCallExpr;
  const Template* mTemplate;
  Flags mFlags;
};

inline bool operator==(const TemplateInstantiation &LHS,
    const TemplateInstantiation &RHS) noexcept {
  return LHS.mFuncDecl == RHS.mFuncDecl
    && LHS.mStmt == RHS.mStmt
    && LHS.mCallExpr == RHS.mCallExpr
    && LHS.mTemplate == RHS.mTemplate;
}
}
namespace llvm {
template<> struct DenseMapInfo<::detail::TemplateInstantiation> {
  static inline ::detail::TemplateInstantiation getEmptyKey() {
    return ::detail::TemplateInstantiation{
      nullptr, nullptr,
      DenseMapInfo<clang::CallExpr *>::getEmptyKey(),
      nullptr, ::detail::TemplateInstantiation::DefaultFlags
    };
  }
  static inline ::detail::TemplateInstantiation getTombstoneKey() {
    return ::detail::TemplateInstantiation{
      nullptr, nullptr,
      DenseMapInfo<clang::CallExpr *>::getTombstoneKey(),
      nullptr, ::detail::TemplateInstantiation::DefaultFlags
    };
  }
  static inline unsigned getHashValue(
      const ::detail::TemplateInstantiation &TI) {
    return DenseMapInfo<clang::CallExpr *>::getHashValue(TI.mCallExpr);
  }
  static inline unsigned getHashValue(const clang::CallExpr *Call) {
    return DenseMapInfo<clang::CallExpr *>::getHashValue(Call);
  }
  static inline bool isEqual(const ::detail::TemplateInstantiation &LHS,
      const ::detail::TemplateInstantiation &RHS) noexcept {
    return LHS == RHS;
  }
  static inline bool isEqual(const clang::CallExpr *Call,
      const ::detail::TemplateInstantiation &RHS) noexcept {
    return RHS.mCallExpr == Call;
  }
};
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

  /// Prototype of a function which checks whether a function can be inlined.
  using TemplateChecker = std::function<bool(const ::detail::Template &)>;

  /// Chain of calls that should be inlined.
  using InlineStackImpl =
    llvm::SmallVectorImpl<const ::detail::TemplateInstantiation *>;

  /// \brief Prototype of a function which checks whether a specified function
  /// call can be inlined to a function at the bottom of a specified stack.
  ///
  /// Note, that to represent a function at the bottom of stack bogus template
  /// instantiation may be used. It contains only mTemplate field, other fields
  /// may be null.
  using TemplateInstantiationChecker =
    std::function<bool(const ::detail::TemplateInstantiation &,
      const InlineStackImpl &)>;
public:
  /// Map from function declaration to its template.
  using TemplateMap = std::map<const clang::FunctionDecl*, ::detail::Template>;

  /// Map from function declarations to the list of calls from its body.
  using TemplateInstantiationMap = std::map<const clang::FunctionDecl*,
    llvm::DenseSet<::detail::TemplateInstantiation>>;

  explicit FInliner(tsar::TransformationContext* TfmCtx)
    : mTransformContext(TfmCtx), mContext(TfmCtx->getContext()),
    mRewriter(TfmCtx->getRewriter()),
    mSourceManager(TfmCtx->getRewriter().getSourceMgr()),
    mGIE(TfmCtx->getContext().getSourceManager(),
      TfmCtx->getContext().getLangOpts()){}

  bool VisitReturnStmt(clang::ReturnStmt* RS);
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE);
  bool VisitTypeLoc(clang::TypeLoc TL);
  bool VisitTagTypeLoc(clang::TagTypeLoc TTL);
  bool VisitTypedefTypeLoc(clang::TypedefTypeLoc TTL);
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

  /// Returns list of checkers.
  llvm::SmallVector<TemplateChecker, 8> getTemplateCheckers() const;

  /// Check is it possible to inline a specified call at the top of a specified
  /// call stack, return true on success, print diagnostics.
  bool checkTemplateInstantiation(::detail::TemplateInstantiation &TI,
    const InlineStackImpl &CallStack,
    const llvm::SmallVectorImpl<TemplateInstantiationChecker> &Checkers);

  /// Returns list of checkers.
  llvm::SmallVector<TemplateInstantiationChecker, 8>
    getTemplatInstantiationCheckers() const;

  /// \brief Does instantiation of TI.
  ///
  /// \param [in] TI Description of a call which should be inlined.
  /// \param [in] Args List of actual parameters.
  /// \param [in] TICheckers List of call checkers.
  /// \param [in, out] CallStack Stack of calls with a root (may be bogus)
  /// of call graph subtree which should be inlined. This function may change
  /// `CallStack` internally, however, after the call it will be in the
  /// initial state (before call).
  /// \post This method generates generating non-collidable identifiers/labels
  /// if necessary and update mIdentifiers set.
  /// \return Text of instantiated function body and result identifier.
  std::pair<std::string, std::string> compile(
    const ::detail::TemplateInstantiation &TI,
    llvm::ArrayRef<std::string> Args,
    const llvm::SmallVectorImpl<TemplateInstantiationChecker> &TICheckers,
    InlineStackImpl &CallStack);

  /// Returns source text at a specified range.
  llvm::StringRef getSourceText(const clang::SourceRange& SR) const;

  /// Returns source range for a specified node, `T` must provide
  /// `getSourceRange()` method.
  template<class T> clang::SourceRange getFileRange(T *Node) const;

  /// Appends numeric suffix to the end of a specified identifier `Prefix`,
  /// avoids collision using set of identifiers available in a translation unit.
  ///
  /// \param [out] Out New identifier which is already inserted into
  /// mIdentifiers.
  void addSuffix(llvm::StringRef Prefix, llvm::SmallVectorImpl<char> &Out);

  tsar::TransformationContext* mTransformContext;

  /// Visitor to collect global information about a translation unit.
  GlobalInfoExtractor mGIE;

  InlineQuery mInlineStmts;

  /// This is a stack of scopes with a function definition at the bottom.
  /// Note, that pragma is also considered as a scope.
  std::vector<detail::ScopeInfo> mScopes;

  /// The last visited clause.
  detail::ScopeInfo mActiveClause;

  /// Root of subtree which contains currently visited statement
  /// (or declaration) and which is located in macro.
  clang::SourceLocation mStmtInMacro;

  /// Set of raw locations which contains reference to some declarations which
  /// are used in the last traversed function.
  GlobalInfoExtractor::RawLocationSet mDeclRefLoc;

  clang::ASTContext& mContext;
  clang::SourceManager& mSourceManager;
  clang::Rewriter& mRewriter;

  /// last seen function decl (with body we are currently in)
  clang::FunctionDecl* mCurrentFD = nullptr;

  /// \brief All identifiers (global and local) mentioned in a translation unit.
  ///
  /// These identifiers is used to prevent conflicts when new identifiers
  /// are added to the source code. It is convenient to avoid intersection with
  /// all available identifiers (including the local ones). For example,
  /// if chain of calls should be inlnined in a function, it is necessary to
  /// check that all new identifiers do not hide forward declarations of all
  /// functions in this chain.
  llvm::StringSet<> mIdentifiers;

  TemplateMap mTs;
  TemplateInstantiationMap mTIs;
};

}

#endif//TSAR_FUNCTION_INLINER_H
