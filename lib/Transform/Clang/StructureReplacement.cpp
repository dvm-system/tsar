//=== StructureReplacement.cpp Source-level Replacement of Structures C++ *===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// The file declares a pass to perform replacement of fields of structures with
// separate variables.
//
// The replacement of function parameters are possible only.
// Type of a parameter to replace must be a pointer to some record type.
//===----------------------------------------------------------------------===//

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/IncludeTree.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace tsar;
using namespace clang;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-struct-replacement"

namespace {
class ClangStructureReplacementPass :
    public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangStructureReplacementPass() : ModulePass(ID) {
      initializeClangStructureReplacementPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TransformationEnginePass>();
    AU.addRequired<ClangGlobalInfoPass>();
    AU.addRequired<ClangIncludeTreePass>();
    AU.getPreservesAll();
  }

private:
  void addSuffix(const Twine &Prefix, SmallVectorImpl<char> &Out) {
    for (unsigned Count = 0;
      mRawInfo->Identifiers.count((Prefix + Twine(Count)).toStringRef(Out));
      ++Count, Out.clear());
    mRawInfo->Identifiers.insert(StringRef(Out.data(), Out.size()));
  }

  ClangGlobalInfoPass::RawInfo *mRawInfo = nullptr;
};

const clang::Type *getCanonicalUnqualifiedType(ValueDecl *VD) {
  return VD->getType()
      .getTypePtr()
      ->getCanonicalTypeUnqualified()
      ->getTypePtr();
}

struct Replacement {
  Replacement(ValueDecl *M) : Member(M) {}

  /// Member, this replacement belongs to, of a parameter which should be
  /// replaced.
  ValueDecl *Member;

  /// Locations in a source code which contains accesses to the member 'Member'
  /// of an original parameter.
  std::vector<SourceRange> Ranges;

  /// Identifier of a new parameter which corresponds to the member 'Member' of
  /// an original parameter which should be replaced.
  SmallString<32> Identifier;

  /// This is 'true' if a value of the member 'Member' of an original parameter
  /// can be changed in the original function call.
  bool InAssignment = false;
};

using ReplacementCandidates = SmallDenseMap<
    NamedDecl *, SmallVector<Replacement, 8>, 8, DenseMapInfo<NamedDecl *>,
    TaggedDenseMapPair<bcl::tagged<NamedDecl *, NamedDecl>,
                       bcl::tagged<SmallVector<Replacement, 8>, Replacement>>>;

/// Description of a possible replacement of a source function.
struct ReplacementMetadata {
  struct ParamReplacement {
    Optional<unsigned> TargetParam;
    FieldDecl *TargetMember = nullptr;
    bool IsPointer = false;
  };

  bool valid(unsigned *ParamIdx = nullptr) const {
    if (!TargetDecl) {
      if (ParamIdx)
        *ParamIdx = Parameters.size();
      return false;
    }
    for (unsigned I = 0, EI = Parameters.size(); I < EI; ++I)
      if (!Parameters[I].TargetParam) {
        if (ParamIdx)
          *ParamIdx = I;
        return false;
      }
    return true;
  }

  /// Declaration of a function which can be replaced with a current one.
  FunctionDecl *TargetDecl = nullptr;

  /// Correspondence between parameters of this function and the target
  /// 'TargetDecl' of a call replacement.
  SmallVector<ParamReplacement, 8> Parameters;
};

/// List of original functions for a clone.
using ReplacementTargets = SmallVector<ReplacementMetadata, 1>;

/// Map from calls that should be replaced to functions which should be used
/// instead of callee.
using ReplacementRequests =
    DenseMap<clang::CallExpr *,
             std::tuple<clang::FunctionDecl *, clang::SourceLocation>,
             DenseMapInfo<clang::CallExpr *>,
             TaggedDenseMapTuple<
                 bcl::tagged<clang::CallExpr *, clang::CallExpr>,
                 bcl::tagged<clang::FunctionDecl *, clang::FunctionDecl>,
                 bcl::tagged<clang::SourceLocation, clang::SourceLocation>>>;

struct FunctionInfo {
  /// List of parameters of this function, which are specified in 'replace'
  /// clause, which should be replaced.
  ReplacementCandidates Candidates;

  /// List of calls from this function, which are marked with a  'with' clause,
  /// which should be replaced.
  ReplacementRequests Requests;

  /// Calls to functions from this list can be replaced with this function.
  ReplacementTargets Targets;

  /// Source ranges which correspond to transformation clauses and which
  /// can be successfully removed.
  SmallVector<CharSourceRange, 8> ToRemoveTransform;

  /// Source ranges which corresponds to metadata clauses
  /// which can be successfully removed.
  SmallVector<CharSourceRange, 8> ToRemoveMetadata;

  SmallPtrSet<DeclRefExpr *, 8> Meta;
  bool Strict = false;

  /// Return true if there is no replacement-related information available.
  bool empty() const {
    return Candidates.empty() && Requests.empty() && Targets.empty();
  }

  /// Return true if at least one replacement candidate has been found.
  bool hasCandidates() const { return !Candidates.empty(); }

  /// Return true if at least on function call inside a current function should
  /// be replaced.
  bool hasRequests() const { return !Requests.empty(); }

  /// Return true if a specified reference is located in a 'replace' clause.
  bool inClause(const DeclRefExpr *DRE) const { return Meta.count(DRE); }
};

using ReplacementMap =
    DenseMap<FunctionDecl *, std::unique_ptr<FunctionInfo>,
             DenseMapInfo<FunctionDecl *>,
             TaggedDenseMapPair<
                 bcl::tagged<FunctionDecl *, FunctionDecl>,
                 bcl::tagged<std::unique_ptr<FunctionInfo>, FunctionInfo>>>;

/// This class collects all 'replace' clauses in the code.
class ReplacementCollector : public RecursiveASTVisitor<ReplacementCollector> {
public:

