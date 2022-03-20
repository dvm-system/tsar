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
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/Analysis/CallGraph.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace tsar;
using namespace clang;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-struct-replacement"

namespace {
inline const clang::Type *getCanonicalUnqualifiedType(ValueDecl *VD) {
  return VD->getType()
      .getTypePtr()
      ->getCanonicalTypeUnqualified()
      ->getTypePtr();
}

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

struct Replacement {
  Replacement(ValueDecl *M) : Member(M) {}

  /// Member, this replacement belongs to, of a parameter which should be
  /// replaced.
  ValueDecl *Member;

  /// Locations in a source code which contains accesses to the member 'Member'
  /// of an original parameter and must be replaced.
  std::vector<SourceRange> RangesToReplace;

  /// Locations in a source code which contains accesses to the member 'Member'
  /// of an original parameter and must be removed.
  ///
  /// For example, to transform `S[I].X` we replace `S[I]` with `S_X0[I]` and
  /// remove `.X`.
  std::vector<SourceRange> RangesToRemove;

  /// Identifier of a new parameter which corresponds to the member 'Member' of
  /// an original parameter which should be replaced.
  SmallString<32> Identifier;

  enum ReplacemntFlags : uint8_t {
    NoFlag = 0u,
    /// It is set if a value of the member 'Member' of an original
    /// parameter can be changed in the original function call.
    InAssignment = 1u << 0,
    /// It is set if an accessed 'Member' is in array of structures.
    InArray = 1u << 2,
    LLVM_MARK_AS_BITMASK_ENUM(InArray)
  } Flags = NoFlag;
};

/// Map from parameter to its replacement which is list of necessary members and
/// replacement string.
using ReplacementCandidates = SmallDenseMap<
    NamedDecl *,
    std::tuple<DeclStmt *, SmallVector<Replacement, 8>, std::string>, 8,
    DenseMapInfo<NamedDecl *>,
    TaggedDenseMapTuple<bcl::tagged<NamedDecl *, NamedDecl>,
                        bcl::tagged<DeclStmt *, DeclStmt>,
                        bcl::tagged<SmallVector<Replacement, 8>, Replacement>,
                        bcl::tagged<std::string, std::string>>>;

/// Description of a possible replacement of a source function.
struct ReplacementMetadata {
  enum ParamKind : uint8_t { PK_Value, PK_Pointer, PK_Array };
  struct ParamReplacement {
    Optional<unsigned> TargetParam;
    FieldDecl *TargetMember = nullptr;
    ParamKind Kind = PK_Value;
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
  CanonicalDeclPtr<FunctionDecl> TargetDecl = nullptr;

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

/// Set of calls that should be implicitly requested due to accesses to
/// replace candidates.
using ReplacementImplicitRequests = DenseSet<clang::CallExpr *>;

struct FunctionInfo {
  FunctionInfo(FunctionDecl *FD) {
    if (FD->doesThisDeclarationHaveABody()) {
      Definition = FD;
    } else {
      const FunctionDecl *D = FD->getFirstDecl();
      if (D->hasBody(D))
        Definition = const_cast<FunctionDecl *>(D);
    }
    assert(
        Definition &&
        "FunctionInfo can be created for a function with a known body only!");
  }

  /// Function redeclaration which has a body.
  FunctionDecl *Definition = nullptr;

  /// List of parameters of this function, which are specified in 'replace'
  /// clause, which should be replaced.
  ReplacementCandidates Candidates;

  /// List of declarations statements which contain replacement candidates.
  SmallSetVector<DeclStmt *, 8> DeclarationStmts;

  /// List of calls from this function, which are marked with a 'with' clause,
  /// which should be replaced.
  ReplacementRequests Requests;

  /// List of calls that should be implicitly requested due to accesses to
  /// replace candidates.
  ReplacementImplicitRequests ImplicitRequests;

  /// Calls to functions from this list can be replaced with this function.
  ReplacementTargets Targets;

  /// Source ranges which correspond to transformation clauses and which
  /// can be successfully removed.
  SmallVector<CharSourceRange, 8> ToRemoveTransform;

  /// Source ranges which corresponds to metadata clauses
  /// which can be successfully removed.
  SmallVector<CharSourceRange, 8> ToRemoveMetadata;

  /// Source ranges which have to be removed from the clone only.
  SmallVector<CharSourceRange, 8> ToRemoveClone;

  /// Name of a new function which is a result of replacement.
  SmallString<32> ReplacmenetName;

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
    DenseMap<CanonicalDeclPtr<FunctionDecl>, std::unique_ptr<FunctionInfo>,
             DenseMapInfo<CanonicalDeclPtr<FunctionDecl>>,
             TaggedDenseMapPair<
                 bcl::tagged<CanonicalDeclPtr<FunctionDecl>, FunctionDecl>,
                 bcl::tagged<std::unique_ptr<FunctionInfo>, FunctionInfo>>>;


clang::DeclRefExpr *getCandidate(clang::Expr *ArgExpr) {
  while (auto *UO{dyn_cast<UnaryOperator>(ArgExpr)})
    if (UO->getOpcode() == UnaryOperatorKind::UO_Deref ||
        UO->getOpcode() == UnaryOperatorKind::UO_AddrOf)
      ArgExpr = UO->getSubExpr();
    else
      break;
  if (auto *Cast = dyn_cast<ImplicitCastExpr>(ArgExpr))
    if (Cast->getCastKind() == CK_LValueToRValue)
      ArgExpr = Cast->getSubExpr();
  return dyn_cast<DeclRefExpr>(ArgExpr);
}

const clang::DeclRefExpr *getCandidate(const clang::Expr *ArgExpr) {
  while (auto *UO{dyn_cast<UnaryOperator>(ArgExpr)})
    if (UO->getOpcode() == UnaryOperatorKind::UO_Deref ||
        UO->getOpcode() == UnaryOperatorKind::UO_AddrOf)
      ArgExpr = UO->getSubExpr();
    else
      break;
  if (auto *Cast = dyn_cast<ImplicitCastExpr>(ArgExpr))
    if (Cast->getCastKind() == CK_LValueToRValue)
      ArgExpr = Cast->getSubExpr();
  return dyn_cast<DeclRefExpr>(ArgExpr);
}

ReplacementCandidates::iterator isExprInCandidates(const clang::Expr *ArgExpr,
    ReplacementCandidates &Candidates) {
  if (auto *DRE = getCandidate(ArgExpr))
    return Candidates.find(DRE->getFoundDecl());
  return Candidates.end();
}

bool isImmediateMacroArg(SourceLocation Loc, const SourceManager &SrcMgr) {
  assert(Loc.isValid() && "Location must be valid!");
  if (!Loc.isMacroID())
    return false;
  auto &EI{SrcMgr.getSLocEntry(SrcMgr.getFileID(Loc)).getExpansion()};
  bool IsArg{EI.isMacroArgExpansion()};
  Loc = EI.getExpansionLocStart();
  bool IsPrevArg{IsArg};
  while (!Loc.isFileID()) {
    IsPrevArg = IsArg;
    auto &EI{SrcMgr.getSLocEntry(SrcMgr.getFileID(Loc)).getExpansion()};
    IsArg = EI.isMacroArgExpansion();
    Loc = EI.getExpansionLocStart();
  }
  return IsPrevArg;
}

template<class T>
bool checkMacroBoundsAndEmitDiag(T *Obj, SourceLocation FuncLoc,
                                 const SourceManager &SrcMgr,
                                 const LangOptions &LangOpts) {
  auto diag = [&SrcMgr, &FuncLoc](SourceLocation Loc) {
    toDiag(SrcMgr.getDiagnostics(), FuncLoc,
           tsar::diag::warn_disable_replace_struct);
    toDiag(SrcMgr.getDiagnostics(), Loc,
           tsar::diag::note_replace_struct_macro_prevent);
  };
  if (Obj->getBeginLoc().isMacroID() &&
      !Lexer::isAtStartOfMacroExpansion(Obj->getBeginLoc(), SrcMgr, LangOpts)) {
    diag(Obj->getBeginLoc());
    return false;
  }
  if (Obj->getEndLoc().isMacroID() &&
      !Lexer::isAtEndOfMacroExpansion(Obj->getEndLoc(), SrcMgr, LangOpts)) {
    diag(Obj->getEndLoc());
    return false;
  }
  return true;
}

using CallList = std::vector<clang::CallExpr *>;

/// This class collects all 'replace' clauses in the code.
class ReplacementCollector : public RecursiveASTVisitor<ReplacementCollector> {
public:
  ReplacementCollector(ClangTransformationContext &TfmCtx,
      const ASTImportInfo &ImportInfo,
      ReplacementMap &Replacements, CallList &Calls)
     : mTfmCtx(TfmCtx)
     , mImportInfo(ImportInfo)
     , mSrcMgr(TfmCtx.getContext().getSourceManager())
     , mLangOpts(TfmCtx.getContext().getLangOpts())
     , mReplacements(Replacements)
     , mCalls(Calls)
  {}

  /// Return list of parameters to replace.
  ReplacementMap & getReplacementInfo() noexcept {
    return mReplacements;
  }

  /// Return list of parameters to replace.
  const ReplacementMap & getReplacementInfo() const noexcept {
    return mReplacements;
  }

  /// Return list of visited call expressions.
  CallList & getCalls() noexcept {
    return mCalls;
  }

