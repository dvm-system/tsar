//===--- Inline.h ------- Source-level Inliner (Clang) ----------*- C++ -*-===//
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
// This file declares classes and methods necessary for function source-level
// inlining.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_INLINER_H
#define TSAR_CLANG_INLINER_H

#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeLoc.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Pass.h>
#include <map>
#include <memory>
#include <vector>

namespace clang {
class Rewriter;
}

namespace llvm {
class Module;

/// Performs source-level inline expansion.
class ClangInlinerPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  ClangInlinerPass() : ModulePass(ID) {
    initializeClangInlinerPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage& AU) const override;
};
}

namespace tsar {
namespace detail {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class Template;

/// Represents one specific place in user source code where one of specified
/// functions (for inlining) is called.
struct TemplateInstantiation {
  TemplateInstantiation() = delete;
  enum Flags : uint8_t {
    DefaultFlags = 0,
    IsNeedBraces = 1u << 0,
    LLVM_MARK_AS_BITMASK_ENUM(IsNeedBraces)
  };
  const Template *mCaller;
  const clang::Stmt* mStmt;
  const clang::CallExpr* mCallExpr;
  const Template* mCallee;
  Flags mFlags;
};

inline bool operator==(const TemplateInstantiation &LHS,
    const TemplateInstantiation &RHS) noexcept {
  return LHS.mCaller == RHS.mCaller
    && LHS.mStmt == RHS.mStmt
    && LHS.mCallExpr == RHS.mCallExpr
    && LHS.mCallee == RHS.mCallee;
}
}
}

namespace tsar {
namespace detail {
/// Contains general information which describes a function.
class Template {
  enum Flags : uint8_t {
    DefaultFlags = 0,
    IsNeedToInline = 1u << 0,
    IsKnownMayForwardDecls = 1u << 1,
    LLVM_MARK_AS_BITMASK_ENUM(IsKnownMayForwardDecls)
  };

  /// Set of calls from the current function. A value is an index in the
  /// list of template instantiations.
  using CallMap = llvm::DenseMap<const clang::CallExpr *, std::size_t>;

public:
  /// Set of declarations, name is used to build hash.
  using DeclSet = llvm::DenseSet<
    const tsar::GlobalInfoExtractor::OutermostDecl *,
    tsar::GlobalInfoExtractor::OutermostDeclNameMapInfo>;

  /// List of reference to a declaration.
  using DeclRefList = std::vector<const clang::DeclRefExpr *>;

  /// Set of statements.
  using StmtSet = llvm::DenseSet<const clang::Stmt *>;

  /// List of calls from the current function.
  using CallList = std::vector<TemplateInstantiation>;

  /// List of source ranges.
  using RangeList = llvm::SmallVector<clang::CharSourceRange, 8>;

  /// \brief List of return statements.
  ///
  /// The value is true if to replace the statement with multiple statements
  /// it is necessary to add braces `{` ... `}`.
  using ReturnStmts = llvm::DenseMap<const clang::ReturnStmt *, bool>;

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

  void addRetStmt(const clang::ReturnStmt* RS, bool IsNeedBraces = true) {
    mRSs.try_emplace(RS, IsNeedBraces).first->second |= IsNeedBraces;
  }
  const ReturnStmts & getRetStmts() const { return mRSs; }

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

  std::pair<CallList::iterator, bool> addCall(TemplateInstantiation &&TI) {
    assert(TI.mCallExpr && "Call must not be null!");
    auto CallInfo = mCallIdxs.try_emplace(TI.mCallExpr, mCalls.size());
    if (CallInfo.second) {
      mCalls.push_back(std::move(TI));
      return std::make_pair(mCalls.end() - 1, true);
    }
    return std::make_pair(mCalls.begin() + CallInfo.first->second, false);
  }
  CallList::iterator findCall(const clang::CallExpr *Call) {
    assert(Call && "Call must not be null!");
    auto I= mCallIdxs.find(Call);
    return I == mCallIdxs.end() ? mCalls.end() : mCalls.begin() + I->second;
  }
  const CallList & getCalls() const noexcept { return mCalls; }

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

  /// \brief Returns source ranges which should be removed from a source code.
  ///
  /// For example, it may be a the whole #pragma or clause inside a pragma.
  RangeList & getToRemove() noexcept { return mToRemove; }
  const RangeList & getToRemove() const noexcept { return mToRemove; }

private:
  const clang::FunctionDecl *mFuncDecl = nullptr;
  Flags mFlags = DefaultFlags;
  mutable llvm::DenseMap<const clang::ParmVarDecl*, DeclRefList> mParmRefs;
  ReturnStmts mRSs;

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
  CallMap mCallIdxs;
  CallList mCalls;
  RangeList mToRemove;
};

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
  void setIsClause(bool IsClause = true) {
    auto Val = mInfo.getPointer();
    Val.setInt(IsClause);
    mInfo.setPointer(Val);
  }

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

// Specialize simplify_type to allow detail::ScopeInfo to participate in
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
struct ASTImportInfo;

/// Performs inline expansion, processes calls which is marked with `inline`
/// clause, print warnings on errors. Do not write changes to file, change
/// memory buffer only (a specified clang::Rewriter is used).
class ClangInliner : public clang::RecursiveASTVisitor<ClangInliner> {
  /// Prototype of a function which checks whether a function can be inlined.
  using TemplateChecker = std::function<bool(const detail::Template &)>;