  ReplacementCollector(TransformationContext &TfmCtx,
      ReplacementMap &Replacements)
     : mTfmCtx(TfmCtx)
     , mSrcMgr(TfmCtx.getContext().getSourceManager())
     , mLangOpts(TfmCtx.getContext().getLangOpts())
     , mReplacements(Replacements)
  {}

  /// Return list of parameters to replace.
  ReplacementMap & getReplacementInfo() noexcept {
    return mReplacements;
  }

  /// Return list of parameters to replace.
  const ReplacementMap & getReplacementInfo() const noexcept {
    return mReplacements;
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    Pragma P(*S);
    SmallVector<Stmt *, 2> Clauses;
    if (findClause(P, ClauseId::Replace, Clauses)) {
      auto ReplaceSize = Clauses.size();
      findClause(P, ClauseId::With, Clauses);
      auto StashSize = Clauses.size();
      mCurrFunc->Strict |= !findClause(P, ClauseId::NoStrict, Clauses);
      // Do not remove 'nostrict' clause if the directive contains other
      // clauses except 'replace'.
      if (P.clause_size() > Clauses.size())
        Clauses.resize(StashSize);
      auto IsPossible = pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
        mCurrFunc->ToRemoveTransform, PragmaFlags::IsInHeader);
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
            diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
            diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
            diag::warn_remove_directive);
      mInClause = ClauseId::Replace;
      Clauses.resize(StashSize);
      auto I = Clauses.begin(), EI = Clauses.end();
      for (auto ReplaceEI = I + ReplaceSize; I < ReplaceEI; ++I) {
        mCurrClauseBeginLoc = (**I).getBeginLoc();
        if (!RecursiveASTVisitor::TraverseStmt(*I))
          break;
      }
      mInClause = ClauseId::With;
      for (; I < EI; ++I) {
        mCurrClauseBeginLoc = (**I).getBeginLoc();
        if (!RecursiveASTVisitor::TraverseStmt(*I))
          break;
      }
      mInClause = ClauseId::NotClause;
      return true;
    }
    if (findClause(P, ClauseId::ReplaceMetadata, Clauses)) {
      assert(mCurrFunc && "Replacement-related data must not be null!");
      pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
        mCurrFunc->ToRemoveMetadata, PragmaFlags::IsInHeader);
      mInClause = ClauseId::ReplaceMetadata;
      for (auto *C : Clauses) {
        mCurrClauseBeginLoc = C->getBeginLoc();
        for (auto *S : Pragma::clause(&C))
          if (!RecursiveASTVisitor::TraverseStmt(S))
            break;
          checkMetadataClauseEnd(mCurrMetaBeginLoc, C->getLocEnd());
      }
      mInClause = ClauseId::NotClause;
      return true;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool VisitStringLiteral(clang::StringLiteral *SL) {
    if (mInClause != ClauseId::ReplaceMetadata)
      return true;
    assert(!mCurrFunc->Targets.empty() &&
           "At least on target must be initialized!");
    auto &CurrMD = mCurrFunc->Targets.back();
    assert(CurrMD.TargetDecl && "Error in pragma, expected source function!");
    assert(mCurrMetaTargetParam < CurrMD.TargetDecl->getNumParams() &&
           "Parameter index is out of range!");
    assert(!SL->getString().empty() && "Member must be specified!");
    auto TargetParam = CurrMD.TargetDecl->getParamDecl(mCurrMetaTargetParam);
    auto Ty = getCanonicalUnqualifiedType(TargetParam);
    auto PtrTy = cast<clang::PointerType>(Ty);
    auto PointeeTy = PtrTy->getPointeeType().getTypePtr();
    auto StructTy = cast<clang::RecordType>(PointeeTy);
    auto StructDecl = StructTy->getDecl();
    auto MemberItr =
        find_if(StructDecl->fields(), [SL](const FieldDecl *FieldD) {
          return FieldD->getDeclName().isIdentifier() &&
                 FieldD->getName() == SL->getString();
        });
    if (MemberItr == StructDecl->field_end()) {
      toDiag(mSrcMgr.getDiagnostics(), SL->getBeginLoc(),
             diag::error_replace_md);
      toDiag(mSrcMgr.getDiagnostics(), StructDecl->getLocation(),
             diag::note_record_member_unknown)
          << SL->getString();
      return false;
    }
    mCurrMetaMember = *MemberItr;
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    if (mInClause == ClauseId::ReplaceMetadata)
      return VisitReplaceMetadataClauseExpr(Expr);
    if (mInClause == ClauseId::Replace)
      return VisitReplaceClauseExpr(Expr);
    if (mInClause == ClauseId::With)
      return VisitReplaceWithClauseExpr(Expr);
    return true;
  }

  bool VisitCallExpr(CallExpr *Expr) {
    if (mInClause == ClauseId::NotClause && mCurrWithTarget)
      mCurrFunc->Requests.try_emplace(Expr, mCurrWithTarget,
                                      mCurrClauseBeginLoc);
    mCurrWithTarget = nullptr;
    return true;
  }

  bool TraverseCompoundStmt(CompoundStmt *CS) {
    if (mInClause != ClauseId::ReplaceMetadata)
      return RecursiveASTVisitor::TraverseCompoundStmt(CS);
    assert(!mCurrFunc->Targets.empty() &&
           "At least on target must be initialized!");
    auto &CurrMD = mCurrFunc->Targets.back();
    if (mCurrMetaTargetParam >= CurrMD.TargetDecl->getNumParams()) {
      toDiag(mSrcMgr.getDiagnostics(), mCurrMetaBeginLoc,
        diag::error_function_args_number) << mCurrMetaTargetParam + 1;
      toDiag(mSrcMgr.getDiagnostics(), CurrMD.TargetDecl->getLocation(),
        diag::note_declared_at);
      return false;
    }
    auto Res = RecursiveASTVisitor::TraverseCompoundStmt(CS);
    ++mCurrMetaTargetParam;
    mCurrMetaMember = nullptr;
    return Res;
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    if (!FD->doesThisDeclarationHaveABody())
      return true;
    mCurrFunc = make_unique<FunctionInfo>();
    mCurrFD = FD;
    auto Res =
        RecursiveASTVisitor::TraverseFunctionDecl(FD->getCanonicalDecl());
    if (!mCurrFunc->empty())
      mReplacements.try_emplace(FD, std::move(mCurrFunc));
    return Res;
  }