  /// Return list of visited call expressions.
  const CallList & getCalls() const noexcept {
    return mCalls;
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
        mImportInfo, mCurrFunc->ToRemoveTransform, PragmaFlags::IsInHeader);
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive);
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
      pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts, mImportInfo,
        mCurrFunc->ToRemoveMetadata, PragmaFlags::IsInHeader);
      mInClause = ClauseId::ReplaceMetadata;
      for (auto *C : Clauses) {
        mCurrClauseBeginLoc = C->getBeginLoc();
        for (auto *S : Pragma::clause(&C))
          if (!RecursiveASTVisitor::TraverseStmt(S))
            break;
          checkMetadataClauseEnd(mCurrMetaBeginLoc, C->getEndLoc());
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
             tsar::diag::error_replace_md);
      toDiag(mSrcMgr.getDiagnostics(), StructDecl->getLocation(),
             tsar::diag::note_record_member_unknown)
          << SL->getString();
      return false;
    }
    mCurrMetaMember = *MemberItr;
    mIsCurrMetaMemberArray = false;
    LocalLexer Lex(mSrcMgr.getExpansionRange(CharSourceRange::getCharRange(
                       mCurrMetaParamLoc, SL->getBeginLoc())),
                   mSrcMgr, mLangOpts);
    clang::Token Tok;
    while (!Lex.LexFromRawLexer(Tok))
      if (Tok.is(tok::r_square)) {
        mIsCurrMetaMemberArray = true;
        break;
      }
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
    mCalls.push_back(Expr);
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
        tsar::diag::error_function_args_number) << mCurrMetaTargetParam + 1;
      toDiag(mSrcMgr.getDiagnostics(), CurrMD.TargetDecl->getLocation(),
        clang::diag::note_declared_at);
      return false;
    }
    mCurrMetaParamLoc = CS->getBeginLoc();
    auto Res = RecursiveASTVisitor::TraverseCompoundStmt(CS);
    ++mCurrMetaTargetParam;
    mCurrMetaMember = nullptr;
    mIsCurrMetaMemberArray = false;
    return Res;
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    if (!FD->doesThisDeclarationHaveABody())
      return true;
    mCurrFunc = mReplacements.try_emplace(
                    FD, std::make_unique<FunctionInfo>(FD))
                         .first->get<FunctionInfo>()
                         .get();
    auto Res =
        RecursiveASTVisitor::TraverseFunctionDecl(FD);
    if (mCurrFunc->empty())
      mReplacements.erase(FD);
    return Res;
  }

private:
  bool VisitReplaceWithClauseExpr(DeclRefExpr *Expr) {
    mCurrFunc->Meta.insert(Expr);
    if (mCurrWithTarget) {
      SmallString<32> Out;
      toDiag(mSrcMgr.getDiagnostics(), mCurrClauseBeginLoc,
             tsar::diag::error_directive_clause_twice)
          << getPragmaText(ClauseId::Replace, Out).trim('\n')
          << getName(ClauseId::With);
      return false;
    }
    auto ND = Expr->getFoundDecl();
    if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
        mCurrWithTarget = FD;
        return true;
    }
    toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
      tsar::diag::error_clause_expect_function) << getName(ClauseId::With);
    toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
           clang::diag::note_declared_at);
    return false;
  }

  bool VisitReplaceMetadataClauseExpr(DeclRefExpr *Expr) {
    assert(mCurrFunc && "Replacement description must not be null!");
    mCurrMetaParamLoc = Expr->getEndLoc();
    mCurrFunc->Meta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
      checkMetadataClauseEnd(mCurrMetaBeginLoc, Expr->getBeginLoc());
      mCurrFunc->Targets.emplace_back();
      auto &CurrMD = mCurrFunc->Targets.back();
      CurrMD.TargetDecl = FD;
      CurrMD.Parameters.resize(mCurrFunc->Definition->getNumParams());
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
      auto TargetParam =
          CurrMD.TargetDecl->getParamDecl(mCurrMetaTargetParam);
      auto LHSTy = PD->getType().getNonReferenceType();
      auto RHSTy =
          mCurrMetaMember ? mCurrMetaMember->getType() : TargetParam->getType();
      RHSTy = RHSTy.getNonReferenceType();
      auto &Sema = mTfmCtx.getCompilerInstance().getSema();
      auto ConvertTy =
          Sema.CheckAssignmentConstraints(Expr->getBeginLoc(), LHSTy, RHSTy);
      bool IsPointer = false;
      if (ConvertTy != Sema::Compatible) {
        if (auto DecayedTy = dyn_cast<clang::DecayedType>(LHSTy)) {
          // Type of parameter (LHS) in replacement candidate is an array type.
          auto LHSPointeeTy = DecayedTy->getPointeeType();
          // Discard outermost array type of RHS value because it is implicitly
          // compatible with a pointer type.
          if (auto ArrayTy = dyn_cast<clang::ArrayType>(RHSTy)) {
            auto RHSElementTy = ArrayTy->getElementType();
            auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
                Expr->getBeginLoc(), LHSPointeeTy, RHSElementTy);
            if (ConvertPointeeTy == Sema::Compatible)
              ConvertTy = ConvertPointeeTy;
          }
        } else if (auto PtrTy = dyn_cast<clang::PointerType>(LHSTy)) {
          auto LHSPointeeTy = PtrTy->getPointeeType();
          // Discard outermost array type of RHS value because it is implicitly
          // compatible with a pointer type.
          if (auto ArrayTy = dyn_cast<clang::ArrayType>(RHSTy)) {
            auto RHSElementTy = ArrayTy->getElementType();
            auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
                Expr->getBeginLoc(), LHSPointeeTy, RHSElementTy);
            if (ConvertPointeeTy == Sema::Compatible) {
              ConvertTy = ConvertPointeeTy;
            } else if (auto NestedPtrTy =
                           dyn_cast<clang::PointerType>(LHSPointeeTy)) {
              auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
                  Expr->getBeginLoc(), NestedPtrTy->getPointeeType(),
                  RHSElementTy);
              if (ConvertPointeeTy == Sema::Compatible) {
                ConvertTy = ConvertPointeeTy;
                LHSTy = LHSPointeeTy;
                IsPointer = true;
              }
            }
          } else {
            auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
              Expr->getBeginLoc(), LHSPointeeTy, RHSTy);
            if (ConvertPointeeTy == Sema::Compatible) {
              LHSTy = LHSPointeeTy;
              ConvertTy = ConvertPointeeTy;
              IsPointer = true;
            }
          }
        }
      }
      if (ConvertTy != Sema::Compatible)
        Sema.DiagnoseAssignmentResult(ConvertTy, Expr->getBeginLoc(), LHSTy,
                                      RHSTy, Expr, Sema::AA_Passing);
      if (ConvertTy == Sema::Incompatible) {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
               tsar::diag::error_replace_md_type_incompatible)
                << (mCurrMetaMember ? 0 : 1);
        toDiag(mSrcMgr.getDiagnostics(),
               mCurrMetaMember ? mCurrMetaMember->getLocation()
                               : TargetParam->getLocation(),
               clang::diag::note_declared_at);
        toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
          clang::diag::note_declared_at);
        return false;
      }
      unsigned ParamIdx = 0;
      for (unsigned EI = mCurrFunc->Definition->getNumParams(); ParamIdx < EI;
           ++ParamIdx)
        if (PD == mCurrFunc->Definition->getParamDecl(ParamIdx))
          break;
      assert(ParamIdx < mCurrFunc->Definition->getNumParams() &&
             "Unknown parameter!");
      CurrMD.Parameters[ParamIdx].Kind =
          mIsCurrMetaMemberArray ? ReplacementMetadata::PK_Array
          : IsPointer            ? ReplacementMetadata::PK_Pointer
                                 : ReplacementMetadata::PK_Value;
      CurrMD.Parameters[ParamIdx].TargetMember = mCurrMetaMember;
      if (CurrMD.Parameters[ParamIdx].TargetParam) {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
          tsar::diag::error_replace_md_param_twice);
        return false;
      }
      CurrMD.Parameters[ParamIdx].TargetParam = mCurrMetaTargetParam;
    } else {
      toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
        tsar::diag::error_expect_function_param);
      toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
        clang::diag::note_declared_at);
      return false;
    }
    return true;
  }

  bool VisitReplaceClauseExpr(DeclRefExpr *Expr) {
    mCurrFunc->Meta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto VD{dyn_cast<VarDecl>(ND)}; VD && VD->isLocalVarDeclOrParm()) {
      auto Ty = getCanonicalUnqualifiedType(VD);
      if (auto PtrTy = dyn_cast<clang::PointerType>(Ty)) {
        auto PointeeTy = PtrTy->getPointeeType().getTypePtr();
        if (auto StructTy = dyn_cast<clang::RecordType>(PointeeTy)) {
          mCurrFunc->Candidates.try_emplace(VD);
        } else {
          toDiag(mSrcMgr.getDiagnostics(), Expr->getBeginLoc(),
                 tsar::diag::warn_disable_replace_struct_no_struct);
        }
      } else if (auto StructTy = dyn_cast<clang::RecordType>(Ty)) {
        mCurrFunc->Candidates.try_emplace(VD);
      } else {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getBeginLoc(),
               tsar::diag::warn_disable_replace_struct_no_struct);
      }
    } else {
      toDiag(mSrcMgr.getDiagnostics(), Expr->getBeginLoc(),
        tsar::diag::warn_disable_replace_struct_no_local);
    }
    return true;
  }

  /// Check that the last metadata clause is correct.
  bool checkMetadataClauseEnd(SourceLocation BeginLoc, SourceLocation EndLoc) {
    if (mCurrFunc->Targets.empty())
      return true;
    auto TargetFD = mCurrFunc->Targets.back().TargetDecl;
    unsigned ParamIdx = mCurrFunc->Targets.back().Parameters.size();
    if (!mCurrFunc->Targets.back().valid(&ParamIdx)) {
      toDiag(mSrcMgr.getDiagnostics(), BeginLoc,
             tsar::diag::error_replace_md_missing);
      toDiag(mSrcMgr.getDiagnostics(),
             mCurrFunc->Definition->getParamDecl(ParamIdx)->getLocation(),
             tsar::diag::note_replace_md_no_param);
      mCurrFunc->Targets.pop_back();
      return false;
    } else if (TargetFD->getNumParams() != mCurrMetaTargetParam) {
      toDiag(mSrcMgr.getDiagnostics(), EndLoc,
             tsar::diag::error_replace_md_target_param_expected);
      toDiag(mSrcMgr.getDiagnostics(),
             TargetFD->getParamDecl(mCurrMetaTargetParam)->getLocation(),
             tsar::diag::note_replace_md_no_param);
      mCurrFunc->Targets.pop_back();
      return false;
    }
    return true;
  }

  ClangTransformationContext &mTfmCtx;
  const ASTImportInfo &mImportInfo;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  ReplacementMap &mReplacements;
  CallList &mCalls;

  FunctionInfo *mCurrFunc = nullptr;
  ClauseId mInClause = ClauseId::NotClause;
  SourceLocation mCurrClauseBeginLoc;

  FunctionDecl *mCurrWithTarget = nullptr;

  unsigned mCurrMetaTargetParam = 0;
  FieldDecl *mCurrMetaMember = nullptr;
  bool mIsCurrMetaMemberArray = false;
  SourceLocation mCurrMetaBeginLoc;
  SourceLocation mCurrMetaParamLoc;
};