  /// Chain of calls that should be inlined.
  using InlineStackImpl =
    llvm::SmallVectorImpl<const detail::TemplateInstantiation *>;

  /// \brief Prototype of a function which checks whether a specified function
  /// call can be inlined to a function at the bottom of a specified stack.
  ///
  /// Note, that to represent a function at the bottom of stack bogus template
  /// instantiation may be used. It contains only mCallee field, other fields
  /// may be null.
  using TemplateInstantiationChecker =
    std::function<bool(const detail::TemplateInstantiation &,
      const InlineStackImpl &)>;
public:
  /// Compares locations of two function declarations.
  struct LocationLess {
    bool operator()(const clang::FunctionDecl *LHS,
        const clang::FunctionDecl *RHS) const {
      return LHS->getLocation() < RHS->getLocation();
    }
  };

  /// Map from function declaration to its template.
  using TemplateMap = std::map<
    const clang::FunctionDecl*, std::unique_ptr<detail::Template>, LocationLess>;

  explicit ClangInliner(clang::Rewriter &Rewriter, clang::ASTContext &Context,
      const GlobalInfoExtractor &GIE,
      ClangGlobalInfo::RawInfo &RawInfo,
      const ASTImportInfo &ImportInfo) :
    mRewriter(Rewriter), mContext(Context),
    mSrcMgr(Context.getSourceManager()), mLangOpts(Context.getLangOpts()),
    mGIE(GIE), mRawInfo(RawInfo), mImportInfo(ImportInfo) {}

  clang::Rewriter & getRewriter() noexcept { return mRewriter; }
  clang::ASTContext & getContext() noexcept { return mContext; }

  /// Performs inline expansion. Note, this function updates list of raw
  /// identifiers (in RawInfo) if it is necessary to create a new one.
  void HandleTranslationUnit();

  bool VisitReturnStmt(clang::ReturnStmt* RS);
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE);
  bool VisitTypeLoc(clang::TypeLoc TL);
  bool VisitTagType(clang::TagType *TT);
  bool VisitTagTypeLoc(clang::TagTypeLoc TTL);
  bool VisitTypedefType(clang::TypedefType *TT);
  bool VisitTypedefTypeLoc(clang::TypedefTypeLoc TTL);
  bool VisitDecl(clang::Decl *D);

  bool TraverseFunctionDecl(clang::FunctionDecl *FD);
  bool TraverseStmt(clang::Stmt *S);
  bool TraverseCallExpr(clang::CallExpr *Call);

private:
  /// General method which visits clang::NamedDecl available from clang::Type
  /// and clang::DeclRefExpr.
  void visitNamedDecl(clang::NamedDecl *ND);

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
  bool checkTemplateInstantiation(const detail::TemplateInstantiation &TI,
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
    const detail::TemplateInstantiation &TI,
    llvm::ArrayRef<std::string> Args,
    const llvm::SmallVectorImpl<TemplateInstantiationChecker> &TICheckers,
    InlineStackImpl &CallStack);

  /// Returns source text at a specified range.
  llvm::StringRef getSourceText(const clang::SourceRange& SR) const;

  /// Returns source range for a specified node, `T` must provide
  /// `getSourceRange()` method.
  template<class T> clang::SourceRange getTfmRange(T *Node) const;

  /// Appends numeric suffix to the end of a specified identifier `Prefix`,
  /// avoids collision using set of identifiers available in a translation unit.
  ///
  /// \param [out] Out New identifier which is already inserted into
  /// mIdentifiers.
  void addSuffix(llvm::StringRef Prefix, llvm::SmallVectorImpl<char> &Out);

  clang::Rewriter &mRewriter;
  clang::ASTContext &mContext;
  const clang::SourceManager &mSrcMgr;
  const clang::LangOptions &mLangOpts;

  /// Visitor to collect global information about a translation unit.
  const GlobalInfoExtractor &mGIE;

  /// Summary of the import process.
  const ASTImportInfo &mImportInfo;

  /// \brief Raw information about objects in a source code.
  ///
  /// The raw identifiers is used to prevent conflicts when new identifiers
  /// are added to the source code. It is convenient to avoid intersection with
  /// all available identifiers (including the local ones). For example,
  /// if chain of calls should be inlined in a function, it is necessary to
  /// check that all new identifiers do not hide forward declarations of all
  /// functions in this chain.
  ///
  /// We update list of raw identifiers if it is necessary to create a new one.
  tsar::ClangGlobalInfo::RawInfo &mRawInfo;

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

  /// Last seen function (with body we are currently in).
  detail::Template *mCurrentT = nullptr;

  TemplateMap mTs;
};
}
#endif//TSAR_CLANG_INLINER_H