private:
  bool VisitReplaceWithClauseExpr(DeclRefExpr *Expr) {
    mCurrFunc->Meta.insert(Expr);
    if (mCurrWithTarget) {
      SmallString<32> Out;
      toDiag(mSrcMgr.getDiagnostics(), mCurrClauseBeginLoc,
             diag::error_directive_clause_twice)
          << getPragmaText(ClauseId::Replace, Out).trim('\n')
          << getName(ClauseId::With);
      return false;
    }
    auto ND = Expr->getFoundDecl();
    if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
        mCurrWithTarget = FD->getCanonicalDecl();
        return true;
    }
    toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
      diag::error_clause_expect_function) << getName(ClauseId::With);
    toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(), diag::note_declared_at);
    return false;
  }

  bool VisitReplaceMetadataClauseExpr(DeclRefExpr *Expr) {
    assert(mCurrFD && "Current function must not be null!");
    assert(mCurrFunc && "Replacement description must not be null!");
    mCurrFunc->Meta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
      checkMetadataClauseEnd(mCurrMetaBeginLoc, Expr->getBeginLoc());
      mCurrFunc->Targets.emplace_back();
      auto &CurrMD = mCurrFunc->Targets.back();
      CurrMD.TargetDecl = FD->getCanonicalDecl();
      CurrMD.Parameters.resize(mCurrFD->getNumParams());
      mCurrMetaTargetParam = 0;
      mCurrMetaBeginLoc = Expr->getBeginLoc();
      return true;
    }
    assert(!mCurrFunc->Targets.empty() &&
           "Storage for metadata must be initialized!");
    auto &CurrMD = mCurrFunc->Targets.back();
    assert(mCurrMetaTargetParam < CurrMD.TargetDecl->getNumParams() &&
           "Parameter index is out of range!");
    if (auto PD = dyn_cast<ParmVarDecl>(ND)) {
      auto ParamTy = getCanonicalUnqualifiedType(PD);
      auto TargetParam =
          CurrMD.TargetDecl->getParamDecl(mCurrMetaTargetParam);
      auto TargetParamTy = mCurrMetaMember
                               ? getCanonicalUnqualifiedType(mCurrMetaMember)
                               : getCanonicalUnqualifiedType(TargetParam);
      bool IsPointer = false;
      if (ParamTy != TargetParamTy) {
        auto PtrTy = dyn_cast<clang::PointerType>(ParamTy);
        if (!PtrTy ||
            TargetParamTy !=
                PtrTy->getPointeeType().getCanonicalType().getTypePtr()) {
          toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
                 diag::error_replace_md_type_incompatible)
                  << (mCurrMetaMember ? 0 : 1);
          toDiag(mSrcMgr.getDiagnostics(),
                 mCurrMetaMember ? mCurrMetaMember->getLocation()
                                 : TargetParam->getLocation(),
                 diag::note_declared_at);
          toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
            diag::note_declared_at);
          return false;
        }
        IsPointer = true;
      }
      unsigned ParamIdx = 0;
      for (unsigned EI = mCurrFD->getNumParams(); ParamIdx < EI; ++ParamIdx)
        if (PD == mCurrFD->getParamDecl(ParamIdx))
          break;
      assert(ParamIdx < mCurrFD->getNumParams() && "Unknown parameter!");
      CurrMD.Parameters[ParamIdx].IsPointer = IsPointer;
      CurrMD.Parameters[ParamIdx].TargetMember = mCurrMetaMember;
      if (CurrMD.Parameters[ParamIdx].TargetParam) {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
          diag::error_replace_md_param_twice);
        return false;
      }
      CurrMD.Parameters[ParamIdx].TargetParam = mCurrMetaTargetParam;
    } else {
      toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
        diag::error_expect_function_param);
      toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
        diag::note_declared_at);
      return false;
    }
    return true;
  }

  bool VisitReplaceClauseExpr(DeclRefExpr *Expr) {
    mCurrFunc->Meta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto PD = dyn_cast<ParmVarDecl>(ND)) {
      auto Ty = getCanonicalUnqualifiedType(PD);
      if (auto PtrTy = dyn_cast<clang::PointerType>(Ty)) {
        auto PointeeTy = PtrTy->getPointeeType().getTypePtr();
        if (auto StructTy = dyn_cast<clang::RecordType>(PointeeTy)) {
          mCurrFunc->Candidates.try_emplace(PD);
        } else {
          toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
                 diag::warn_disable_replace_struct_no_struct);
        }
      } else {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
          diag::warn_disable_replace_struct_no_pointer);
      }
    } else {
      toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
        diag::warn_disable_replace_struct_no_param);
    }
    return true;
  }

  /// Check that the last metadata clause is correct.
  bool checkMetadataClauseEnd(SourceLocation BeginLoc, SourceLocation EndLoc) {
    if (mCurrFunc->Targets.empty())
      return true;
    auto *TargetFD = mCurrFunc->Targets.back().TargetDecl;
    unsigned ParamIdx = mCurrFunc->Targets.back().Parameters.size();
    if (!mCurrFunc->Targets.back().valid(&ParamIdx)) {
      toDiag(mSrcMgr.getDiagnostics(), BeginLoc,
             diag::error_replace_md_missing);
      toDiag(mSrcMgr.getDiagnostics(),
             mCurrFD->getParamDecl(ParamIdx)->getLocation(),
             diag::note_replace_md_no_param);
      mCurrFunc->Targets.pop_back();
      return false;
    } else if (TargetFD->getNumParams() != mCurrMetaTargetParam) {
      toDiag(mSrcMgr.getDiagnostics(), EndLoc,
             diag::error_replace_md_target_param_expected);
      toDiag(mSrcMgr.getDiagnostics(),
             TargetFD->getParamDecl(mCurrMetaTargetParam)->getLocation(),
             diag::note_replace_md_no_param);
      mCurrFunc->Targets.pop_back();
      return false;
    }
    return true;
  }

  TransformationContext &mTfmCtx;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  ReplacementMap &mReplacements;

  FunctionDecl *mCurrFD = nullptr;
  std::unique_ptr<FunctionInfo> mCurrFunc;
  ClauseId mInClause = ClauseId::NotClause;
  SourceLocation mCurrClauseBeginLoc;

  FunctionDecl *mCurrWithTarget = nullptr;

  unsigned mCurrMetaTargetParam = 0;
  FieldDecl *mCurrMetaMember = nullptr;
  SourceLocation mCurrMetaBeginLoc;
};