/// Return metadata which are necessary to process request or nullptr.
///
/// Emit diagnostics if request is not valid.
ReplacementMetadata * findRequestMetadata(
    const ReplacementRequests::value_type &Request,
    const ReplacementMap &ReplacementInfo, const SourceManager &SrcMgr) {
  auto TargetItr = ReplacementInfo.find(Request.get<FunctionDecl>());
  auto toDiagNoMetadata = [&Request, &SrcMgr]() {
    toDiag(SrcMgr.getDiagnostics(), Request.get<CallExpr>()->getBeginLoc(),
           tsar::diag::warn_replace_call_unable);
    toDiag(SrcMgr.getDiagnostics(), Request.get<SourceLocation>(),
           tsar::diag::note_replace_call_no_md)
        << Request.get<FunctionDecl>();
    toDiag(SrcMgr.getDiagnostics(),
           Request.get<FunctionDecl>()->getLocation(),
           clang::diag::note_declared_at);
  };
  if (TargetItr == ReplacementInfo.end()) {
    toDiagNoMetadata();
    return nullptr;
  }
  CanonicalDeclPtr<FunctionDecl> CalleeFD{
      Request.get<clang::CallExpr>()->getDirectCallee()};
  if (!CalleeFD) {
    toDiag(SrcMgr.getDiagnostics(),
           Request.get<clang::CallExpr>()->getBeginLoc(),
      tsar::diag::warn_replace_call_indirect_unable);
    return nullptr;
  }
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

class ReplacementSanitizer : public RecursiveASTVisitor<ReplacementSanitizer> {
public:
  using ReplacementCandidates =
    SmallDenseMap<NamedDecl *, SmallVector<Replacement, 8>, 8>;

  ReplacementSanitizer(ClangTransformationContext &TfmCtx,
      ClangGlobalInfo::RawInfo &RawInfo, FunctionInfo &RC,
      ReplacementMap &ReplacementInfo)
     : mLangOpts(TfmCtx.getContext().getLangOpts())
     , mSrcMgr(TfmCtx.getContext().getSourceManager())
     , mRawInfo(&RawInfo)
     , mReplacements(RC)
     , mReplacementInfo(ReplacementInfo)
  {}

  bool VisitStmt(Stmt *S) {
    if (!S)
      return true;
    if (auto *Cast = dyn_cast<ImplicitCastExpr>(S))
      if (Cast->getCastKind() == CK_LValueToRValue) {
        LLVM_DEBUG(dbgs() << "[REPLACE]: disable assignment check at ";
                   S->getBeginLoc().print(dbgs(), mSrcMgr); dbgs() << "\n");
        mInAssignment = false;
        return true;
      }
    // Skip chain of cast expressions.
    if (!mInAssignment && isa<CastExpr>(S))
      return true;
    mInAssignment = true;
    LLVM_DEBUG(dbgs() << "[REPLACE]: enable assignment check at ";
               S->getBeginLoc().print(dbgs(), mSrcMgr); dbgs() << "\n");
    return true;
  }

  bool TraverseCallExpr(clang::CallExpr *Expr) {
    auto RequestItr = mReplacements.Requests.find(Expr);
    if (RequestItr == mReplacements.Requests.end()) {
      auto ImplicitItr = mReplacements.ImplicitRequests.find(Expr);
      if (ImplicitItr == mReplacements.ImplicitRequests.end())
        return RecursiveASTVisitor::TraverseCallExpr(Expr);
      if (!checkMacroBoundsAndEmitDiag(Expr,
                                       mReplacements.Definition->getLocation(),
                                       mSrcMgr, mLangOpts)) {
        mReplacements.ImplicitRequests.erase(ImplicitItr);
        return RecursiveASTVisitor::TraverseCallExpr(Expr);
      }
      bool Res = true;
      for (unsigned ArgIdx = 0, EI = Expr->getNumArgs(); ArgIdx < EI; ++ArgIdx) {
        auto ArgExpr = Expr->getArg(ArgIdx);
        auto ReplacementItr =
          isExprInCandidates(ArgExpr, mReplacements.Candidates);
        // Do not process replacement candidates if a corresponding callee
        // may be cloned further.
        if (ReplacementItr == mReplacements.Candidates.end())
          Res &= TraverseStmt(ArgExpr);
      }
      return Res;
    }
    assert(RequestItr->get<clang::FunctionDecl>() &&
           "Target function must not be null!");
    auto Meta = findRequestMetadata(*RequestItr, mReplacementInfo, mSrcMgr);
    if (!Meta || !checkMacroBoundsAndEmitDiag(
                     Expr, mReplacements.Definition->getLocation(), mSrcMgr,
                     mLangOpts)) {
      mReplacements.Requests.erase(RequestItr);
      return RecursiveASTVisitor::TraverseCallExpr(Expr);
    }
    bool Res = true;
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
                   ReplacementItr->get<NamedDecl>()->getBeginLoc(),
                   tsar::diag::warn_disable_replace_struct);
            toDiag(mSrcMgr.getDiagnostics(), Expr->getBeginLoc(),
                   tsar::diag::note_replace_struct_arrow);
            mReplacements.Candidates.erase(ReplacementItr->get<NamedDecl>());
            break;
          }
          auto Itr = addToReplacement(ParamMeta.TargetMember,
                                      ReplacementItr->get<Replacement>());
        }
      } else {
        Res &= TraverseStmt(ArgExpr);
      }
    }
    return Res;
  }

  bool VisitVarDecl(VarDecl* VD) {
    if (isa_and_nonnull<DeclStmt>(mParent))
      if (auto I{mReplacements.Candidates.find(VD)};
          I != mReplacements.Candidates.end()) {
        if (!isa_and_nonnull<CompoundStmt>(mScope)) {
        toDiag(mSrcMgr.getDiagnostics(), VD->getLocation(),
          tsar::diag::warn_disable_replace_struct);
          if (mScope)
            toDiag(mSrcMgr.getDiagnostics(), mScope->getBeginLoc(),
                   tsar::diag::note_replace_struct_not_compound_stmt);
          mReplacements.Candidates.erase(I);
        } else if (auto Init{VD->getInit()}) {
          if (!(isa<CXXConstructExpr>(Init) && cast<CXXConstructExpr>(Init)
                                                   ->getConstructor()
                                                   ->isDefaultConstructor())) {
            toDiag(mSrcMgr.getDiagnostics(), VD->getLocation(),
                   tsar::diag::warn_disable_replace_struct_init);
            mReplacements.Candidates.erase(I);
          }
        }
        I->get<DeclStmt>() = cast<DeclStmt>(mParent);
        mReplacements.DeclarationStmts.insert(cast<DeclStmt>(mParent));
      }
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    if (mReplacements.inClause(Expr))
      return true;
    if (!mIsInnermostMember) {
      auto ND = Expr->getFoundDecl();
      if (mReplacements.Candidates.count(ND)) {
        toDiag(mSrcMgr.getDiagnostics(), ND->getBeginLoc(),
          tsar::diag::warn_disable_replace_struct);
        toDiag(mSrcMgr.getDiagnostics(), Expr->getBeginLoc(),
          tsar::diag::note_replace_struct_arrow);
        mReplacements.Candidates.erase(ND);
      }
     } else {
      auto ND = Expr->getFoundDecl();
      auto ReplacementItr = mReplacements.Candidates.find(ND);
      if (ReplacementItr != mReplacements.Candidates.end())
        mLastDeclRef = Expr;
    }
    return true;
  }

  bool TraverseStmt(Stmt *S) {
    auto StashParent{mParent};
    mParent = S;
    auto StashScope(mScope);
    if (S && (isa<CompoundStmt>(S) || isa<ForStmt>(S) || isa<WhileStmt>(S) ||
              isa<DoStmt>(S) || isa<IfStmt>(S)))
      mScope = S;
    auto Res{RecursiveASTVisitor::TraverseStmt(S)};
    mParent = StashParent;
    mScope = StashScope;
    if (!mIsInnermostMember || !mLastDeclRef || isa<DeclRefExpr>(S) ||
        isa<ParenExpr>(S) || isa<ArraySubscriptExpr>(S))
      return Res;
    if (auto *Cast{dyn_cast<ImplicitCastExpr>(S)};
        Cast && Cast->getCastKind() == CK_LValueToRValue)
      return Res;
    if (auto *Op{dyn_cast<UnaryOperator>(S)};
        Op && (Op->getOpcode() == UnaryOperatorKind::UO_Deref ||
               Op->getOpcode() == UnaryOperatorKind::UO_AddrOf))
      return Res;
    auto ND = mLastDeclRef->getFoundDecl();
    toDiag(mSrcMgr.getDiagnostics(), ND->getBeginLoc(),
           tsar::diag::warn_disable_replace_struct);
    toDiag(mSrcMgr.getDiagnostics(), cast<Expr>(S)->getExprLoc(),
           tsar::diag::note_replace_struct_arrow);
    mReplacements.Candidates.erase(ND);
    mLastDeclRef = nullptr;
    return Res;
  }

  bool TraverseMemberExpr(MemberExpr *Expr) {
    mIsInnermostMember = true;
    bool CurrInAssignment = mInAssignment;
    auto Res = RecursiveASTVisitor::TraverseMemberExpr(Expr);
    if (mIsInnermostMember && mLastDeclRef) {
      bool IsMacroArg{isImmediateMacroArg(Expr->getBeginLoc(), mSrcMgr) &&
                       isImmediateMacroArg(Expr->getEndLoc(), mSrcMgr)};
      auto ND = mLastDeclRef->getFoundDecl();
      if (!IsMacroArg && !checkMacroBoundsAndEmitDiag(
                             Expr, mReplacements.Definition->getLocation(),
                             mSrcMgr, mLangOpts)) {
        mReplacements.Candidates.erase(ND);
      } else {
        auto ReplacementItr = mReplacements.Candidates.find(ND);
        auto Itr = addToReplacement(Expr->getMemberDecl(),
                                    ReplacementItr->get<Replacement>());
        if (auto *Subscript{
                dyn_cast<ArraySubscriptExpr>(*Expr->child_begin())}) {
          auto ToReplace{Subscript->getBase()->getSourceRange()};
          if (IsMacroArg)
            ToReplace = getSpellingRange(mSrcMgr, ToReplace).getAsRange();
          Itr->RangesToReplace.emplace_back(std::move(ToReplace));
          SourceRange ToRemove{Expr->getOperatorLoc(), Expr->getEndLoc()};
          if (IsMacroArg)
            ToRemove = getSpellingRange(mSrcMgr, ToRemove).getAsRange();
          Itr->RangesToRemove.emplace_back(std::move(ToRemove));
          Itr->Flags |= Replacement::InArray;
        } else {
          auto Range(Expr->getSourceRange());
          if (IsMacroArg)
            Range = getSpellingRange(mSrcMgr, Range).getAsRange();
          Itr->RangesToReplace.emplace_back(std::move(Range));
        }
        if (CurrInAssignment)
          Itr->Flags |= Replacement::InAssignment;
      }
    }
    mIsInnermostMember = false;
    mLastDeclRef = nullptr;
    return Res;
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

  const LangOptions &mLangOpts;
  SourceManager &mSrcMgr;
  ClangGlobalInfo::RawInfo *mRawInfo;
  FunctionInfo &mReplacements;
  ReplacementMap &mReplacementInfo;

  Stmt *mParent = nullptr;
  Stmt *mScope = nullptr;
  bool mIsInnermostMember = false;
  DeclRefExpr *mLastDeclRef = nullptr;
  bool mInAssignment = true;
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
             tsar::diag::warn_disable_replace_struct);
      toDiag(mSrcMgr.getDiagnostics(), mCheckDecl->getBeginLoc(),
             tsar::diag::note_replace_struct_decl);
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
void addPragmaMetadata(FunctionInfo &FuncInfo,
    SourceManager &SrcMgr, const LangOptions &LangOpts,
    ExternalRewriter &Canvas) {
  SmallString<256> MDPragma;
  MDPragma.push_back('\n');
  getPragmaText(ClauseId::ReplaceMetadata, MDPragma);
  if (MDPragma.back() == '\n')
    MDPragma.pop_back();
  MDPragma.push_back('(');
  MDPragma += FuncInfo.Definition->getName();
  MDPragma.push_back('(');
  for (unsigned I = 0, EI = FuncInfo.Definition->getNumParams(); I < EI; ++I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I);
    if (I > 0)
      MDPragma.push_back(',');
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    if (ReplacementItr == FuncInfo.Candidates.end()) {
      MDPragma += PD->getName();
      continue;
    }
    MDPragma += "{";
    auto Itr = ReplacementItr->get<Replacement>().begin();
    auto EndItr = ReplacementItr->get<Replacement>().end();
    if (Itr != EndItr) {
      if (Itr->Flags & Replacement::InArray)
        MDPragma += "[]";
      MDPragma += ".";
      MDPragma += Itr->Member->getName();
      MDPragma += "=";
      MDPragma += Itr->Identifier;
      ++Itr;
    }
    for (auto &R : make_range(Itr, EndItr)) {
      MDPragma += ',';
      if (Itr->Flags & Replacement::InArray)
        MDPragma += "[]";
      MDPragma += ".";
      MDPragma += R.Member->getName();
      MDPragma += "=";
      MDPragma += R.Identifier;
    }
    MDPragma.push_back('}');
  }
  MDPragma.push_back(')');
  MDPragma.push_back(')');
  auto FuncBody = FuncInfo.Definition->getBody();
  assert(FuncBody && "Body of a transformed function must be available!");
  auto NextToBraceLoc = SrcMgr.getExpansionLoc(FuncBody->getBeginLoc());
  Token Tok;
  if (getRawTokenAfter(NextToBraceLoc, SrcMgr, LangOpts, Tok) ||
      SrcMgr.getPresumedLineNumber(Tok.getLocation()) ==
        SrcMgr.getPresumedLineNumber(NextToBraceLoc)) {
    MDPragma.push_back('\n');
  }
  NextToBraceLoc = NextToBraceLoc.getLocWithOffset(1);
  Canvas.InsertTextAfter(NextToBraceLoc, MDPragma);
}

