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
    public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangStructureReplacementPass() : FunctionPass(ID) {
      initializeClangStructureReplacementPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

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
  ValueDecl *Member;
  std::vector<SourceRange> Ranges;
  SmallString<32> Identifier;
  bool InAssignment = false;
};

using ReplacementCandidates =
  SmallDenseMap<NamedDecl *, SmallVector<Replacement, 8>, 8>;

/// This class collects all 'replace' clauses in the code.
class ReplacementCollector : public RecursiveASTVisitor<ReplacementCollector> {
public:

  ReplacementCollector(TransformationContext &TfmCtx)
     : mTfmCtx(TfmCtx)
     , mSrcMgr(TfmCtx.getContext().getSourceManager())
     , mLangOpts(TfmCtx.getContext().getLangOpts())
  {}

  /// Return list of parameters to replace.
  ReplacementCandidates & getCandidates() noexcept {
    return mReplacementCandidates;
  }

  /// Return list of parameters to replace.
  const ReplacementCandidates & getCandidates() const noexcept {
    return mReplacementCandidates;
  }

  /// Return list of replacement-related clauses to remove.
  const ArrayRef<CharSourceRange> getClausesToRemove() const noexcept {
    return mToRemove;
  }

  /// Return true if at least one replacement candidate has been found.
  bool hasCandidates() const { return !mReplacementCandidates.empty(); }

  /// Return true if a specified reference is located in a 'replace' clause.
  bool inClause(const DeclRefExpr *DRE) const { return mMeta.count(DRE); }