/// Return metadata which are necessary to process request or nullptr.
///
/// Emit diagnostics if request is not valid.
ReplacementMetadata * findRequestMetadata(
    const ReplacementRequests::value_type &Request,
    const ReplacementMap &ReplacementInfo, const SourceManager &SrcMgr) {
  auto TargetItr = ReplacementInfo.find(Request.get<FunctionDecl>());
  auto toDiagNoMetadata = [&Request, &SrcMgr]() {
    toDiag(SrcMgr.getDiagnostics(), Request.get<CallExpr>()->getLocStart(),
           diag::warn_replace_call_unable);
    toDiag(SrcMgr.getDiagnostics(), Request.get<SourceLocation>(),
           diag::note_replace_call_no_md)
        << Request.get<FunctionDecl>();
    toDiag(SrcMgr.getDiagnostics(),
           Request.get<FunctionDecl>()->getLocation(),
           diag::note_declared_at);
  };
  if (TargetItr == ReplacementInfo.end()) {
    toDiagNoMetadata();
    return nullptr;
  }
  auto CalleeFD = Request.get<clang::CallExpr>()->getDirectCallee();
  if (!CalleeFD) {
    toDiag(SrcMgr.getDiagnostics(),
           Request.get<clang::CallExpr>()->getLocStart(),
      diag::warn_replace_call_indirect_unable);
    return nullptr;
  }
  CalleeFD = CalleeFD->getCanonicalDecl();
  auto &TargetInfo = *TargetItr->get<FunctionInfo>();
  auto MetaItr = llvm::find_if(
      TargetInfo.Targets, [CalleeFD](const ReplacementMetadata &RM) {
        return RM.TargetDecl == CalleeFD;
      });
  if (MetaItr == TargetInfo.Targets.end()) {
    toDiagNoMetadata();
    return nullptr;
  }
  return &*MetaItr;
}

ReplacementCandidates::iterator isExprInCandidates(clang::Expr *ArgExpr,
    ReplacementCandidates &Candidates) {
  if (auto *Cast = dyn_cast<ImplicitCastExpr>(ArgExpr))
    if (Cast->getCastKind() == CK_LValueToRValue)
      ArgExpr = Cast->getSubExpr();
  if (auto *DRE = dyn_cast<DeclRefExpr>(ArgExpr))
    return Candidates.find(DRE->getFoundDecl());
  return Candidates.end();
}

class ReplacementSanitizer : public RecursiveASTVisitor<ReplacementSanitizer> {
public:
  using ReplacementCandidates =
    SmallDenseMap<NamedDecl *, SmallVector<Replacement, 8>, 8>;

  ReplacementSanitizer(TransformationContext &TfmCtx, FunctionInfo &RC,
      ReplacementMap &ReplacementInfo)
     : mTfmCtx(TfmCtx)
     , mSrcMgr(mTfmCtx.getContext().getSourceManager())
     , mReplacements(RC)
     , mReplacementInfo(ReplacementInfo)
  {}

  bool TraverseStmt(Stmt *S) {
    if (S && std::distance(S->child_begin(), S->child_end()) > 1) {
      LLVM_DEBUG(if (mInAssignment) dbgs()
                 << "[REPLACE]: disable assignment check\n");
      mInAssignment = false;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseCallExpr(clang::CallExpr *Expr) {
    auto RequestItr = mReplacements.Requests.find(Expr);
    if (RequestItr == mReplacements.Requests.end())
      return RecursiveASTVisitor::TraverseCallExpr(Expr);
    assert(RequestItr->get<clang::FunctionDecl>() &&
           "Target function must not be null!");
    auto Meta = findRequestMetadata(*RequestItr, mReplacementInfo, mSrcMgr);
    if (!Meta) {
      mReplacements.Requests.erase(RequestItr);
      return RecursiveASTVisitor::TraverseCallExpr(Expr);
    }
    for (unsigned ArgIdx = 0, EI= Expr->getNumArgs(); ArgIdx < EI; ++ArgIdx) {
      auto ArgExpr = Expr->getArg(ArgIdx);
      auto ReplacementItr =
          isExprInCandidates(ArgExpr, mReplacements.Candidates);
      if (ReplacementItr != mReplacements.Candidates.end()) {
        for (auto &ParamMeta : Meta->Parameters) {
          if (*ParamMeta.TargetParam != ArgIdx)
            continue;
          if (!ParamMeta.TargetMember) {
            toDiag(mSrcMgr.getDiagnostics(),
                   ReplacementItr->get<NamedDecl>()->getLocStart(),
                   diag::warn_disable_replace_struct);
            toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
                   diag::note_replace_struct_arrow);
            mReplacements.Candidates.erase(ReplacementItr->get<NamedDecl>());
            break;
          }
          auto Itr = addToReplacement(ParamMeta.TargetMember,
                                      ReplacementItr->get<Replacement>());
        }
      } else if (!RecursiveASTVisitor::TraverseStmt(Expr->getArg(ArgIdx))) {
        return false;
      }
    }
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    mLastDeclRef = nullptr;
    if (!mIsInnermostMember && !mReplacements.inClause(Expr)) {
      auto ND = Expr->getFoundDecl();
      if (mReplacements.Candidates.count(ND)) {
        toDiag(mSrcMgr.getDiagnostics(), ND->getLocStart(),
          diag::warn_disable_replace_struct);
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
          diag::note_replace_struct_arrow);
        mReplacements.Candidates.erase(ND);
      }
    } else {
      mLastDeclRef = Expr;
    }
    return true;
  }