template <class RewriterT>
bool replaceCall(FunctionInfo &FI, const CallExpr &Expr,
    StringRef ReplacementName, const ReplacementMetadata &Meta,
    RewriterT &Rewriter) {
  auto &SrcMgr = Rewriter.getSourceMgr();
  SmallString<256> NewCallExpr;
  NewCallExpr += ReplacementName;
  NewCallExpr += '(';
  for (unsigned I = 0, EI = Meta.Parameters.size(); I < EI; ++I) {
    if (I > 0)
      NewCallExpr += ", ";
    auto &ParamInfo = Meta.Parameters[I];
    auto ArgExpr = Expr.getArg(*ParamInfo.TargetParam);
    auto ReplacementItr = isExprInCandidates(ArgExpr, FI.Candidates);
    if (ReplacementItr == FI.Candidates.end()) {
      if (ParamInfo.Kind == ReplacementMetadata::PK_Array) {
        toDiag(SrcMgr.getDiagnostics(), Expr.getBeginLoc(),
               tsar::diag::warn_replace_call_unable);
        toDiag(SrcMgr.getDiagnostics(), ParamInfo.TargetMember->getLocation(),
               tsar::diag::note_replace_call_no_array);
        return false;
      }
      if (ParamInfo.Kind == ReplacementMetadata::PK_Pointer)
        NewCallExpr += '&';
      if (ParamInfo.TargetMember) {
        NewCallExpr += '(';
        NewCallExpr += Rewriter.getRewrittenText(
          SrcMgr.getExpansionRange(ArgExpr->getSourceRange()).getAsRange());
        NewCallExpr += ')';
        NewCallExpr += "->";
        NewCallExpr += ParamInfo.TargetMember->getName();
      } else {
        if (ParamInfo.Kind == ReplacementMetadata::PK_Pointer)
          NewCallExpr += '(';
        NewCallExpr += Rewriter.getRewrittenText(
          SrcMgr.getExpansionRange(ArgExpr->getSourceRange()).getAsRange());
        if (ParamInfo.Kind == ReplacementMetadata::PK_Pointer)
          NewCallExpr += ')';
      }
    } else {
      auto Itr = find_if(ReplacementItr->template get<Replacement>(),
                      [&ParamInfo](const Replacement &R) {
                        return R.Member == ParamInfo.TargetMember;
                      });
      assert(Itr != ReplacementItr->get<Replacement>().end() &&
             "Description of the replacement must be found!");
      if (Itr->Flags & (Replacement::InAssignment | Replacement::InArray)) {
        if (ParamInfo.Kind == ReplacementMetadata::PK_Pointer ||
            ParamInfo.Kind == ReplacementMetadata::PK_Array) {
          NewCallExpr += Itr->Identifier;
        } else {
          NewCallExpr += "*";
          NewCallExpr += Itr->Identifier;
        }
      } else if (ParamInfo.Kind == ReplacementMetadata::PK_Pointer) {
        NewCallExpr += "&";
        NewCallExpr += Itr->Identifier;
      } else {
        assert(ParamInfo.Kind != ReplacementMetadata::PK_Array &&
               "Unable to pass value as array parameter!");
        NewCallExpr += Itr->Identifier;
      }
    }
  }
  NewCallExpr += ')';
  Rewriter.ReplaceText(
      getExpansionRange(SrcMgr, Expr.getSourceRange()).getAsRange(),
      NewCallExpr);
  return true;
}

#ifdef LLVM_DEBUG
void printMetadataLog(const FunctionInfo &FuncInfo) {
  auto *FD = FuncInfo.Definition;
  auto &Sources = FuncInfo.Targets;
  if (Sources.empty())
    return;
  dbgs() << "[REPLACE]: replacement is '" << FD->getName() << "' function\n";
  for (auto &SI : Sources) {
    dbgs() << "[REPLACE]: target '" << SI.TargetDecl->getName()
           << "' for replacement";
    if (!SI.valid()) {
      dbgs() << " is not valid\n";
      continue;
    }
    if (CanonicalDeclPtr<FunctionDecl>(FD->getCanonicalDecl()) == SI.TargetDecl)
      dbgs() << " is implicit";
    dbgs() << "\n";
    const FunctionDecl *TargetDefinition = SI.TargetDecl;
    SI.TargetDecl->hasBody(TargetDefinition);
    for (unsigned I = 0, EI = SI.Parameters.size(); I < EI; ++I) {
      auto &PI = SI.Parameters[I];
      auto TargetParam = TargetDefinition->getParamDecl(*PI.TargetParam);
      dbgs() << "[REPLACE]: target parameter ";
      if (!TargetParam->getIdentifier())
        dbgs() << TargetParam->getName();
      else
        dbgs() << "<" << *PI.TargetParam << ">";
      if (PI.TargetMember)
        dbgs() << "." << PI.TargetMember->getName();
      dbgs() << "->" << I << " (";
      if (CanonicalDeclPtr<FunctionDecl>(FD->getCanonicalDecl())
            != SI.TargetDecl)
        dbgs() << FD->getParamDecl(I)->getName() << ",";
      switch (PI.Kind) {
      case ReplacementMetadata::PK_Value:
        dbgs() << "value";
        break;
      case ReplacementMetadata::PK_Pointer:
        dbgs() << "pointer";
        break;
      case ReplacementMetadata::PK_Array:
        dbgs() << "array";
        break;
      }
      dbgs() << ")\n";
    }
  }
}

void printCandidateLog(const ReplacementCandidates &Candidates, bool IsStrict) {
  dbgs() << "[REPLACE]: " << (IsStrict ? "strict" : "nostrict")
    << " replacement\n";
  for (auto &Candidate : Candidates) {
    dbgs() << "[REPLACE]: replacement candidate "
           << Candidate.get<NamedDecl>()->getName();
    if (!Candidate.get<Replacement>().empty()) {
      dbgs() << " with members: ";
      for (auto &R : Candidate.get<Replacement>()) {
        dbgs() << " " << R.Member->getName();
        if (R.Flags & Replacement::InAssignment)
          dbgs() << "(ref)";
        if (R.Flags & Replacement::InArray)
          dbgs() << "(array)";
      }
    }
    dbgs() << "\n";
  }
}

void printRequestLog(FunctionInfo &FuncInfo, const SourceManager &SrcMgr) {
  if (FuncInfo.Requests.empty())
    return;
  dbgs() << "[REPLACE]: callee replacement requests inside '"
         << FuncInfo.Definition->getName()
         << "' found\n";
  for (auto &Request : FuncInfo.Requests) {
    dbgs() << "[REPALCE]: with " << FuncInfo.Definition->getName() << " at ";
    Request.get<clang::CallExpr>()->getBeginLoc().print(dbgs(), SrcMgr);
    dbgs() << "\n";
  }
}