  /// Return true if at least one strict (without 'nostrict' clause) replacement
  /// is specified.
  bool isStrict() const noexcept { return mStrict; }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    Pragma P(*S);
    SmallVector<Stmt *, 2> Clauses;
    if (findClause(P, ClauseId::Replace, Clauses)) {
      auto StashSize = Clauses.size();
      mStrict |= !findClause(P, ClauseId::NoStrict, Clauses);
      // Do not remove 'nostrict' clause if the directive contains other
      // clauses except 'replace'.
      if (P.clause_size() > Clauses.size())
        Clauses.resize(StashSize);
      auto IsPossible =
        pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts, mToRemove,
          PragmaFlags::IsInHeader);
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
      mInClause = true;
      Clauses.resize(StashSize);
      for (auto *C : Clauses)
        if (!RecursiveASTVisitor::TraverseStmt(C))
          return false;
      mInClause = false;
      return true;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    if (!mInClause)
      return true;
    mMeta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto PD = dyn_cast<ParmVarDecl>(ND)) {
      auto Ty = getCanonicalUnqualifiedType(PD);
      if (auto PtrTy = dyn_cast<clang::PointerType>(Ty)) {
        auto PointeeTy = PtrTy->getPointeeType().getTypePtr();
        if (auto StructTy = dyn_cast<clang::RecordType>(PointeeTy)) {
          mReplacementCandidates.try_emplace(PD);
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

private:
  TransformationContext &mTfmCtx;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;

  ReplacementCandidates mReplacementCandidates;
  SmallVector<CharSourceRange, 8> mToRemove;
  SmallPtrSet<DeclRefExpr *, 8> mMeta;
  bool mStrict = false;
  bool mInClause = false;
};

class ReplacementSanitizer : public RecursiveASTVisitor<ReplacementSanitizer> {
public:
  using ReplacementCandidates =
    SmallDenseMap<NamedDecl *, SmallVector<Replacement, 8>, 8>;

  ReplacementSanitizer(TransformationContext &TfmCtx, ReplacementCollector &RC)
     : mTfmCtx(TfmCtx)
     , mSrcMgr(mTfmCtx.getContext().getSourceManager())
     , mReplacements(RC)
  {}

  bool TraverseStmt(Stmt *S) {
    if (S && std::distance(S->child_begin(), S->child_end()) > 1) {
      LLVM_DEBUG(if (mInAssignment) dbgs()
                 << "[REPLACE]: disable assignment check\n");
      mInAssignment = false;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    mLastDeclRef = nullptr;
    if (!mIsInnermostMember && !mReplacements.inClause(Expr)) {
      auto ND = Expr->getFoundDecl();
      if (mReplacements.getCandidates().count(ND)) {
        toDiag(mSrcMgr.getDiagnostics(), ND->getLocStart(),
          diag::warn_disable_replace_struct);
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
          diag::note_replace_struct_arrow);
        mReplacements.getCandidates().erase(ND);
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
      auto ReplacementItr = mReplacements.getCandidates().find(ND);
      if (ReplacementItr != mReplacements.getCandidates().end()) {
        if (!Expr->isArrow()) {
          toDiag(mSrcMgr.getDiagnostics(), ND->getLocStart(),
            diag::warn_disable_replace_struct);
          toDiag(mSrcMgr.getDiagnostics(), Expr->getOperatorLoc(),
            diag::note_replace_struct_arrow);
          mReplacements.getCandidates().erase(ReplacementItr);
        } else {
          auto Itr = find_if(ReplacementItr->second,
           [Expr](const Replacement &R) {
              return R.Member == Expr->getMemberDecl();
            });
          if (Itr == ReplacementItr->second.end()) {
            ReplacementItr->second.emplace_back(Expr->getMemberDecl());
            Itr = ReplacementItr->second.end() - 1;
          }
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
  TransformationContext &mTfmCtx;
  SourceManager &mSrcMgr;
  ReplacementCollector &mReplacements;

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
  bool FirstReplacement = true;
  for (unsigned I = 0, EI = FuncDecl.getNumParams(); I < EI; ++I) {
    auto *PD = FuncDecl.getParamDecl(I);
    auto ReplacementItr = Candidates.find(PD);
    if (ReplacementItr == Candidates.end())
      continue;
    if (!FirstReplacement)
      MDPragma.push_back(',');
    FirstReplacement = false;
    (Twine(I + 1) + ":{").toStringRef(MDPragma);
    auto Itr = ReplacementItr->second.begin();
    auto EndItr = ReplacementItr->second.end();
    if (Itr != EndItr) {
      MDPragma += ".";
      MDPragma += Itr->Member->getName();
      MDPragma += "->";
      MDPragma += Itr->Identifier;
      ++Itr;
    }
    for (auto &R : make_range(Itr, EndItr)) {
      MDPragma += ',';
      MDPragma += ".";
      MDPragma += R.Member->getName();
      MDPragma += "->";
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

FunctionPass * llvm::createClangStructureReplacementPass() {
  return new ClangStructureReplacementPass;
}

bool ClangStructureReplacementPass::runOnFunction(Function &F) {
  auto &M = *F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto FD = TfmCtx->getDeclForMangledName(F.getName());
  if (!FD)
    return false;
  auto FuncDecl = FD->getAsFunction();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  mRawInfo = &GIP.getRawInfo();
  ReplacementCollector Collector(*TfmCtx);
  Collector.TraverseDecl(FuncDecl);
  if (!Collector.hasCandidates())
    return false;
  ReplacementSanitizer Verifier(*TfmCtx, Collector);
  Verifier.TraverseDecl(FuncDecl);
  if (!Collector.hasCandidates())
    return false;
  auto &Rewriter = TfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  // Check general preconditions.
  auto FuncRange = SrcMgr.getExpansionRange(FuncDecl->getSourceRange());
  if (!SrcMgr.isWrittenInSameFile(FuncRange.getBegin(), FuncRange.getEnd())) {
    toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocation(),
      diag::warn_disable_replace_struct);
    toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocStart(),
      diag::note_source_range_not_single_file);
    toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocEnd(),
      diag::note_end_location);
    return false;
  }
  if (SrcMgr.getFileCharacteristic(FuncDecl->getLocStart()) !=
      SrcMgr::C_User) {
    toDiag(SrcMgr.getDiagnostics(), FuncDecl->getLocation(),
      diag::warn_disable_replace_struct_system);
    return false;
  }
  if (Collector.isStrict()) {
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
      return false;
  }
  LLVM_DEBUG(
    dbgs() << "[REPLACE]: " << (Collector.isStrict() ? "strict" : "nostrict")
           << " replacement\n";
    dbgs() << "[REPLACE]: replacement candidates found";
    for (auto &Candidate: Collector.getCandidates())
      dbgs() << " " << Candidate.first->getName();
    dbgs() << "\n";
  );
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
    auto ReplacementItr = Collector.getCandidates().find(PD);
    if (ReplacementItr == Collector.getCandidates().end()) {
      TheLastParam = false;
      continue;
    }
    SmallString<128> NewParams;
    // We also remove an unused parameter if it is mentioned in replace clause.
    if (ReplacementItr->second.empty()) {
      SourceLocation EndLoc = PD->getLocEnd();
      Token CommaTok;
      if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
                            CommaTok)) {
        toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
          diag::warn_disable_replace_struct);
        toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
               diag::note_replace_struct_de_decl);
        Collector.getCandidates().erase(ReplacementItr);
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
    for (auto &R : ReplacementItr->second) {
      TypeSearch TS(PD, R.Member, SrcMgr, GIP.getGlobalInfo());
      TS.TraverseDecl(R.Member);
      if (!TS.isOk()) {
        Context.resize(StashContextSize);
        NewParams.clear();
        break;
      }
      addSuffix(PD->getName() + "_" + R.Member->getName(), R.Identifier);
      auto ParamType = (R.InAssignment || Collector.isStrict())
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
      Context +=  StringRef(NewParams.data() + Size, NewParams.size() - Size);
      Context += ";";
      LLVM_DEBUG(dbgs() << "[REPLACE]: replacement for " << I << " parameter: "
                        << StringRef(NewParams.data() + Size,
                                     NewParams.size() - Size) << "\n");
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
          Collector.getCandidates().erase(ReplacementItr);
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
      Collector.getCandidates().erase(ReplacementItr);
    }
    TheLastParam = false;
  }
  // Replace accesses to parameters.
  for (auto &ParamInfo : Collector.getCandidates()) {
    for (auto &R : ParamInfo.second) {
      for (auto Range : R.Ranges) {
        SmallString<64> Tmp;
        auto AccessString = R.InAssignment || Collector.isStrict()
                                ? ("(*" + R.Identifier + ")").toStringRef(Tmp)
                                : StringRef(R.Identifier);
        Canvas.ReplaceText(Range, AccessString);
      }
    }
  }
  if (Collector.getCandidates().empty())
    return false;
  // Remove pragmas from the original function and its clone if replacement
  // is still possible.
  Rewriter::RewriteOptions RemoveEmptyLine;
  /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
  /// set to true then removing (in RewriterBuffer) works incorrect.
  RemoveEmptyLine.RemoveLineIfEmpty = false;
  for (auto SR : Collector.getClausesToRemove()) {
    Rewriter.RemoveText(SR, RemoveEmptyLine);
    Canvas.RemoveText(SR, true);
  }
  addPragmaMetadata(
    *FuncDecl, Collector.getCandidates(), SrcMgr, LangOpts, Canvas);
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
  return false;
}