  bool TraverseMemberExpr(MemberExpr *Expr) {
    mIsInnermostMember = true;
    auto Res = RecursiveASTVisitor::TraverseMemberExpr(Expr);
    if (mIsInnermostMember && mLastDeclRef) {
      auto ND = mLastDeclRef->getFoundDecl();
      auto ReplacementItr = mReplacements.Candidates.find(ND);
      if (ReplacementItr != mReplacements.Candidates.end()) {
        if (!Expr->isArrow()) {
          toDiag(mSrcMgr.getDiagnostics(), ND->getLocStart(),
            diag::warn_disable_replace_struct);
          toDiag(mSrcMgr.getDiagnostics(), Expr->getOperatorLoc(),
            diag::note_replace_struct_arrow);
          mReplacements.Candidates.erase(ReplacementItr);
        } else {
          auto Itr = addToReplacement(Expr->getMemberDecl(),
            ReplacementItr->get<Replacement>());
          Itr->Ranges.emplace_back(Expr->getSourceRange());
          Itr->InAssignment |= mInAssignment;
        }
      }
    }
    mIsInnermostMember = false;
    return Res;
  }

  bool TraverseBinAssign(BinaryOperator *BO) {
    mInAssignment = true;
    LLVM_DEBUG(dbgs() << "[REPLACE]: check assignment at ";
               BO->getOperatorLoc().print(dbgs(), mSrcMgr); dbgs() << "\n");
    auto Res = TraverseStmt(BO->getLHS());
    LLVM_DEBUG(dbgs() << "[REPLACE]: disable assignment check\n");
    mInAssignment = false;
    return Res && TraverseStmt(BO->getRHS());
  }

private:
  auto addToReplacement(ValueDecl *Member, SmallVectorImpl<Replacement> &List)
      -> SmallVectorImpl<Replacement>::iterator {
    auto Itr = find_if(
        List, [Member](const Replacement &R) { return R.Member == Member; });
    if (Itr == List.end()) {
      List.emplace_back(Member);
      return List.end() - 1;
    }
    return Itr;
  }

  TransformationContext &mTfmCtx;
  SourceManager &mSrcMgr;
  FunctionInfo &mReplacements;
  ReplacementMap &mReplacementInfo;

  bool mIsInnermostMember = false;
  DeclRefExpr *mLastDeclRef;
  bool mInAssignment = false;
};

/// Check that types which are necessary to build checked declaration are
/// available outside the root declaration.
class TypeSearch : public RecursiveASTVisitor<TypeSearch> {
public:
  TypeSearch(NamedDecl *Root, NamedDecl *Check, SourceManager &SrcMgr,
      const GlobalInfoExtractor &GlobalInfo)
    : mRootDecl(Root), mCheckDecl(Check)
    , mSrcMgr(SrcMgr), mGlobalInfo(GlobalInfo) {
    assert(Root && "Declaration must not be null!");
    assert(Check && "Declaration must not be null!");
  }

  bool VisitTagType(TagType *TT) {
    if (!mGlobalInfo.findOutermostDecl(TT->getDecl())) {
      toDiag(mSrcMgr.getDiagnostics(), mRootDecl->getLocation(),
             diag::warn_disable_replace_struct);
      toDiag(mSrcMgr.getDiagnostics(), mCheckDecl->getLocStart(),
             diag::note_replace_struct_decl);
      mIsOk = false;
      return false;
    }
    return true;
  }

  bool isOk() const noexcept { return mIsOk; }

private:
  NamedDecl *mRootDecl;
  NamedDecl *mCheckDecl;
  SourceManager &mSrcMgr;
  const GlobalInfoExtractor &mGlobalInfo;
  bool mIsOk = true;
};

/// Insert #pragma inside the body of a new function to describe its relation
/// with the original function.
void addPragmaMetadata(FunctionDecl &FuncDecl,
    ReplacementCandidates &Candidates,
    SourceManager &SrcMgr, const LangOptions &LangOpts,
    ExternalRewriter &Canvas) {
  SmallString<256> MDPragma;
  MDPragma.push_back('\n');
  getPragmaText(ClauseId::ReplaceMetadata, MDPragma);
  if (MDPragma.back() == '\n')
    MDPragma.pop_back();
  MDPragma.push_back('(');
  MDPragma += FuncDecl.getName();
  MDPragma.push_back('(');
  for (unsigned I = 0, EI = FuncDecl.getNumParams(); I < EI; ++I) {
    auto *PD = FuncDecl.getParamDecl(I);
    if (I > 0)
      MDPragma.push_back(',');
    auto ReplacementItr = Candidates.find(PD);
    if (ReplacementItr == Candidates.end()) {
      MDPragma += PD->getName();
      continue;
    }
    MDPragma += "{";
    auto Itr = ReplacementItr->get<Replacement>().begin();
    auto EndItr = ReplacementItr->get<Replacement>().end();
    if (Itr != EndItr) {
      MDPragma += ".";
      MDPragma += Itr->Member->getName();
      MDPragma += "=";
      MDPragma += Itr->Identifier;
      ++Itr;
    }
    for (auto &R : make_range(Itr, EndItr)) {
      MDPragma += ',';
      MDPragma += ".";
      MDPragma += R.Member->getName();
      MDPragma += "=";
      MDPragma += R.Identifier;
    }
    MDPragma.push_back('}');
  }
  MDPragma.push_back(')');
  MDPragma.push_back(')');
  auto FuncBody = FuncDecl.getBody();
  assert(FuncBody && "Body of a transformed function must be available!");
  auto NextToBraceLoc = SrcMgr.getExpansionLoc(FuncBody->getLocStart());
  Token Tok;
  if (getRawTokenAfter(NextToBraceLoc, SrcMgr, LangOpts, Tok) ||
      SrcMgr.getPresumedLineNumber(Tok.getLocation()) ==
        SrcMgr.getPresumedLineNumber(NextToBraceLoc)) {
    MDPragma.push_back('\n');
  }
  NextToBraceLoc = NextToBraceLoc.getLocWithOffset(1);
  Canvas.InsertTextAfter(NextToBraceLoc, MDPragma);
}