void printImplicitRequestLog(const FunctionInfo &FuncInfo,
    const SourceManager &SrcMgr) {
  if (FuncInfo.ImplicitRequests.empty())
    return;
  dbgs() << "[REPLACE]: callee replacement implicit requests inside '"
         << FuncInfo.Definition->getName() << "' found\n";
  for (auto &Request : FuncInfo.ImplicitRequests) {
    dbgs() << "[REPALCE]: at ";
    Request->getBeginLoc().print(dbgs(), SrcMgr);
    dbgs() << "\n";
  }
}
#endif

template<class RewriterT > bool replaceCalls(FunctionInfo &FI,
    ReplacementMap &ReplacementInfo, RewriterT &Rewriter) {
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  LLVM_DEBUG(printRequestLog(FI, SrcMgr));
  bool IsChanged = false;
  for (auto &Request : FI.Requests) {
    assert(Request.get<clang::CallExpr>() && "Call must not be null!");
    assert(Request.get<clang::FunctionDecl>() &&
           "Target function must not be null!");
    auto Meta = findRequestMetadata(Request, ReplacementInfo, SrcMgr);
    if (!Meta)
      continue;
    if (!checkMacroBoundsAndEmitDiag(Request.get<clang::CallExpr>(),
                                     FI.Definition->getLocation(), SrcMgr,
                                     LangOpts))
      continue;
    if (!replaceCall(FI, *Request.get<clang::CallExpr>(),
                     Request.get<FunctionDecl>()->getName(), *Meta, Rewriter))
      continue;
    IsChanged = true;
  }
  for (auto *Request : FI.ImplicitRequests) {
    if (!checkMacroBoundsAndEmitDiag(Request, FI.Definition->getLocation(),
                                     SrcMgr, LangOpts))
      continue;
    auto Callee = Request->getDirectCallee();
    if (!Callee)
      continue;
    auto Itr = ReplacementInfo.find(Callee);
    if (Itr == ReplacementInfo.end())
      continue;
    auto MetaItr = find_if(
        Itr->get<FunctionInfo>()->Targets, [Callee](ReplacementMetadata &Meta) {
          return Meta.TargetDecl == CanonicalDeclPtr<FunctionDecl>(Callee);
        });
    if (MetaItr == Itr->get<FunctionInfo>()->Targets.end())
      continue;
    assert(!Itr->get<FunctionInfo>()->ReplacmenetName.empty() &&
      "Name of the function clone must not be null!");
    if (!replaceCall(FI, *Request, Itr->get<FunctionInfo>()->ReplacmenetName,
                     *MetaItr, Rewriter))
      continue;
    IsChanged = true;
  }
  return IsChanged;
}

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

  void releaseMemory() override {
    mReplacementInfo.clear();
    mTfmCtx = nullptr;
    mGlobalInfo = nullptr;
    mRawInfo = nullptr;
  }

private:
  void addSuffix(const Twine &Prefix, SmallVectorImpl<char> &Out) {
    for (unsigned Count = 0;
      mRawInfo->Identifiers.count((Prefix + Twine(Count)).toStringRef(Out));
      ++Count, Out.clear());
    mRawInfo->Identifiers.insert(StringRef(Out.data(), Out.size()));
  }

  FunctionInfo * tieCallGraphNode(CallGraphNode *CGN) {
    if (!CGN->getDecl() || !isa<FunctionDecl>(CGN->getDecl()))
      return nullptr;
    auto InfoItr = mReplacementInfo.find(CGN->getDecl()->getAsFunction());
    if (InfoItr == mReplacementInfo.end())
      return nullptr;
    return InfoItr->get<FunctionInfo>().get();
  }

  /// Collect replacement candidates for functions in a specified strongly
  /// connected component in a call graph.
  void collectCandidatesIn(scc_iterator<CallGraph *> &SCC,
                           const ASTImportInfo &ImportInfo);

  /// Check accesses to replacement candidates inside a specified function.
  ///
  /// Remove replacement candidate if it cannot be replaced.
  void sanitizeCandidates(FunctionInfo &FuncInfo);

  /// Update list of members which should become parameters in a new function
  /// according to accesses in callees.
  ///
  /// If parameter has a structure type and it should be replaced, some its
  /// members can be implicitly accessed in callees while these members are not
  /// accessed in a function explicitly. This function collects these members
  /// for further replacement.
  void fillReplacementMembersFromCallees(scc_iterator<CallGraph *> &SCC);

  /// Check replacement candidates which are passed to calls.
  /// Remove replacement candidates if it cannot be replaced in callee.
  void sanitizeCandidatesInCalls(scc_iterator<CallGraph *> &SCC);

  StringRef buildMemberDeclaration(VarDecl *VD, char Separator, Replacement &R,
                                   std::string &Context,
                                   SmallVectorImpl<char> &DeclString);

  SmallString<128>
  buildMemberDeclaration(char Separator, std::string &Context,
                         ReplacementCandidates::value_type &Candidate);

  void buildDeclarations(FunctionInfo &FuncInfo);
  void buildDeclarations(scc_iterator<CallGraph *> &SCC) {
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      buildDeclarations(*FuncInfo);
    }
  }

  bool insertNewFunction(FunctionInfo &FuncInfo,
    SmallVectorImpl<ExternalRewriter> &Canvas);
  bool insertNewFunctions(scc_iterator<CallGraph *> &SCC) {
    SmallVector<ExternalRewriter, 1> Canvas;
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      if (!insertNewFunction(*FuncInfo, Canvas))
        return false;
    }
    // Update sources.
    auto &Rewriter = mTfmCtx->getRewriter();
    auto &SrcMgr = Rewriter.getSourceMgr();
    auto &LangOpts = Rewriter.getLangOpts();
    auto CanvasItr = Canvas.begin();
    for (auto &CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      auto OriginDefStart{Lexer::getSourceText(
          CharSourceRange::getTokenRange(FuncInfo->Definition->getBeginLoc(),
                                         FuncInfo->Definition->getLocation()),
          SrcMgr, LangOpts)};
      StringRef OriginDefParams;
      if (FuncInfo->Definition->getNumParams() > 0)
        OriginDefParams = Lexer::getSourceText(
            CharSourceRange::getTokenRange(
                FuncInfo->Definition->getParamDecl(0)->getBeginLoc(),
                FuncInfo->Definition
                    ->getParamDecl(FuncInfo->Definition->getNumParams() - 1)
                    ->getEndLoc()),
            SrcMgr, LangOpts);
      auto LocToInsert =
          SrcMgr.getExpansionLoc(FuncInfo->Definition->getEndLoc());
      Rewriter.InsertTextAfterToken(LocToInsert,
        ("\n\n/* Replacement for " +
          OriginDefStart + "(" + OriginDefParams + ") */\n").str());
      Rewriter.InsertTextAfterToken(LocToInsert, CanvasItr->getBuffer());
    }
    return true;
  }

  ClangTransformationContext *mTfmCtx = nullptr;
  ClangGlobalInfo::RawInfo *mRawInfo = nullptr;
  const GlobalInfoExtractor *mGlobalInfo = nullptr;
  ReplacementMap mReplacementInfo;
};
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

void ClangStructureReplacementPass::collectCandidatesIn(
    scc_iterator<CallGraph *> &SCC, const ASTImportInfo &ImportInfo) {
  std::vector<std::pair<FunctionDecl *, CallList>> Calls;
  LLVM_DEBUG(dbgs() << "[REPLACE]: process functions in SCC\n");
  for (auto *CGN : *SCC) {
    if (!CGN->getDecl() || !isa<FunctionDecl>(CGN->getDecl()))
      continue;
    const auto *Definition =
        cast<FunctionDecl>(CGN->getDecl()->getCanonicalDecl());
    if (!Definition->hasBody(Definition))
      continue;
    LLVM_DEBUG(dbgs() << "[REPLACE]: process '" << Definition->getName()
                      << "'\n");
    Calls.emplace_back(const_cast<FunctionDecl *>(Definition), CallList());
    ReplacementCollector Collector(*mTfmCtx, ImportInfo, mReplacementInfo,
                                   Calls.back().second);
    Collector.TraverseDecl(const_cast<FunctionDecl *>(Definition));
  }
  bool IsChanged = false;
  do {
    IsChanged = false;
    for (auto &CallerToCalls : Calls) {
      auto *Caller = CallerToCalls.first;
      auto CallerItr = mReplacementInfo.find(Caller);
      if (CallerItr == mReplacementInfo.end() ||
          !CallerItr->get<FunctionInfo>()->hasCandidates())
        continue;
      auto &CallerFN = *CallerItr->get<FunctionInfo>();
      for (auto *Call : CallerToCalls.second) {
        if (CallerFN.Requests.count(Call))
          continue;
        const auto *CalleeDefinition = Call->getDirectCallee();
        if (!CalleeDefinition)
          continue;
        CalleeDefinition = CalleeDefinition->getFirstDecl();
        if (!CalleeDefinition->hasBody(CalleeDefinition))
          continue;
        auto *Callee = const_cast<FunctionDecl *>(CalleeDefinition);
        auto CalleeItr = mReplacementInfo.find(Callee);
        for (unsigned I = 0, EI = Call->getNumArgs(); I < EI; ++I) {
          auto Itr = (isExprInCandidates(Call->getArg(I), CallerFN.Candidates));
          if (Itr == CallerFN.Candidates.end())
            continue;
          CallerFN.ImplicitRequests.insert(Call);
          if (CalleeItr == mReplacementInfo.end()) {
            CalleeItr =
                mReplacementInfo
                    .try_emplace(Callee, std::make_unique<FunctionInfo>(Callee))
                    .first;
            CalleeItr->get<FunctionInfo>()->Strict = CallerFN.Strict;
            LLVM_DEBUG(dbgs()
                       << "[REPLACE]: add implicit "
                       << (CalleeItr->get<FunctionInfo>()->Strict ? "strict"
                                                                  : "nostrict")
                       << " replacement for '"
                       << CalleeItr->get<FunctionDecl>()->getName() << "'\n");
          }
          if (CalleeItr->get<FunctionInfo>()
                  ->Candidates.try_emplace(Callee->getParamDecl(I))
                  .second) {
            IsChanged = true;
            CalleeItr->get<FunctionInfo>()->Strict |= CallerFN.Strict;
          }
        }
      }
    }
  } while (IsChanged && SCC.hasCycle());
}