template<class RewriterT > bool replaceCalls(FunctionDecl *FuncDecl,
    FunctionInfo &FI, ReplacementMap &ReplacementInfo, RewriterT &Rewriter) {
  auto &SrcMgr = Rewriter.getSourceMgr();
  LLVM_DEBUG(printRequestLog(FuncDecl, FI.Requests, SrcMgr));
  bool IsChanged = false;
  for (auto &Request : FI.Requests) {
    assert(Request.get<clang::CallExpr>() && "Call must not be null!");
    assert(Request.get<clang::FunctionDecl>() &&
           "Target function must not be null!");
    auto Meta = findRequestMetadata(Request, ReplacementInfo, SrcMgr);
    if (!Meta)
      continue;
    SmallString<256> NewCallExpr;
    NewCallExpr += Request.get<FunctionDecl>()->getName();
    NewCallExpr += '(';
    for (unsigned I = 0, EI = Meta->Parameters.size(); I < EI; ++I) {
      if (I > 0)
        NewCallExpr += ", ";
      auto &ParamInfo = Meta->Parameters[I];
      auto ArgExpr =
          Request.get<clang::CallExpr>()->getArg(*ParamInfo.TargetParam);
      auto ReplacementItr = isExprInCandidates(ArgExpr, FI.Candidates);
      if (ReplacementItr == FI.Candidates.end()) {
        if (ParamInfo.IsPointer)
          NewCallExpr += '&';
        if (ParamInfo.TargetMember) {
          NewCallExpr += '(';
          NewCallExpr += Rewriter.getRewrittenText(
            SrcMgr.getExpansionRange(ArgExpr->getSourceRange()).getAsRange());
          NewCallExpr += ')';
          NewCallExpr += "->";
          NewCallExpr += ParamInfo.TargetMember->getName();
        } else {
          if (ParamInfo.IsPointer)
            NewCallExpr += '(';
          NewCallExpr += Rewriter.getRewrittenText(
            SrcMgr.getExpansionRange(ArgExpr->getSourceRange()).getAsRange());
          if (ParamInfo.IsPointer)
            NewCallExpr += ')';
        }
      } else {
        auto Itr = find_if(ReplacementItr->get<Replacement>(),
                        [&ParamInfo](const Replacement &R) {
                          return R.Member == ParamInfo.TargetMember;
                        });
        assert(Itr != ReplacementItr->get<Replacement>().end() &&
               "Description of the replacement must be found!");
        if (Itr->InAssignment || FI.Strict) {
          if (ParamInfo.IsPointer) {
            NewCallExpr += Itr->Identifier;
          } else {
            NewCallExpr += "*";
            NewCallExpr += Itr->Identifier;
          }
        } else if (ParamInfo.IsPointer) {
          NewCallExpr += "&";
          NewCallExpr += Itr->Identifier;
        } else {
          NewCallExpr += Itr->Identifier;
        }
      }
    }
    NewCallExpr += ')';
    Rewriter.ReplaceText(
        getExpansionRange(SrcMgr,
                          Request.get<clang::CallExpr>()->getSourceRange())
            .getAsRange(),
        NewCallExpr);
    IsChanged = true;
  }
  return IsChanged;
}

#ifdef LLVM_DEBUG
void printMetadataLog(FunctionDecl *FD, ReplacementTargets &Sources) {
  if (Sources.empty())
    return;
  dbgs() << "[REPLACE]: target found '" << FD->getName() << "'\n";
  for (auto &SI : Sources) {
    dbgs() << "[REPLACE]: source for target '" << SI.TargetDecl->getName();
    if (!SI.valid()) {
      dbgs() << " is not valid\n";
      continue;
    }
    dbgs() << "\n";
    for (unsigned I = 0, EI = SI.Parameters.size(); I < EI; ++I) {
      auto &PI = SI.Parameters[I];
      auto TargetParam = SI.TargetDecl->getParamDecl(*PI.TargetParam);
      dbgs() << "[REPLACE]: replacement " << TargetParam->getName()
        << "." << PI.TargetMember->getName() << "->"
        << FD->getParamDecl(I)->getName() << " ("
        << (PI.IsPointer ? "pointer" : "value") << ")\n";
    }
  }
}

void printCandidateLog(ReplacementCandidates &Candidates, bool IsStrict) {
  dbgs() << "[REPLACE]: " << (IsStrict ? "strict" : "nostrict")
    << " replacement\n";
  dbgs() << "[REPLACE]: replacement candidates found";
  for (auto &Candidate : Candidates)
    dbgs() << " " << Candidate.get<NamedDecl>()->getName();
  dbgs() << "\n";
}

void printRequestLog(FunctionDecl *FD, ReplacementRequests &Requests,
    const SourceManager &SrcMgr) {
  if (Requests.empty())
    return;
  dbgs() << "[REPLACE]: callee replacement requests inside '" << FD->getName()
         << "' found\n";
  for (auto &Request : Requests) {
    dbgs() << "[REPALCE]: with " << Request.get<FunctionDecl>()->getName()
           << " at ";
    Request.get<clang::CallExpr>()->getLocStart().print(dbgs(), SrcMgr);
    dbgs() << "\n";
  }
}
#endif

} // namespace

char ClangStructureReplacementPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangStructureReplacementPass,
  "clang-struct-replacement", "Source-level Structure Replacement (Clang)",
  false, false,
  tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangIncludeTreePass)
INITIALIZE_PASS_IN_GROUP_END(ClangStructureReplacementPass,
  "clang-struct-replacement", "Source-level Structure Replacement (Clang)",
  false, false,
  tsar::TransformationQueryManager::getPassRegistry())

ModulePass * llvm::createClangStructureReplacementPass() {
  return new ClangStructureReplacementPass;
}

bool ClangStructureReplacementPass::runOnModule(llvm::Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  mRawInfo = &GIP.getRawInfo();
  ReplacementMap ReplacementInfo;
  ReplacementCollector Collector(*TfmCtx, ReplacementInfo);
  Collector.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  auto &Rewriter = TfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  if (SrcMgr.getDiagnostics().hasErrorOccurred())
    return false;
  for (auto &Info : ReplacementInfo) {
    auto FuncDecl = Info.get<FunctionDecl>();
    if (!Info.get<FunctionInfo>()->hasCandidates()) {
      if (replaceCalls(FuncDecl, *Info.get<FunctionInfo>(), ReplacementInfo,
                       Rewriter)) {
        Rewriter::RewriteOptions RemoveEmptyLine;
        /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
        /// set to true then removing (in RewriterBuffer) works incorrect.
        RemoveEmptyLine.RemoveLineIfEmpty = false;
        for (auto SR : Info.get<FunctionInfo>()->ToRemoveTransform)
          Rewriter.RemoveText(SR, RemoveEmptyLine);
      }
      continue;
    }
    ReplacementSanitizer Verifier(*TfmCtx, *Info.get<FunctionInfo>(),
                                  ReplacementInfo);
    Verifier.TraverseDecl(FuncDecl);
    LLVM_DEBUG(printMetadataLog(FuncDecl, Info.get<FunctionInfo>()->Targets));
    if (!Info.get<FunctionInfo>()->hasCandidates())
      continue;
    // Check general preconditions.
    auto FuncRange = SrcMgr.getExpansionRange(FuncDecl->getSourceRange());
    if (!SrcMgr.isWrittenInSameFile(FuncRange.getBegin(), FuncRange.getEnd())) {
      toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocation(),
        diag::warn_disable_replace_struct);
      toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocStart(),
        diag::note_source_range_not_single_file);
      toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocEnd(),
        diag::note_end_location);
      continue;
    }
    if (SrcMgr.getFileCharacteristic(FuncDecl->getLocStart()) !=
      SrcMgr::C_User) {
      toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocation(),
        diag::warn_disable_replace_struct_system);
      continue;
    }
    if (Info.get<FunctionInfo>()->Strict) {
      bool HasMacro = false;
      for_each_macro(FuncDecl, SrcMgr, LangOpts, mRawInfo->Macros,
        [FuncDecl, &SrcMgr, &HasMacro](SourceLocation Loc) {
          if (!HasMacro) {
            HasMacro = true;
            toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocation(),
              diag::warn_disable_replace_struct);
            toDiag(SrcMgr.getDiagnostics(), Loc,
              diag::note_replace_struct_macro_prevent);
          }
        });
      if (HasMacro)
        continue;
    }
    LLVM_DEBUG(printCandidateLog(Info.get<FunctionInfo>()->Candidates,
                                 Info.get<FunctionInfo>()->Strict));
    LLVM_DEBUG(
        printRequestLog(FuncDecl, Info.get<FunctionInfo>()->Requests, SrcMgr));
    ExternalRewriter Canvas(
      tsar::getExpansionRange(SrcMgr, FuncDecl->getSourceRange()).getAsRange(),
      SrcMgr, LangOpts);
    // Build unique name for a new function.
    SmallString<32> NewName;
    addSuffix(FuncDecl->getName() + "_spf", NewName);
    SourceRange NameRange(SrcMgr.getExpansionLoc(FuncDecl->getLocation()));
    NameRange.setEnd(
      NameRange.getBegin().getLocWithOffset(FuncDecl->getName().size() - 1));
    Canvas.ReplaceText(NameRange, NewName);
    // Look up for declaration of types of parameters.
    auto &FT = getAnalysis<ClangIncludeTreePass>().getFileTree();
    std::string Context;
    auto *OFD = GIP.getGlobalInfo().findOutermostDecl(FuncDecl);
    assert(OFD && "Outermost declaration for the current function must be known!");
    auto Root = FileNode::ChildT(FT.findRoot(OFD));
    assert(Root && "File which contains declaration must be known!");
    for (auto &Internal : FT.internals()) {
      if (auto *TD = dyn_cast<TypeDecl>(Internal.getDescendant())) {
        Context += Lexer::getSourceText(
          SrcMgr.getExpansionRange(TD->getSourceRange()), SrcMgr, LangOpts);
        Context += ";";
      }
    }
    for (auto *N : depth_first(&Root)) {
      if (N->is<FileNode *>())
        continue;
      auto *OD = N->get<const FileNode::OutermostDecl *>();
      if (OD == OFD)
        break;
      if (auto *TD = dyn_cast<TypeDecl>(OD->getDescendant())) {
        Context += Lexer::getSourceText(
          SrcMgr.getExpansionRange(TD->getSourceRange()), SrcMgr, LangOpts);
        Context += ";";
      }
    }
    // Replace aggregate parameters with separate variables.
    StringMap<std::string> Replacements;
    bool TheLastParam = true;
    for (unsigned I = FuncDecl->getNumParams(); I > 0; --I) {
      auto *PD = FuncDecl->getParamDecl(I - 1);
      auto ReplacementItr = Info.get<FunctionInfo>()->Candidates.find(PD);
      if (ReplacementItr == Info.get<FunctionInfo>()->Candidates.end()) {
        TheLastParam = false;
        continue;
      }
      SmallString<128> NewParams;
      // We also remove an unused parameter if it is mentioned in replace clause.
      if (ReplacementItr->get<Replacement>().empty()) {
        SourceLocation EndLoc = PD->getLocEnd();
        Token CommaTok;
        if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
          CommaTok)) {
          toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
            diag::warn_disable_replace_struct);
          toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
            diag::note_replace_struct_de_decl);
          Info.get<FunctionInfo>()->Candidates.erase(ReplacementItr);
          TheLastParam = false;
          continue;
        }
        if (CommaTok.is(tok::comma))
          EndLoc = CommaTok.getLocation();
        Canvas.RemoveText(SrcMgr
          .getExpansionRange(CharSourceRange::getTokenRange(
            PD->getLocStart(), EndLoc))
          .getAsRange());
        toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
          diag::remark_replace_struct);
        toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
          diag::remark_remove_de_decl);
        // Do not update TheLastParam variable. If the current parameter is the
        // last in the list and if it is removed than the previous parameter
        // in the list become the last one.
        continue;
      }
      auto StashContextSize = Context.size();
      for (auto &R : ReplacementItr->get<Replacement>()) {
        TypeSearch TS(PD, R.Member, SrcMgr, GIP.getGlobalInfo());
        TS.TraverseDecl(R.Member);
        if (!TS.isOk()) {
          Context.resize(StashContextSize);
          NewParams.clear();
          break;
        }
        addSuffix(PD->getName() + "_" + R.Member->getName(), R.Identifier);
        auto ParamType = (R.InAssignment || Info.get<FunctionInfo>()->Strict)
          ? R.Member->getType().getAsString() + "*"
          : R.Member->getType().getAsString();
        auto Tokens =
          buildDeclStringRef(ParamType, R.Identifier, Context, Replacements);
        if (Tokens.empty()) {
          Context.resize(StashContextSize);
          NewParams.clear();
          toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
            diag::warn_disable_replace_struct);
          toDiag(SrcMgr.getDiagnostics(), R.Member->getLocStart(),
            diag::note_replace_struct_decl);
          break;
        }
        if (!NewParams.empty())
          NewParams.push_back(',');
        auto Size = NewParams.size();
        join(Tokens.begin(), Tokens.end(), " ", NewParams);
        Context += StringRef(NewParams.data() + Size, NewParams.size() - Size);
        Context += ";";
        LLVM_DEBUG(dbgs() << "[REPLACE]: replacement for " << I
                          << " parameter: "
                          << StringRef(NewParams.data() + Size,
                                       NewParams.size() - Size)
                          << "\n");
      }
      if (!NewParams.empty()) {
        SourceLocation EndLoc = PD->getLocEnd();
        // If the next parameter in the parameter list is unused and it has been
        // successfully remove, we have to remove comma after the current
        // parameter.
        if (TheLastParam) {
          Token CommaTok;
          if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
            CommaTok)) {
            toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
              diag::warn_disable_replace_struct);
            toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
              diag::note_replace_struct_decl_internal);
            Info.get<FunctionInfo>()->Candidates.erase(ReplacementItr);
            continue;
          }
          if (CommaTok.is(tok::comma))
            EndLoc = CommaTok.getLocation();
        }
        auto Range = SrcMgr
          .getExpansionRange(CharSourceRange::getTokenRange(
            PD->getLocStart(), EndLoc))
          .getAsRange();
        Canvas.ReplaceText(Range, NewParams);
      } else {
        Info.get<FunctionInfo>()->Candidates.erase(ReplacementItr);
      }
      TheLastParam = false;
    }
    // Replace accesses to parameters.
    for (auto &ParamInfo : Info.get<FunctionInfo>()->Candidates) {
      for (auto &R : ParamInfo.get<Replacement>()) {
        for (auto Range : R.Ranges) {
          SmallString<64> Tmp;
          auto AccessString = R.InAssignment || Info.get<FunctionInfo>()->Strict
            ? ("(*" + R.Identifier + ")").toStringRef(Tmp)
            : StringRef(R.Identifier);
          Canvas.ReplaceText(Range, AccessString);
        }
      }
    }
    if (!Info.get<FunctionInfo>()->hasCandidates())
      continue;
    replaceCalls(FuncDecl, *Info.get<FunctionInfo>(), ReplacementInfo, Canvas);
    // Remove pragmas from the original function and its clone if replacement
    // is still possible.
    Rewriter::RewriteOptions RemoveEmptyLine;
    /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
    /// set to true then removing (in RewriterBuffer) works incorrect.
    RemoveEmptyLine.RemoveLineIfEmpty = false;
    for (auto SR : Info.get<FunctionInfo>()->ToRemoveTransform) {
      Rewriter.RemoveText(SR, RemoveEmptyLine);
      Canvas.RemoveText(SR, true);
    }
    for (auto SR : Info.get<FunctionInfo>()->ToRemoveMetadata)
      Canvas.RemoveText(SR, true);
    addPragmaMetadata(
      *FuncDecl, Info.get<FunctionInfo>()->Candidates, SrcMgr, LangOpts, Canvas);
    // Update sources.
    auto OriginDefString = Lexer::getSourceText(
      CharSourceRange::getTokenRange(
        FuncDecl->getBeginLoc(),
        FuncDecl->getParamDecl(FuncDecl->getNumParams() - 1)->getEndLoc()),
      SrcMgr, LangOpts);
    auto LocToInsert = SrcMgr.getExpansionLoc(FuncDecl->getLocEnd());
    Rewriter.InsertTextAfterToken(
      LocToInsert,
      ("\n\n/* Replacement for " + OriginDefString + ") */\n").str());
    Rewriter.InsertTextAfterToken(LocToInsert, Canvas.getBuffer());
  }
  return false;
}