void ClangStructureReplacementPass::sanitizeCandidates(FunctionInfo &FuncInfo) {
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  auto &LangOpts = mTfmCtx->getContext().getLangOpts();
  // Check general preconditions.
  auto FuncRange =
      SrcMgr.getExpansionRange(FuncInfo.Definition->getSourceRange());
  if (!SrcMgr.isWrittenInSameFile(FuncRange.getBegin(), FuncRange.getEnd())) {
    FuncInfo.Candidates.clear();
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocation(),
      tsar::diag::warn_disable_replace_struct);
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getBeginLoc(),
      tsar::diag::note_source_range_not_single_file);
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getEndLoc(),
      tsar::diag::note_end_location);
    return;
  }
  if (SrcMgr.getFileCharacteristic(FuncInfo.Definition->getBeginLoc()) !=
    SrcMgr::C_User) {
    FuncInfo.Candidates.clear();
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocation(),
      tsar::diag::warn_disable_replace_struct_system);
    return;
  }
  bool HasMacro = false;
  auto diagMacro = [&FuncInfo, &SrcMgr, &HasMacro](SourceLocation Loc) {
    if (!HasMacro) {
      HasMacro = true;
      toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocation(),
             tsar::diag::warn_disable_replace_struct);
      toDiag(SrcMgr.getDiagnostics(), Loc,
             tsar::diag::note_replace_struct_macro_prevent);
    }
  };
  if (FuncInfo.Definition->getLocation().isMacroID()) {
    diagMacro(FuncInfo.Definition->getLocation());
    FuncInfo.Candidates.clear();
    return;
  }
  auto *Body = FuncInfo.Definition->getBody();
  if (Body->getBeginLoc().isMacroID()) {
    diagMacro(Body->getBeginLoc());
    FuncInfo.Candidates.clear();
    return;
  }
  if (FuncInfo.Strict) {
    for_each_macro(FuncInfo.Definition, SrcMgr, LangOpts, mRawInfo->Macros,
                   diagMacro);
    if (HasMacro) {
      FuncInfo.Candidates.clear();
      return;
    }
  } else {
    SmallVector<NamedDecl *, 4> ToRemove;
    for (auto &C : FuncInfo.Candidates) {
      auto *VD = C.get<NamedDecl>();
      if (!checkMacroBoundsAndEmitDiag(VD, FuncInfo.Definition->getLocation(),
                                       SrcMgr, LangOpts)) {
        ToRemove.push_back(VD);
        continue;
      }
    }
    for (auto *PD : ToRemove)
      FuncInfo.Candidates.erase(PD);
  }
  ReplacementSanitizer Verifier(*mTfmCtx, *mRawInfo, FuncInfo, mReplacementInfo);
  Verifier.TraverseDecl(FuncInfo.Definition);
  for (auto &C : FuncInfo.Candidates)
    for (auto &R : C.get<Replacement>())
      if (isa<ValueDecl>(C.get<NamedDecl>()) &&
          isa<clang::RecordType>(
              getCanonicalUnqualifiedType(cast<ValueDecl>(C.get<NamedDecl>()))))
        R.Flags &= ~Replacement::InAssignment;
      else if (isa<clang::ArrayType>(R.Member->getType()))
        R.Flags &= ~Replacement::InAssignment;
      else if (FuncInfo.Strict)
        R.Flags |= Replacement::InAssignment;
}

void ClangStructureReplacementPass::fillReplacementMembersFromCallees(
    scc_iterator<CallGraph *> &SCC) {
  auto &SrcMgr{mTfmCtx->getContext().getSourceManager()};
  bool IsChanged = false;
  do {
    IsChanged = false;
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      for (auto &R : FuncInfo->Requests) {
        auto Meta{findRequestMetadata(R, mReplacementInfo, SrcMgr)};
        if (!Meta)
          continue;
        for (auto &ParamInfo : Meta->Parameters) {
          assert(ParamInfo.TargetParam &&
                 "Target parameter must be known for a valid request!");
          if (ParamInfo.Kind == ReplacementMetadata::PK_Array) {
            auto ArgExpr{R.get<CallExpr>()->getArg(*ParamInfo.TargetParam)};
            auto ReplacementItr{
                isExprInCandidates(ArgExpr, FuncInfo->Candidates)};
            if (ReplacementItr != FuncInfo->Candidates.end()) {
              auto I{find_if(ReplacementItr->get<Replacement>(),
                             [&ParamInfo](Replacement &R) {
                               return R.Member == ParamInfo.TargetMember;
                             })};
              assert(I != ReplacementItr->get<Replacement>().end() &&
                     "Member to replace must be known for a valid request!");
              I->Flags |= Replacement::InArray;
            }
          }
        }
      }
      for (auto *Call : FuncInfo->ImplicitRequests) {
        auto *Callee = Call->getDirectCallee();
        assert(Callee &&
          "Called must be known for a valid implicit request!");
        auto CalleeItr = mReplacementInfo.find(Callee);
        if (CalleeItr == mReplacementInfo.end())
          continue;
        for (unsigned I = 0, EI = Call->getNumArgs(); I < EI; ++I) {
          auto Itr =
            isExprInCandidates(Call->getArg(I), FuncInfo->Candidates);
          if (Itr == FuncInfo->Candidates.end())
            continue;
          auto CalleeCandidateItr =
            CalleeItr->get<FunctionInfo>()->Candidates.find(
              CalleeItr->get<FunctionInfo>()->Definition->getParamDecl(I));
          if (CalleeCandidateItr ==
            CalleeItr->get<FunctionInfo>()->Candidates.end())
            continue;
          for (auto &CalleeR : CalleeCandidateItr->get<Replacement>()) {
            auto CallerRItr = find_if(Itr->get<Replacement>(),
              [&CalleeR](const Replacement &R) {
                return R.Member == CalleeR.Member;
              });
            if (CallerRItr != Itr->get<Replacement>().end()) {
              if (!(isa<ValueDecl>(Itr->get<NamedDecl>()) &&
                    isa<clang::RecordType>(getCanonicalUnqualifiedType(
                        cast<ValueDecl>(Itr->get<NamedDecl>()))))) {
                if (!(CallerRItr->Flags & Replacement::InAssignment) &&
                    CalleeR.Flags & Replacement::InAssignment)
                  IsChanged = (CallerRItr->Flags |= Replacement::InAssignment);
                if (!(CallerRItr->Flags & Replacement::InArray) &&
                    CalleeR.Flags & Replacement::InArray)
                  IsChanged = (CallerRItr->Flags |= Replacement::InArray);
              }
            } else {
              Itr->get<Replacement>().emplace_back(CalleeR.Member);
              Itr->get<Replacement>().back().Flags = CalleeR.Flags;
              if (isa<ValueDecl>(Itr->get<NamedDecl>()) &&
                  isa<clang::RecordType>(getCanonicalUnqualifiedType(
                      cast<ValueDecl>(Itr->get<NamedDecl>()))))
                Itr->get<Replacement>().back().Flags &=
                    ~Replacement::InAssignment;
              IsChanged = true;
            }
          }
        }
      }
    }
  } while (IsChanged && SCC.hasCycle());
}

void ClangStructureReplacementPass::sanitizeCandidatesInCalls(
    scc_iterator<CallGraph *> &SCC){
  auto &SrcMgr{mTfmCtx->getContext().getSourceManager()};
  auto &LangOpts{mTfmCtx->getContext().getLangOpts()};
  bool IsChanged = false;
  do {
    IsChanged = false;
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      for (auto &R : FuncInfo->Requests) {
        auto *Callee{R.get<CallExpr>()->getDirectCallee()};
        assert(Callee && "Called must be known for a valid implicit request!");
        auto CalleeItr{mReplacementInfo.find(Callee)};
        if (CalleeItr == mReplacementInfo.end())
          continue;
        for (unsigned I = 0, EI = R.get<CallExpr>()->getNumArgs(); I < EI;
             ++I) {
          auto Itr{isExprInCandidates(R.get<CallExpr>()->getArg(I),
                                      FuncInfo->Candidates)};
          if (Itr != FuncInfo->Candidates.end())
            continue;
          if (auto CalleeCandidateItr{
                  CalleeItr->get<FunctionInfo>()->Candidates.find(
                      CalleeItr->get<FunctionInfo>()->Definition->getParamDecl(
                          I))};
              CalleeCandidateItr !=
              CalleeItr->get<FunctionInfo>()->Candidates.end()) {
            if (auto ToArrayItr{find_if(
                    CalleeCandidateItr->get<Replacement>(),
                    [](auto &R) { return R.Flags & Replacement::InArray; })};
                ToArrayItr != CalleeCandidateItr->get<Replacement>().end()) {
              toDiag(SrcMgr.getDiagnostics(), R.get<CallExpr>()->getBeginLoc(),
                     tsar::diag::warn_replace_call_unable);
              toDiag(SrcMgr.getDiagnostics(), ToArrayItr->Member->getLocation(),
                     tsar::diag::note_replace_call_no_array);
              IsChanged = true;
            }
          }
        }
      }
      SmallVector<CallExpr *, 8> ImplicitRequestToRemove;
      for (auto *Call : FuncInfo->ImplicitRequests) {
        auto *Callee = Call->getDirectCallee();
        assert(Callee &&
          "Called must be known for a valid implicit request!");
        auto CalleeItr = mReplacementInfo.find(Callee);
        bool HasCandidatesInArgs = false;
        for (unsigned I = 0, EI = Call->getNumArgs(); I < EI; ++I) {
          auto Itr =
            (isExprInCandidates(Call->getArg(I), FuncInfo->Candidates));
          if (Itr == FuncInfo->Candidates.end()) {
            if (CalleeItr == mReplacementInfo.end())
              continue;
            // Do not replace a call if at least one formal parameter must be
            // replaced with an array and actual parameter cannot be replaced.
            // In this case we disable replacement of all actual parameters
            // also.
            if (auto CalleeCandidateItr{
                    CalleeItr->get<FunctionInfo>()->Candidates.find(
                        CalleeItr->get<FunctionInfo>()
                            ->Definition->getParamDecl(I))};
                CalleeCandidateItr !=
                CalleeItr->get<FunctionInfo>()->Candidates.end()) {
              if (auto ToArrayItr{find_if(
                      CalleeCandidateItr->get<Replacement>(),
                      [](auto &R) { return R.Flags & Replacement::InArray; })};
                  ToArrayItr != CalleeCandidateItr->get<Replacement>().end()) {
                toDiag(SrcMgr.getDiagnostics(), Call->getBeginLoc(),
                       tsar::diag::warn_replace_call_unable);
                toDiag(SrcMgr.getDiagnostics(),
                       ToArrayItr->Member->getLocation(),
                       tsar::diag::note_replace_call_no_array);
                for_each(
                    Call->arguments(), [Call, &SrcMgr, FuncInfo](auto *Arg) {
                      auto Itr{isExprInCandidates(Arg, FuncInfo->Candidates)};
                      if (Itr == FuncInfo->Candidates.end())
                        return;
                      toDiag(SrcMgr.getDiagnostics(),
                             Itr->template get<NamedDecl>()->getLocation(),
                             tsar::diag::warn_disable_replace_struct);
                      toDiag(SrcMgr.getDiagnostics(), Arg->getBeginLoc(),
                             tsar::diag::note_replace_struct_arrow);
                      FuncInfo->Candidates.erase(Itr);
                    });
                HasCandidatesInArgs = false;
                IsChanged = true;
                break;
              }
            }
          } else if (CalleeItr == mReplacementInfo.end()) {
            toDiag(SrcMgr.getDiagnostics(),
                   Itr->get<NamedDecl>()->getLocation(),
                   tsar::diag::warn_disable_replace_struct);
            toDiag(SrcMgr.getDiagnostics(), Call->getArg(I)->getBeginLoc(),
              tsar::diag::note_replace_struct_arrow);
            FuncInfo->Candidates.erase(Itr);
            IsChanged = true;
          } else {
            if (!CalleeItr->get<FunctionInfo>()->Candidates.count(
                    CalleeItr->get<FunctionInfo>()->Definition->getParamDecl(
                        I))) {
              toDiag(SrcMgr.getDiagnostics(),
                     Itr->get<NamedDecl>()->getLocation(),
                     tsar::diag::warn_disable_replace_struct);
              toDiag(SrcMgr.getDiagnostics(), Call->getArg(I)->getBeginLoc(),
                     tsar::diag::note_replace_struct_arrow);
              FuncInfo->Candidates.erase(Itr);
              IsChanged = true;
            } else {
              HasCandidatesInArgs = true;
            }
          }
        }
        if (!HasCandidatesInArgs)
          ImplicitRequestToRemove.push_back(Call);
      }
      for (auto *Call : ImplicitRequestToRemove)
        FuncInfo->ImplicitRequests.erase(Call);
    }
  } while (IsChanged);
}


static Optional<std::string> buildParameterType(const Replacement &R) {
  auto ParamType{R.Member->getType().getAsString()};
  if (!(R.Flags & (Replacement::InArray | Replacement::InAssignment)))
    return ParamType;
  if (auto *ATy{dyn_cast<clang::ArrayType>(R.Member->getType())}) {
    auto ETy{ATy->getElementType()};
    while (auto *T{dyn_cast<clang::ArrayType>(ETy)})
      ETy = T->getElementType();
    auto ElemType{ETy.getAsString()};
    if (ParamType.size() <= ElemType.size() ||
        ParamType.substr(0, ElemType.size()) != ElemType)
      return None;
    if (isa<clang::ConstantArrayType>(R.Member->getType())) {
      ParamType = ElemType + "(*)" + ParamType.substr(ElemType.size());
    } else if (isa<clang::IncompleteArrayType>(R.Member->getType())) {
      StringRef Suffix{ParamType.data() + ElemType.size(),
                       ParamType.size() - ElemType.size()};
      Suffix = Suffix.trim();
      if (!Suffix.startswith("[]"))
        return None;
      ParamType = ElemType + "(**)" + Suffix.substr(2).str();
    } else {
      return None;
    }
  } else if (auto PTy{dyn_cast<clang::PointerType>(R.Member->getType())};
             PTy && isa<clang::ParenType>(PTy->getPointeeType())) {
    auto ParenPos{ParamType.find_first_of('(')};
    if (ParenPos == std::string::npos)
      return None;
    ParamType.insert(ParenPos + 1, "*");
  } else {
    ParamType += "*";
  }
  return ParamType;
}

inline StringRef ClangStructureReplacementPass::buildMemberDeclaration(
    VarDecl *VD, char Separator, Replacement &R,
    std::string &Context, SmallVectorImpl<char> &DeclString) {
  auto &SrcMgr{mTfmCtx->getContext().getSourceManager()};
  TypeSearch TS(VD, R.Member, SrcMgr, *mGlobalInfo);
  TS.TraverseDecl(R.Member);
  if (!TS.isOk())
    return StringRef{};
  addSuffix(VD->getName() + "_" + R.Member->getName(), R.Identifier);
  auto ParamType{buildParameterType(R)};
  if (!ParamType)
    return StringRef{};
  StringMap<std::string> Replacements;
  auto Tokens{
      buildDeclStringRef(*ParamType, R.Identifier, Context, Replacements)};
  if (Tokens.empty())
    return StringRef{};
  if (!DeclString.empty())
    DeclString.push_back(Separator);
  auto Size{DeclString.size()};
  join(Tokens.begin(), Tokens.end(), " ", DeclString);
  Context += StringRef(DeclString.data() + Size, DeclString.size() - Size);
  Context += ";";
  return StringRef{DeclString.data() + Size, DeclString.size() - Size};
}

inline SmallString<128> ClangStructureReplacementPass::buildMemberDeclaration(
    char Separator, std::string &Context,
    ReplacementCandidates::value_type &Candidate) {
  auto &SrcMgr{mTfmCtx->getContext().getSourceManager()};
  SmallString<128> NewDecls;
  auto StashContextSize = Context.size();
  for (auto &R : Candidate.get<Replacement>()) {
    auto NewDecl{
        buildMemberDeclaration(cast<VarDecl>(Candidate.get<NamedDecl>()),
                               Separator, R, Context, NewDecls)};
    if (NewDecl.empty()) {
      Context.resize(StashContextSize);
      NewDecls.clear();
      toDiag(SrcMgr.getDiagnostics(), Candidate.get<NamedDecl>()->getLocation(),
             tsar::diag::warn_disable_replace_struct);
      toDiag(SrcMgr.getDiagnostics(), R.Member->getBeginLoc(),
             tsar::diag::note_replace_struct_decl);
      break;
    }
    LLVM_DEBUG(dbgs() << "[REPLACE]: replacement for "
                      << Candidate.get<NamedDecl>()->getName()
                      << (isa<ParmVarDecl>(Candidate.get<NamedDecl>())
                              ? " parameter: "
                              : " variable: ")
                      << NewDecl << "\n");
  }
  return NewDecls;
}

void ClangStructureReplacementPass::buildDeclarations(FunctionInfo &FuncInfo) {
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  auto &LangOpts = mTfmCtx->getContext().getLangOpts();
  // Look up for declaration of types of parameters.
  auto &FT = getAnalysis<ClangIncludeTreePass>().getFileTree();
  std::string Context;
  // Add all macros to context to ensure that types of new parameters won't
  // be overridden after preprocessing.
  for (auto &M : mRawInfo->Macros) {
    Context += "#define ";
    Context += M.first();
    Context += "\n";
  }
  auto *OFD = mGlobalInfo->findOutermostDecl(FuncInfo.Definition);
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
  for (unsigned I = 0, EI = FuncInfo.Definition->getNumParams(); I < EI; ++I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I);
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    if (ReplacementItr == FuncInfo.Candidates.end() ||
        ReplacementItr->get<Replacement>().empty())
      continue;
    auto NewParams{buildMemberDeclaration(',', Context, *ReplacementItr)};
    if (!NewParams.empty())
      ReplacementItr->get<std::string>() = std::string(NewParams);
    else
      FuncInfo.Candidates.erase(ReplacementItr);
  }
  // Replace aggregate variables with separate variables.
  SmallVector<NamedDecl *, 8> ToRemove;
  for (auto &Candidate : FuncInfo.Candidates) {
    if (isa<ParmVarDecl>(Candidate.get<NamedDecl>()))
      continue;
    if (Candidate.get<Replacement>().empty())
      continue;
    auto NewDecls{buildMemberDeclaration(';', Context, Candidate)};
    if (!NewDecls.empty())
      Candidate.get<std::string>() = (NewDecls + ";").str();
    else
      ToRemove.push_back(Candidate.get<NamedDecl>());
  }
  for_each(ToRemove, [&FuncInfo](auto *ND) { FuncInfo.Candidates.erase(ND); });
}

bool ClangStructureReplacementPass::insertNewFunction(
    FunctionInfo &FuncInfo, SmallVectorImpl<ExternalRewriter> &CanvasList) {
  auto &Rewriter = mTfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  LLVM_DEBUG(printCandidateLog(FuncInfo.Candidates, FuncInfo.Strict));
  LLVM_DEBUG(printRequestLog(FuncInfo, SrcMgr));
  LLVM_DEBUG(printImplicitRequestLog(FuncInfo, SrcMgr));
  CanvasList.emplace_back(
      tsar::getExpansionRange(SrcMgr, FuncInfo.Definition->getSourceRange())
          .getAsRange(),
      SrcMgr, LangOpts);
  auto &Canvas = CanvasList.back();
  // Build unique name for a new function.
  addSuffix(FuncInfo.Definition->getName() + "_spf", FuncInfo.ReplacmenetName);
  SourceRange NameRange(
      SrcMgr.getExpansionLoc(FuncInfo.Definition->getLocation()));
  NameRange.setEnd(NameRange.getBegin().getLocWithOffset(
      FuncInfo.Definition->getName().size() - 1));
  Canvas.ReplaceText(NameRange, FuncInfo.ReplacmenetName);
  // Replace accesses to variables.
  auto replaceAccesses = [&SrcMgr, &Canvas](auto ReplacementItr) {
    for (auto &R : ReplacementItr->template get<Replacement>()) {
      for (auto Range : R.RangesToReplace) {
        SmallString<64> Tmp;
        auto AccessString = (R.Flags & Replacement::InAssignment &&
                             !(R.Flags & Replacement::InArray))
                                ? ("(*" + R.Identifier + ")").toStringRef(Tmp)
                                : StringRef(R.Identifier);
        Canvas.ReplaceText(getExpansionRange(SrcMgr, Range).getAsRange(),
                           AccessString);
      }
      for (auto Range : R.RangesToRemove)
        Canvas.RemoveText(getExpansionRange(SrcMgr, Range).getAsRange());
    }
  };
  // Replace aggregate parameters with separate variables.
  bool TheLastParam = true;
  for (unsigned I = FuncInfo.Definition->getNumParams(); I > 0; --I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I - 1);
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    SourceLocation EndLoc = PD->getEndLoc();
    if (TheLastParam || (ReplacementItr != FuncInfo.Candidates.end() &&
                         ReplacementItr->get<Replacement>().empty())) {
      Token CommaTok;
      if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
                           CommaTok)) {
        toDiag(SrcMgr.getDiagnostics(), PD->getEndLoc(),
               tsar::diag::warn_transform_internal);
        return false;
      }
      if (CommaTok.is(tok::comma))
        EndLoc = CommaTok.getLocation();
    }
    if (ReplacementItr == FuncInfo.Candidates.end()) {
      // Remove comma after the current parameter if it becomes the last one.
      if (EndLoc != PD->getEndLoc())
        Canvas.RemoveText(
            CharSourceRange::getTokenRange(SrcMgr.getExpansionLoc(EndLoc)));
      TheLastParam = false;
      continue;
    }
    // We also remove an unused parameter if it is mentioned in replace clause.
    if (ReplacementItr->get<Replacement>().empty()) {
      Canvas.RemoveText(CharSourceRange::getTokenRange(
                            SrcMgr.getExpansionLoc(PD->getBeginLoc()),
                            SrcMgr.getExpansionLoc(EndLoc))
                            .getAsRange());
      toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
             tsar::diag::remark_replace_struct);
      toDiag(SrcMgr.getDiagnostics(), PD->getBeginLoc(),
             tsar::diag::remark_remove_de_decl);
      // Do not update TheLastParam variable. If the current parameter is the
      // last in the list and if it is removed than the previous parameter
      // in the list become the last one.
      continue;
    }
    TheLastParam = false;
    auto ReplaceRange = CharSourceRange::getTokenRange(
                            SrcMgr.getExpansionLoc(PD->getBeginLoc()),
                            SrcMgr.getExpansionLoc(EndLoc))
                            .getAsRange();
    Canvas.ReplaceText(ReplaceRange, ReplacementItr->get<std::string>());
    replaceAccesses(ReplacementItr);
  }
  for (auto &DS : FuncInfo.DeclarationStmts) {
    Decl *NotToReplace{nullptr};
    SmallVector<Decl *, 4> Unused, Candidates;
    for (auto *D : DS->getDeclGroup()) {
      auto ReplacementItr{FuncInfo.Candidates.find(cast<NamedDecl>(D))};
      if (ReplacementItr == FuncInfo.Candidates.end()) {
        NotToReplace = D;
        continue;
      }
      Candidates.push_back(D);
      if (!ReplacementItr->get<Replacement>().empty()) {
        if (DS->getEndLoc().isMacroID())
          Canvas.InsertTextAfterToken(SrcMgr.getExpansionLoc(DS->getEndLoc()),
                                      " ");
        Canvas.InsertTextAfterToken(SrcMgr.getExpansionLoc(DS->getEndLoc()),
                                    ReplacementItr->get<std::string>());
        replaceAccesses(ReplacementItr);
      } else {
        Unused.push_back(D);
      }
    }
    if (!NotToReplace) {
      if (!checkMacroBoundsAndEmitDiag(DS, FuncInfo.Definition->getLocation(),
                                       SrcMgr, LangOpts)) {
        for (auto *D : Candidates) {
          toDiag(SrcMgr.getDiagnostics(), D->getLocation(),
                 tsar::diag::remark_replace_struct);
          toDiag(SrcMgr.getDiagnostics(), D->getLocation(),
                 tsar::diag::note_replace_struct_de_decl);
        }
      } else {
        Canvas.RemoveText(SrcMgr.getExpansionRange(DS->getSourceRange()));
        for (auto *D : Unused) {
          toDiag(SrcMgr.getDiagnostics(), D->getLocation(),
                 tsar::diag::remark_replace_struct);
          toDiag(SrcMgr.getDiagnostics(), D->getLocation(),
                 tsar::diag::remark_remove_de_decl);
        }
      }
    } else {
      toDiag(SrcMgr.getDiagnostics(), DS->getBeginLoc(),
             tsar::diag::remark_replace_struct);
      toDiag(SrcMgr.getDiagnostics(), NotToReplace->getLocation(),
             tsar::diag::note_de_multiple_prevent);
      for (auto *D : Candidates) {
        toDiag(SrcMgr.getDiagnostics(), D->getLocation(),
               tsar::diag::remark_replace_struct);
        toDiag(SrcMgr.getDiagnostics(), D->getLocation(),
               tsar::diag::note_replace_struct_de_decl);
      }
    }
  }
  // Build implicit metadata.
  FuncInfo.Targets.emplace_back();
  auto &FuncMeta = FuncInfo.Targets.back();
  FuncMeta.TargetDecl = FuncInfo.Definition;
  for (unsigned I = 0, EI = FuncInfo.Definition->getNumParams(); I < EI; ++I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I);
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    if (ReplacementItr == FuncInfo.Candidates.end()) {
      FuncMeta.Parameters.emplace_back();
      FuncMeta.Parameters.back().TargetParam = I;
      FuncMeta.Parameters.back().Kind = ReplacementMetadata::PK_Value;
    } else {
      for (auto &R : ReplacementItr->get<Replacement>()) {
        FuncMeta.Parameters.emplace_back();
        FuncMeta.Parameters.back().TargetParam = I;
        FuncMeta.Parameters.back().TargetMember = cast<FieldDecl>(R.Member);
        if (R.Flags & Replacement::InArray)
          FuncMeta.Parameters.back().Kind = ReplacementMetadata::PK_Array;
        else if (R.Flags & Replacement::InAssignment)
          FuncMeta.Parameters.back().Kind = ReplacementMetadata::PK_Pointer;
        else
          FuncMeta.Parameters.back().Kind = ReplacementMetadata::PK_Value;
      }
    }
  }
  LLVM_DEBUG(printMetadataLog(FuncInfo));
  replaceCalls(FuncInfo, mReplacementInfo, Canvas);
  // Remove pragmas from the original function and its clone if replacement
  // is still possible.
  Rewriter::RewriteOptions RemoveEmptyLine;
  /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
  /// set to true then removing (in RewriterBuffer) works incorrect.
  RemoveEmptyLine.RemoveLineIfEmpty = false;
  for (auto SR : FuncInfo.ToRemoveTransform) {
    Rewriter.RemoveText(SR, RemoveEmptyLine);
    Canvas.RemoveText(SR, true);
  }
  for (auto SR : FuncInfo.ToRemoveMetadata)
    Canvas.RemoveText(SR, true);
  for (auto SR : FuncInfo.ToRemoveClone)
    Canvas.RemoveText(SR, true);
  addPragmaMetadata(FuncInfo, SrcMgr, LangOpts, Canvas);
  return true;
}

bool ClangStructureReplacementPass::runOnModule(llvm::Module &M) {
  auto *CUs{M.getNamedMetadata("llvm.dbg.cu")};
  auto CXXCUItr{find_if(CUs->operands(), [](auto *MD) {
    auto *CU{dyn_cast<DICompileUnit>(MD)};
    return CU &&
           (isC(CU->getSourceLanguage()) || isCXX(CU->getSourceLanguage()));
  })};
  if (CXXCUItr == CUs->op_end()) {
    M.getContext().emitError(
        "cannot transform sources"
        ": transformation of C/C++ sources are only possible now");
    return false;
  }
  if (CUs->getNumOperands() != 1) {
    M.getContext().emitError("cannot transform sources"
      ": inline expansion are only implemented for a single source file now");
    return false;
  }
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  mTfmCtx = TfmInfo ? cast_or_null<ClangTransformationContext>(
                          TfmInfo->getContext(cast<DICompileUnit>(**CXXCUItr)))
                    : nullptr;
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto *GI{GIP.getGlobalInfo(mTfmCtx)};
  assert(GI && "Global information must not be null!");
  mRawInfo = &GI->RI;
  mGlobalInfo = &GI->GIE;
  clang::CallGraph CG;
  CG.TraverseDecl(mTfmCtx->getContext().getTranslationUnitDecl());
  std::vector<scc_iterator<CallGraph *>> Postorder;
  for (auto I = scc_iterator<CallGraph *>::begin(&CG); !I.isAtEnd(); ++I)
    Postorder.push_back(I);
  LLVM_DEBUG(dbgs() << "[REPLACE]: number of SCCs " << Postorder.size() << "\n");
  LLVM_DEBUG(dbgs() << "[REPLACE]: traverse call graph in reverse postorder\n");
  for (auto &SCC: llvm::reverse(Postorder))
    collectCandidatesIn(SCC, *ImportInfo);
  auto &Rewriter = mTfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  if (SrcMgr.getDiagnostics().hasErrorOccurred())
    return false;
  LLVM_DEBUG(dbgs() << "[REPLACE]: traverse call graph in postorder\n");
  for (auto &SCC : Postorder) {
    LLVM_DEBUG(dbgs() << "[REPLACE]: process functions in SCC\n");
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo)
        continue;
      LLVM_DEBUG(printMetadataLog(*FuncInfo));
      LLVM_DEBUG(printCandidateLog(FuncInfo->Candidates, FuncInfo->Strict));
      LLVM_DEBUG(printRequestLog(*FuncInfo, SrcMgr));
      LLVM_DEBUG(printImplicitRequestLog(*FuncInfo, SrcMgr));
      if (!FuncInfo->hasCandidates()) {
        if (replaceCalls(*FuncInfo, mReplacementInfo, Rewriter)) {
          Rewriter::RewriteOptions RemoveEmptyLine;
          /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
          /// set to true then removing (in RewriterBuffer) works incorrect.
          RemoveEmptyLine.RemoveLineIfEmpty = false;
          for (auto SR : FuncInfo->ToRemoveTransform)
            Rewriter.RemoveText(SR, RemoveEmptyLine);
        }
      } else {
        sanitizeCandidates(*FuncInfo);
      }
    }
    fillReplacementMembersFromCallees(SCC);
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->Strict)
        continue;
      SmallVector<NamedDecl *, 8> ToRemove;
      for (auto &Candidate : FuncInfo->Candidates) {
        if (any_of(Candidate.get<Replacement>(),
                   [](auto &R) { return R.Flags & Replacement::InArray; }))
          ToRemove.push_back(Candidate.get<NamedDecl>());
        for_each(ToRemove, [this, FuncInfo](auto *ND) {
          toDiag(mTfmCtx->getContext().getDiagnostics(), ND->getLocation(),
                 tsar::diag::warn_disable_replace_struct_array_strict);
          FuncInfo->Candidates.erase(ND);
        });
      }
    }
    buildDeclarations(SCC);
    sanitizeCandidatesInCalls(SCC);
    if (!insertNewFunctions(SCC))
      return false;
  }
  return false;
}
