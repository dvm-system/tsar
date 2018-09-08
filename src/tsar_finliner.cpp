//===--- tsar_finliner.cpp - Frontend Inliner (clang) -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements methods necessary for function source-level inlining.
///
//===----------------------------------------------------------------------===//


#include "tsar_finliner.h"
#include "ClangUtils.h"
#include "Diagnostic.h"
#include "GlobalInfoExtractor.h"
#include "tsar_pass_provider.h"
#include "tsar_pragma.h"
#include "SourceLocationTraverse.h"
#include "tsar_transformation.h"

#include <algorithm>
#include <numeric>
#include <regex>
#include <map>
#include <set>
#include <stack>
#include <vector>
#include <type_traits>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclLookups.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Analysis/CFG.h>
#include <clang/Format/Format.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

// FIXME: ASTImporter can break mapping node->source (VLAs, etc)

using namespace clang;
using namespace llvm;
using namespace tsar;
using namespace ::detail;

#undef DEBUG_TYPE
#define DEBUG_TYPE "function-inliner"

char FunctionInlinerImmutableStorage::ID = 0;
INITIALIZE_PASS(FunctionInlinerImmutableStorage, "function-inliner-is",
  "Function Inliner (Immutable Storage)", true, true)

template<> char FunctionInlinerImmutableWrapper::ID = 0;
INITIALIZE_PASS(FunctionInlinerImmutableWrapper, "function-inliner-iw",
  "Function Inliner (Immutable Wrapper)", true, true)

typedef FunctionPassProvider<
  TransformationEnginePass
> FunctionInlinerProvider;

INITIALIZE_PROVIDER_BEGIN(FunctionInlinerProvider, "function-inliner-provider",
  "Function Inliner Data Provider")
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PROVIDER_END(FunctionInlinerProvider, "function-inliner-provider",
  "Function Inliner Data Provider")

char FunctionInlinerPass::ID = 0;
INITIALIZE_PASS_BEGIN(FunctionInlinerPass, "function-inliner",
  "Function Inliner", false, false)
  INITIALIZE_PASS_DEPENDENCY(FunctionInlinerProvider)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(FunctionInlinerPass, "function-inliner",
  "Function Inliner", false, false)

ModulePass* createFunctionInlinerPass() {
  return new FunctionInlinerPass();
}

void FunctionInlinerPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<FunctionInlinerProvider>();
  AU.addRequired<TransformationEnginePass>();
}

inline FilenameAdjuster getFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    llvm::SmallString<128> Path = Filename;
    llvm::sys::path::replace_extension(
      Path, ".inl" + llvm::sys::path::extension(Path));
    return Path.str();
  };
}

inline bool reformat(
  clang::Rewriter& Rewriter, clang::FileID FID) {
  clang::SourceManager& SM = Rewriter.getSourceMgr();
  llvm::MemoryBuffer* Code = SM.getBuffer(FID);
  if (Code->getBufferSize() == 0)
    return false;
  unsigned int Offset = SM.getFileOffset(SM.getLocForStartOfFile(FID));
  unsigned int Length = SM.getFileOffset(SM.getLocForEndOfFile(FID)) - Offset;
  std::vector<clang::tooling::Range> Ranges({
    clang::tooling::Range(Offset, Length) });
  clang::format::FormatStyle FormatStyle
    = clang::format::getStyle("LLVM", "", "LLVM");
  clang::tooling::Replacements Replaces = clang::format::sortIncludes(
    FormatStyle, Code->getBuffer(), Ranges,
    SM.getFileEntryForID(FID)->getName());
  llvm::Expected<std::string> ChangedCode
    = clang::tooling::applyAllReplacements(Code->getBuffer(), Replaces);
  assert(bool(ChangedCode) == true && "Failed to apply replacements");
  for (const auto& R : Replaces) {
    Ranges.push_back({ R.getOffset(), R.getLength() });
  }
  clang::tooling::Replacements FormatChanges = clang::format::reformat(
    FormatStyle, ChangedCode.get(), Ranges,
    SM.getFileEntryForID(FID)->getName());
  Replaces = Replaces.merge(FormatChanges);
  clang::tooling::applyAllReplacements(Replaces, Rewriter);
  return false;
}

bool FunctionInlinerPass::runOnModule(llvm::Module& M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return false;
  }
  FunctionInlinerProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass& TEP) {
    TEP.setContext(M, TfmCtx);
  });
  auto& Context = TfmCtx->getContext();
  auto& Rewriter = TfmCtx->getRewriter();
  auto& SrcMgr = Rewriter.getSourceMgr();
  FInliner Inliner(TfmCtx);
  Inliner.HandleTranslationUnit(Context);
  /*for (Function& F : M) {
    if (F.empty())
      continue;
    auto& Provider = getAnalysis<FunctionInlinerProvider>(F);
  }*/
  TfmCtx->release(getFilenameAdjuster());
  // clang::tooling can not apply replacements over rewritten sources,
  // only over original non-modified sources
  // dump modifications and reload files to apply stylization
  clang::Rewriter Rewrite(SrcMgr, Rewriter.getLangOpts());
  for (auto I = Rewriter.buffer_begin(), E = Rewriter.buffer_end();
    I != E; ++I) {
    const clang::FileEntry* Entry = SrcMgr.getFileEntryForID(I->first);
    std::string Name = getFilenameAdjuster()(Entry->getName());
    clang::FileID FID = SrcMgr.createFileID(
      SrcMgr.getFileManager().getFile(Name),
      clang::SourceLocation(), clang::SrcMgr::C_User);
    reformat(Rewrite, FID);
  }
  if (Rewrite.overwriteChangedFiles() == false) {
    llvm::outs() << "All changes were successfully saved" << '\n';
  }
  return false;
}

namespace std {
template<>
struct less<clang::SourceRange> {
  bool operator()(clang::SourceRange LSR, clang::SourceRange RSR) {
    return LSR.getBegin() == RSR.getBegin()
      ? LSR.getEnd() < RSR.getEnd() : LSR.getBegin() < RSR.getBegin();
  }
};
}

namespace {
#ifdef DEBUG
void printLocLog(const SourceManager &SM, SourceRange R) {
  dbgs() << "[";
  R.getBegin().dump(SM);
  dbgs() << ",";
  R.getEnd().dump(SM);
  dbgs() << "]";
}

void templatesInfoLog(const FInliner::TemplateMap &Ts,
    const FInliner::TemplateInstantiationMap &TIs,
    const SourceManager &SM, const LangOptions &LangOpts) {
  auto sourceText = [&SM, &LangOpts](const Stmt *S) {
    auto SR = getExpansionRange(SM, S->getSourceRange());
    return Lexer::getSourceText(
      CharSourceRange::getTokenRange(SR), SM, LangOpts);
  };
  llvm::dbgs() << "[INLINE]: enabled templates (" <<
    std::count_if(std::begin(Ts), std::end(Ts),
      [](const std::pair<const clang::FunctionDecl*, Template> &LHS) {
        return LHS.second.isNeedToInline();
      }) << "):\n";
  for (auto &T : Ts)
    if (T.second.isNeedToInline())
      llvm::dbgs() << " '" << T.first->getName() << "'";
  llvm::dbgs() << '\n';
  llvm::dbgs() << "[INLINE]: disabled templates (" <<
    std::count_if(std::begin(Ts), std::end(Ts),
      [](const std::pair<const clang::FunctionDecl*, Template> &LHS) {
        return !LHS.second.isNeedToInline();
      }) << "):\n";
  for (auto &T : Ts)
    if (!T.second.isNeedToInline())
      llvm::dbgs() << " '" << T.first->getName() << "'";
  llvm::dbgs() << '\n';
  llvm::dbgs() << "[INLINE]: total template instantiations:\n";
  for (auto &TIs : TIs) {
    if (TIs.second.empty())
      continue;
    llvm::dbgs() << " in '" << TIs.first->getName() << "':\n";
    for (auto &TI : TIs.second) {
      llvm::dbgs() << "  '" << sourceText(TI.mCallExpr) << "' at ";
      TI.mCallExpr->getLocStart().dump(SM);
      dbgs() << "\n";
    }
  }
}
#endif
}

void FInliner::rememberMacroLoc(SourceLocation Loc) {
  if (Loc.isInvalid() || !Loc.isMacroID())
    return;
  mTs[mCurrentFD].setMacroInDecl(Loc);
  /// Find root of subtree located in macro.
  if (mStmtInMacro.isInvalid())
   mStmtInMacro = Loc;
}

bool FInliner::TraverseFunctionDecl(clang::FunctionDecl *FD) {
  // Do not visit functions without body to avoid implicitly created templates
  // after call of mTs[CurrentFD] method.
  if (!FD->isThisDeclarationADefinition())
    return true;
  mCurrentFD = FD;
  std::unique_ptr<clang::CFG> CFG = clang::CFG::buildCFG(
    nullptr, FD->getBody(), &mContext, clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + mCurrentFD->getName()).str().data());
  llvm::SmallPtrSet<clang::CFGBlock *, 8> UB;
  unreachableBlocks(*CFG, UB);
  auto &UnreachableStmts = mUnreachableStmts[mCurrentFD];
  for (auto *BB : UB)
    for (auto &I : *BB)
      if (auto CS = I.getAs<clang::CFGStmt>())
          UnreachableStmts.insert(CS->getStmt());
  auto &T = mTs.emplace(std::piecewise_construct,
    std::forward_as_tuple(mCurrentFD),
    std::forward_as_tuple(mCurrentFD)).first->second;
  if (CFG->getExit().pred_size() <= 1)
    T.setSingleReturn();
  auto Res = RecursiveASTVisitor::TraverseFunctionDecl(FD);
  mCurrentFD = nullptr;
  return Res;
}

bool FInliner::VisitReturnStmt(clang::ReturnStmt* RS) {
  mTs[mCurrentFD].addRetStmt(RS);
  return RecursiveASTVisitor::VisitReturnStmt(RS);
}

bool FInliner::VisitExpr(clang::Expr* E) {
  mExprs[mCurrentFD].insert(E);
  return RecursiveASTVisitor::VisitExpr(E);
}

bool FInliner::VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
  if (auto PVD = dyn_cast<ParmVarDecl>(DRE->getDecl()))
    mTs[mCurrentFD].addParmRef(PVD, DRE);
  auto ND = DRE->getFoundDecl();
  if (auto OD = mGIE.findOutermostDecl(ND)) {
    DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
      mCurrentFD->getName() << "' found '" << ND->getName() << "'\n");
    mTs[mCurrentFD].addForwardDecl(OD);
  }
  DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
    mCurrentFD->getName() << "' at ";
    DRE->getLocation().dump(mSourceManager);  dbgs() << "\n");
  mDeclRefLoc.insert(
    mSourceManager.getExpansionLoc(DRE->getLocation()).getRawEncoding());
  return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
}

bool FInliner::VisitDecl(Decl *D) {
  traverseSourceLocation(D,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  return RecursiveASTVisitor::VisitDecl(D);
}

bool FInliner::VisitTypeLoc(TypeLoc TL) {
  traverseSourceLocation(TL,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  return RecursiveASTVisitor::VisitTypeLoc(TL);
}

bool FInliner::VisitTagTypeLoc(TagTypeLoc TTL) {
  if (auto ND = dyn_cast_or_null<NamedDecl>(TTL.getDecl())) {
    if (auto OD = mGIE.findOutermostDecl(ND)) {
      DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
        mCurrentFD->getName() << "' found '" << ND->getName() << "'\n");
      mTs[mCurrentFD].addForwardDecl(OD);
    }
    DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
      mCurrentFD->getName() << "' at ";
      TTL.getNameLoc().dump(mSourceManager);  dbgs() << "\n");
    mDeclRefLoc.insert(
      mSourceManager.getExpansionLoc(TTL.getNameLoc()).getRawEncoding());
  }
  return RecursiveASTVisitor::VisitTagTypeLoc(TTL);
}

bool FInliner::VisitTypedefTypeLoc(TypedefTypeLoc TTL) {
  if (auto ND = dyn_cast_or_null<NamedDecl>(TTL.getTypedefNameDecl())) {
    if (auto OD = mGIE.findOutermostDecl(ND)) {
      DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
        mCurrentFD->getName() << "' found '" << ND->getName() << "'\n");
      mTs[mCurrentFD].addForwardDecl(OD);
    }
    DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
      mCurrentFD->getName() << "' at ";
      TTL.getNameLoc().dump(mSourceManager);  dbgs() << "\n");
    mDeclRefLoc.insert(
      mSourceManager.getExpansionLoc(TTL.getNameLoc()).getRawEncoding());
  }
  return RecursiveASTVisitor::VisitTypedefTypeLoc(TTL);
}

bool FInliner::TraverseStmt(clang::Stmt *S) {
  if (!S)
    return RecursiveASTVisitor::TraverseStmt(S);
  Pragma P(*S);
  if (P) {
    if (P.getDirectiveId() != DirectiveId::Transform)
      return true;
    for (auto CI = P.clause_begin(), CE = P.clause_end(); CI != CE; ++CI) {
      ClauseId Id;
      if (!getTsarClause(P.getDirectiveId(), Pragma::clause(CI).getName(), Id))
        continue;
      if (Id == ClauseId::Inline)
        mActiveClause = { *CI, true, false };
    }
    return true;
  }
  traverseSourceLocation(S,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  if (mActiveClause) {
    mScopes.push_back(mActiveClause);
    mInlineStmts[mCurrentFD].emplace_back(mActiveClause, S);
    mActiveClause = { nullptr, false, false };
  }
  mScopes.emplace_back(S);
  auto Res = RecursiveASTVisitor::TraverseStmt(S);
  mScopes.pop_back();
  if (!mScopes.empty() && mScopes.back().isClause()) {
    if (!mScopes.back().isUsed()) {
      toDiag(mSourceManager.getDiagnostics(), mScopes.back()->getLocStart(),
        diag::warn_unexpected_directive);
      toDiag(mSourceManager.getDiagnostics(), S->getLocStart(),
        diag::note_inline_no_call);
    }
    mScopes.pop_back();
  }
  // Disable clause at the end of compound statement, body of loop, etc.
  // #pragma ...
  // }
  // <stmt>, pragma should not mark <stmt>
  if (mActiveClause) {
    toDiag(mSourceManager.getDiagnostics(), mActiveClause->getLocStart(),
      diag::warn_unexpected_directive);
    mActiveClause.reset();
  }
  return Res;
}

bool FInliner::TraverseCallExpr(CallExpr *Call) {
  DEBUG(dbgs() << "[INLINE]: traverse call expression '" <<
    getSourceText(getRange(Call)) << "' at ";
    Call->getLocStart().dump(mSourceManager); dbgs() << "\n");
  auto InlineInMacro = mStmtInMacro;
  mStmtInMacro = (Call->getLocStart().isMacroID()) ? Call->getLocStart() :
    Call->getLocEnd().isMacroID() ? Call->getLocEnd() : SourceLocation();
  if (!RecursiveASTVisitor::TraverseCallExpr(Call))
    return false;
  std::swap(InlineInMacro, mStmtInMacro);
  if (mStmtInMacro.isInvalid())
    mStmtInMacro = InlineInMacro;
  assert(!mScopes.empty() && "At least one parent statement must exist!");
  auto ScopeI = mScopes.rbegin(), ScopeE = mScopes.rend();
  clang::Stmt *StmtWithCall = Call;
  auto ClauseI = mScopes.rend();
  bool InCondOp = false, InLoopCond = false, InForInc = false;
  for (; ScopeI != ScopeE; StmtWithCall = *(ScopeI++)) {
    if (ScopeI->isClause()) {
      ClauseI = ScopeI;
      break;
    }
    if (isa<ConditionalOperator>(*ScopeI)) {
      InCondOp = true;
    } else if (auto For = dyn_cast<ForStmt>(*ScopeI)) {
      // Check that #pragma is set before loop.
      if (For->getBody() != StmtWithCall && (ScopeI + 1)->isClause())
        ClauseI = ScopeI + 1;
      InLoopCond = (For->getCond() == StmtWithCall);
      InForInc = (For->getInc() == StmtWithCall);
      // In case of call inside for-loop initialization, the body of function
      // should be inserted before for-loop.
      if (For->getInit() == StmtWithCall)
        StmtWithCall = For;
      break;
    } else if (auto While = dyn_cast<WhileStmt>(*ScopeI)) {
      // Check that #pragma is set before loop.
      if (While->getBody() != StmtWithCall && (ScopeI + 1)->isClause())
        ClauseI = ScopeI + 1;
      InLoopCond = (While->getCond() == StmtWithCall);
      break;
    } else if (auto Do = dyn_cast<DoStmt>(*ScopeI)) {
      // Check that #pragma is set before loop.
      if (Do->getBody() != StmtWithCall && (ScopeI + 1)->isClause())
        ClauseI = ScopeI + 1;
      InLoopCond = (Do->getCond() == StmtWithCall);
      break;
    } else if (auto If = dyn_cast<IfStmt>(*ScopeI)) {
      // Check that #pragma is set before `if`.
      if (If->getThen() != StmtWithCall && If->getElse() != StmtWithCall &&
          (ScopeI + 1)->isClause())
        ClauseI = ScopeI + 1;
      // In case of call inside condition, the body of function
      // should be inserted before `if`.
      if (If->getCond() == StmtWithCall)
        StmtWithCall = If;
      break;
    } else if (isa<CompoundStmt>(*ScopeI) ||
               isa<CaseStmt>(*ScopeI) || isa<DefaultStmt>(*ScopeI)) {
      break;
    }
  }
  DEBUG(dbgs() << "[INLINE]: statement with call '" <<
    getSourceText(getRange(StmtWithCall)) << "' at ";
    StmtWithCall->getLocStart().dump(mSourceManager); dbgs() << "\n");
  if (ClauseI == mScopes.rend()) {
    for (auto I = ScopeI + 1, PrevI = ScopeI; I != ScopeE; ++I, ++PrevI) {
      if (!I->isClause() || !isa<CompoundStmt>(*PrevI))
        continue;
      ClauseI = I;
      break;
    }
    if (ClauseI == mScopes.rend()) {
      DEBUG(dbgs() << "[INLINE]: clause not found\n");
      return true;
    }
  }
  DEBUG(dbgs() << "[INLINE]: clause found '" <<
    getSourceText(getRange(ClauseI->getStmt())) << "' at ";
    (*ClauseI)->getLocStart().dump(mSourceManager); dbgs() << "\n");
  const FunctionDecl* Definition = nullptr;
  Call->getDirectCallee()->hasBody(Definition);
  if (!Definition) {
    toDiag(mSourceManager.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_no_body);
    return true;
  }
  if (InlineInMacro.isValid()) {
    toDiag(mSourceManager.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline);
    toDiag(mSourceManager.getDiagnostics(), InlineInMacro,
      diag::note_inline_macro_prevent);
    return true;
  }
  if (InCondOp) {
    toDiag(mSourceManager.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_ternary);
    return true;
  }
  if (InLoopCond) {
    toDiag(mSourceManager.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_loop_cond);
    return true;
  }
  if (InForInc) {
    toDiag(mSourceManager.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_for_inc);
    return true;
  }
  ClauseI->setIsUsed();
  auto &TI = mTIs[mCurrentFD];
  // Template may not exist yet if forward declaration of a function is used.
  auto &CalleeT = mTs.emplace(std::piecewise_construct,
    std::forward_as_tuple(Definition),
    std::forward_as_tuple(Definition)).first->second;
  CalleeT.setNeedToInline();
  TI.push_back({ mCurrentFD, StmtWithCall, Call, &CalleeT });
  return true;
}

std::vector<std::string> FInliner::construct(
  const std::string& Type, const std::string& Identifier,
  const std::string& Context,
  std::map<std::string, std::string>& Replacements) {
  // custom tokenizer is needed because ASTUnit doesn't have
  // properly setuped Lexer/Rewriter
  const std::string TokenPattern(
    "[(struct|union|enum)\\s+]?" + mIdentifierPattern + "|\\d+|\\S");
  clang::ast_matchers::MatchFinder MatchFinder;
  MatchFinder.addMatcher(clang::ast_matchers::varDecl().bind("varDecl"),
    &VarDeclHandler);
  std::vector<std::string> Tokens = tokenize(Type, TokenPattern);
  for (auto& T : Tokens) {
    if (Replacements.find(T) != std::end(Replacements)) {
      T = Replacements[T];
    }
  }
  VarDeclHandler.setParameters(join(Tokens, " "), Identifier,
    [&](const std::string& Line) -> std::string {
    return join(tokenize(Line, TokenPattern), " ");
  });
  Tokens.push_back(Identifier);
  // multiple positions can be found in cases like 'unsigned' and 'unsigned int'
  // which mean same type; since it's part of declaration-specifiers in grammar,
  // it is guaranteed to be before declared identifier, just choose far position
  // (meaning choosing longest type string)
  // optimization: match in reverse order until success
  std::vector<int> Counts(Tokens.size(), 0);
  swap(llvm::errs(), llvm::nulls());
  for (int Pos = Tokens.size() - 1; Pos >= 0; --Pos) {
    VarDeclHandler.initCount();
    std::unique_ptr<clang::ASTUnit> ASTUnit
      = clang::tooling::buildASTFromCode(Context + join(Tokens, " ") + ";");
    assert(ASTUnit.get() != nullptr && "AST construction failed");
    /*
    if (ASTUnit->getDiagnostics().hasErrorOccurred()) {
      std::swap(tokens[i], tokens[std::max(i - 1, 0)]);
      continue;
    }
    */
    // AST can be correctly parsed even with errors
    // ignore all, just try to find our node
    MatchFinder.matchAST(ASTUnit->getASTContext());
    Counts[Pos] = VarDeclHandler.getCount();
    if (Counts[Pos])
      break;
    std::swap(Tokens[Pos], Tokens[std::max(Pos - 1, 0)]);
  }
  swap(llvm::errs(), llvm::nulls());
  assert(std::find_if(std::begin(Counts), std::end(Counts),
    [](int Count) -> bool {
    return Count != 0;
  }) != std::end(Counts) && "At least one valid position must be found");
  return Tokens;
}

std::pair<std::string, std::string> FInliner::compile(
  const TemplateInstantiation& TI, const std::vector<std::string>& Args,
  std::set<std::string>& Decls) {
  assert(TI.mTemplate->getFuncDecl()->getNumParams() == Args.size()
    && "Undefined behavior: incorrect number of arguments specified");
  auto SrcFD = TI.mTemplate->getFuncDecl();
  // smart buffer
  std::string Canvas(getSourceText(getRange(SrcFD)));
  std::vector<unsigned int> Mapping(Canvas.size() + 1);
  std::iota(std::begin(Mapping), std::end(Mapping), 0);
  unsigned int base = getRange(SrcFD).getBegin().getRawEncoding();
  auto get = [&](const clang::SourceRange& SR) -> std::string {
    unsigned int OrigBegin = SR.getBegin().getRawEncoding() - base;
    unsigned int Begin = Mapping[OrigBegin];
    unsigned int End = Mapping[OrigBegin + getSourceText(SR).size()];
    return Canvas.substr(Begin, End - Begin);
  };
  auto update = [&](const clang::SourceRange& SR, std::string NewStr) -> void {
    unsigned int OrigBegin = SR.getBegin().getRawEncoding() - base;
    unsigned int OrigEnd = OrigBegin + getSourceText(SR).size();
    unsigned int Begin = Mapping[OrigBegin];
    unsigned int End = Mapping[OrigEnd];
    if (End - Begin == NewStr.size()) {
      Canvas.replace(Begin, End - Begin, NewStr);
    } else if (End - Begin < NewStr.size()) {
      for (auto i = OrigEnd; i < Mapping.size(); ++i) {
        Mapping[i] += NewStr.size() - (End - Begin);
      }
      Canvas.replace(Begin, End - Begin, NewStr);
    } else {
      for (auto i = OrigEnd; i < Mapping.size(); ++i) {
        Mapping[i] -= (End - Begin) - NewStr.size();
      }
      Canvas.replace(Begin, End - Begin, NewStr);
    }
    return;
  };

  std::string Params;
  std::string Context;
  // effective context construction
  auto initContext = [&]() {
    Context = "";
    //context = getSourceText(getRange(mContext.getTranslationUnitDecl()));
    for (auto D : mTs[SrcFD].getForwardDecls()) {
      Context += getSourceText(getRange(D->getRoot())) + ";";
    }
  };
  initContext();
  // update Decls set with local names to avoid collisions on deeper levels
  Decls.insert(std::begin(Args), std::end(Args));
  for (auto D : SrcFD->decls()) {
    if (const clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(D)) {
      Decls.insert(ND->getName());
    }
  }
  // prepare formal parameters' assignments
  std::map<std::string, std::string> Replacements;
  for (auto& PVD : SrcFD->parameters()) {
    std::string Identifier = addSuffix(PVD->getName(), Decls);
    Replacements[PVD->getName()] = Identifier;
    std::vector<std::string> Tokens = construct(PVD->getType().getAsString(),
      Identifier, Context, Replacements);
    Context += join(Tokens, " ") + ";";
    Params.append(join(Tokens, " ") + " = " + Args[PVD->getFunctionScopeIndex()]
      + ";");
    std::set<clang::SourceRange, std::less<clang::SourceRange>> ParmRefs;
    for (auto DRE : TI.mTemplate->getParmRefs(PVD)) {
      ParmRefs.insert(std::end(ParmRefs), getRange(DRE));
    }
    for (auto& SR : ParmRefs) {
      update(SR, Identifier);
    }
  }

  // recursively instantiate templates callable by this template
  auto hasActiveTIs = [&](const std::pair<const clang::FunctionDecl*,
    std::vector<TemplateInstantiation>>& RTI) -> bool {
    return TI.mTemplate != nullptr && RTI.first == SrcFD;
  };
  if (std::find_if(std::begin(mTIs), std::end(mTIs), hasActiveTIs)
    != std::end(mTIs)) {
    auto I = mTIs.find(SrcFD);
    if (I != mTIs.end()) {
      for (auto& TI : I->second) {
        if (TI.mTemplate == nullptr
          || !TI.mTemplate->isNeedToInline()) {
          continue;
        }
        if (mUnreachableStmts[TI.mFuncDecl].find(TI.mStmt)
          != std::end(mUnreachableStmts[TI.mFuncDecl])) {
          continue;
        }
        std::vector<std::string> Args(TI.mCallExpr->getNumArgs());
        std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
          std::begin(Args),
          [&](const clang::Expr* Arg) -> std::string {
          return get(getRange(Arg));
        });
        auto Text = compile(TI, Args, Decls);
        auto CallExpr = getSourceText(getRange(TI.mCallExpr));
        if (Text.second.size() == 0) {
          Text.first = "{" + Text.first + ";}";
        } else {
          update(getRange(TI.mCallExpr), Text.second);
          Text.first += get(getRange(TI.mStmt));
          Text.first = requiresBraces(TI.mFuncDecl, TI.mStmt)
            ? "{" + Text.first + ";}" : Text.first;
        }
        update(getRange(TI.mStmt), "/* " + CallExpr
          + " is inlined below */\n" + Text.first);
      }
    }
  }

  std::set<const clang::ReturnStmt*> RetStmts(TI.mTemplate->getRetStmts());
  std::set<const clang::ReturnStmt*> UnreachableRetStmts;
  std::set<const clang::ReturnStmt*> ReachableRetStmts;
  for (auto S : mUnreachableStmts[SrcFD]) {
    if (const clang::ReturnStmt* RS = clang::dyn_cast<clang::ReturnStmt>(S)) {
      UnreachableRetStmts.insert(RS);
    }
  }
  std::set_difference(std::begin(RetStmts), std::end(RetStmts),
    std::begin(UnreachableRetStmts), std::end(UnreachableRetStmts),
    std::inserter(ReachableRetStmts, std::end(ReachableRetStmts)));
  // for void functions one return can be implicit
  bool isSingleReturn = false;

  std::string Identifier;
  std::string RetStmt;
  std::string RetLab = addSuffix("L", Decls);
  if (SrcFD->getReturnType()->isVoidType() == false) {
    isSingleReturn = ReachableRetStmts.size() < 2;
    Identifier = addSuffix("R", Decls);
    initContext();
    std::map<std::string, std::string> Replacements;
    auto Tokens
      = construct(TI.mTemplate->getFuncDecl()->getReturnType().getAsString(),
        Identifier, Context, Replacements);
    RetStmt = join(Tokens, " ") + ";";
    for (auto& RS : ReachableRetStmts) {
      auto Text = Identifier + " = " + get(getRange(RS->getRetValue())) + ";";
      if (!isSingleReturn) {
        Text += "goto " + RetLab + ";";
        Text = "{" + Text + "}";
      }
      update(getRange(RS), Text);
    }
  } else {
    isSingleReturn = TI.mTemplate->isSingleReturn();
    std::string RetStmt(isSingleReturn ? "" : ("goto " + RetLab));
    for (auto& RS : ReachableRetStmts) {
      update(getRange(RS), RetStmt);
    }
  }
  // macro-deactivate unreachable returns
  for (auto RS : UnreachableRetStmts) {
    update(getRange(RS), "\n#if 0\n" + get(getRange(RS)) +"\n#endif\n");
  }
  Canvas = get(getRange(SrcFD->getBody()));
  if (!isSingleReturn) {
    Canvas += RetLab + ":;";
  }
  Canvas.insert(std::begin(Canvas) + 1, std::begin(Params), std::end(Params));
  Canvas.insert(std::begin(Canvas), std::begin(RetStmt), std::end(RetStmt));
  return {Canvas, Identifier};
}

DenseSet<const clang::FunctionDecl *> FInliner::findRecursion() const {
  DenseSet<const clang::FunctionDecl*> Recursive;
  for (auto &TIs : mTIs) {
    if (Recursive.count(TIs.first))
      continue;
    DenseSet<const clang::FunctionDecl *> Callers = { TIs.first };
    DenseSet<const clang::FunctionDecl *> Callees;
    auto isStepRecursion = [&Callers, &Callees, &Recursive]() {
      for (auto Caller : Callers)
        if (Callees.count(Caller)) {
          Recursive.insert(Caller);
          return true;
        }
    };
    for (auto &TIs : TIs.second)
      if (TIs.mTemplate && TIs.mTemplate->isNeedToInline())
        Callees.insert(TIs.mTemplate->getFuncDecl());
    while (!Callees.empty() && !isStepRecursion()) {
      DenseSet<const clang::FunctionDecl *> NewCallees;
      for (auto Caller : Callees) {
        auto I = mTIs.find(Caller);
        if (I == mTIs.end())
          continue;
        bool NeedToAdd = false;
        for (auto &TI : I->second)
          if (NeedToAdd = (TI.mTemplate && TI.mTemplate->isNeedToInline()))
            NewCallees.insert(TI.mTemplate->getFuncDecl());
        if (NeedToAdd)
          Callers.insert(Caller);
      }
      Callees.swap(NewCallees);
    }
  }
  return Recursive;
}

void FInliner::checkTemplates(
    const SmallVectorImpl<TemplateChecker> &Checkers) {
  for (auto& T : mTs) {
    if (!T.second.isNeedToInline())
      continue;
    for (auto &Checker : Checkers)
      if (!Checker(T.second)) {
        T.second.disableInline();
        break;
      }
  }
}

auto FInliner::getTemplateCheckers() const -> SmallVector<TemplateChecker, 8> {
  SmallVector<TemplateChecker, 8> Checkers;
  // Checks that a function is defined by the user.
  Checkers.push_back([this](const Template &T) {
    if (mSourceManager.getFileCharacteristic(T.getFuncDecl()->getLocStart()) !=
        SrcMgr::C_User) {
      DEBUG(dbgs() << "[INLINE]: non-user defined function '" <<
        T.getFuncDecl()->getName() << "' for instantiation\n");
      toDiag(mSourceManager.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline_system);
      return false;
    }
    return true;
  });
  // Checks that there are no macro in a function definition and that macro
  // does not contain function definition.
  Checkers.push_back([this](const Template &T) {
    if (T.isMacroInDecl()) {
      toDiag(mSourceManager.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline);
      toDiag(mSourceManager.getDiagnostics(), T.getMacroInDecl(),
        diag::note_inline_macro_prevent);
      if (T.getMacroSpellingHint().isValid())
        toDiag(mSourceManager.getDiagnostics(), T.getMacroSpellingHint(),
          diag::note_expanded_from_here);
      return false;
    }
    return true;
  });
  /// Checks that a specified function is not a variadic.
  Checkers.push_back([this](const Template &T) {
    if (T.getFuncDecl()->isVariadic()) {
      toDiag(mSourceManager.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline_variadic);
      return false;
    }
    return true;
  });
  /// Checks that a specified function does not contain recursion.
  Checkers.push_back([this](const Template &T) {
    static auto Recursive = findRecursion();
    if (Recursive.count(T.getFuncDecl())) {
      toDiag(mSourceManager.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline_recursive);
      return false;
    }
    return true;
  });
  return Checkers;
}

void FInliner::checkTemplateInstantiations(
    const SmallVectorImpl<TemplateInstantiationChecker> &Checkers) {
  for (auto& TIs : mTIs)
    for (auto& TI : TIs.second) {
      if (TI.mTemplate == nullptr || !TI.mTemplate->isNeedToInline())
        continue;
      for (auto &Checker : Checkers)
        if (!Checker(TI)) {
          TI.mTemplate = nullptr;
          break;
      }
    }
}

auto FInliner::getTemplatInstantiationeCheckers() const
    -> SmallVector<TemplateInstantiationChecker, 8> {
  SmallVector<TemplateInstantiationChecker, 8> Checkers;
  // Checks that external dependencies are available at the call location.
  Checkers.push_back([this](const TemplateInstantiation &TI) {
    auto &CallerT = mTs.find(TI.mFuncDecl)->second;
    auto CallerLoc =
      mSourceManager.getDecomposedExpansionLoc(TI.mFuncDecl->getLocStart());
    auto checkFD = [this, &TI, &CallerT, &CallerLoc](
        const GlobalInfoExtractor::OutermostDecl *FD) {
      if (CallerT.getForwardDecls().count(FD))
        return true;
      auto FDLoc = mSourceManager.getDecomposedExpansionLoc(
        FD->getRoot()->getLocEnd());
      while (FDLoc.first.isValid() && FDLoc.first != CallerLoc.first)
        FDLoc = mSourceManager.getDecomposedIncludedLoc(FDLoc.first);
      if (FDLoc.first.isValid() && FDLoc.second < CallerLoc.second)
        return true;
      toDiag(mSourceManager.getDiagnostics(), TI.mCallExpr->getLocStart(),
        diag::warn_disable_inline);
      toDiag(mSourceManager.getDiagnostics(),
        FD->getDescendant()->getLocation(),
        diag::note_inline_unresolvable_extern_dep);
      return false;
    };
    for (auto FD : TI.mTemplate->getForwardDecls())
      if (!checkFD(FD))
        return false;
    for (auto FD : TI.mTemplate->getMayForwardDecls())
      if (!checkFD(FD))
        return false;
    return true;
  });
  // Checks collision between local declarations of caller and global
  // declarations which is used in callee.
  // In the following example local X will hide global X after inlining.
  // So, it is necessary to disable inline expansion in this case.
  // int X;
  // void f() { X = 5; }
  // void f1() { int X; f(); }
  Checkers.push_back([this](const TemplateInstantiation &TI) {
    auto FDs = TI.mTemplate->getForwardDecls();
    if (FDs.empty())
      return true;
    /// TODO (kaniandr@gmail.com): we do not check declaration context of the
    /// caller. So, some its declarations may not hide declarations of callee
    /// with the same name. We should make this conservative search
    /// more accurate.
    auto checkCollision = [this, &TI](
        const Decl *D, const ::detail::Template::DeclSet &FDs) {
      if (auto ND = dyn_cast<NamedDecl>(D)) {
        auto HiddenItr = FDs.find_as(ND);
        if (HiddenItr != FDs.end()) {
          toDiag(mSourceManager.getDiagnostics(), TI.mCallExpr->getLocStart(),
            diag::warn_disable_inline);
          toDiag(mSourceManager.getDiagnostics(),
            (*HiddenItr)->getDescendant()->getLocation(),
            diag::note_inline_hidden_extern_dep);
          toDiag(mSourceManager.getDiagnostics(), D->getLocation(),
            diag::note_decl_hide);
          return false;
        }
      }
      return true;
    };
    for (auto D : TI.mFuncDecl->decls()) {
      if (!checkCollision(D, TI.mTemplate->getForwardDecls()))
        return false;
      if (!checkCollision(D, TI.mTemplate->getMayForwardDecls()))
        return false;
    }
    return true;
  });
  return Checkers;
}

void FInliner::HandleTranslationUnit(clang::ASTContext& Context) {
  mGIE.TraverseDecl(Context.getTranslationUnitDecl());
  StringMap<SourceLocation> RawMacros, RawIncludes;
  for (auto &File : mGIE.getFiles()) {
    StringSet<> TmpRawIds;
    getRawMacrosAndIncludes(File.second, File.first, mSourceManager,
      Context.getLangOpts(), RawMacros, RawIncludes, TmpRawIds);
  }
  for (auto &Id : RawMacros)
    mGlobalIdentifiers.insert(Id.getKey());
  for (auto &Ds : mGIE.getOutermostDecls())
    if (!Ds.getKey().empty())
      mGlobalIdentifiers.insert(Ds.getKey());
  // We check that all includes are mentioned in AST. For example, if there is
  // an include which contains macros only and this macros do not used then
  // there is no FileID for this include. Hence, it has not been parsed
  // by getRawMacrosAndIncludes() function and some macro names are lost.
  // The lost macro names potentially leads to transformation errors.
  for (auto &Include : RawIncludes) {
    // Do not check system files, because the may contains only macros which
    // are never used.
    if (mSourceManager.getFileCharacteristic(Include.second) !=
        SrcMgr::C_User)
      continue;
    if (!mGIE.getIncludeLocs().count(Include.second.getRawEncoding())) {
      toDiag(mSourceManager.getDiagnostics(), Include.second,
        diag::warn_disable_inline_include);
      return;
    }
  }
  // We perform conservative search of external dependencies and macros for
  // each function. Functions from system library will be ignored. If there is
  // a global declaration with a name equal to an identifier and location of
  // this identifier has not be visited in TraverseDecl(D),
  // we conservatively assume dependence from this declaration.
  // We also collects all raw identifiers mentioned in the body of each
  // user-defined function.
  for (auto *D : Context.getTranslationUnitDecl()->decls()) {
    if (!isa<FunctionDecl>(D))
      continue;
    mDeclRefLoc.clear();
    TraverseDecl(D);
    for (auto &T : mTs) {
      if (mSourceManager.getFileCharacteristic(T.first->getLocStart()) !=
          clang::SrcMgr::C_User)
        continue;
      LocalLexer Lex(T.first->getSourceRange(),
        Context.getSourceManager(), Context.getLangOpts());
      if (T.second.isMacroInDecl()) {
        Token Tok;
        while (!Lex.LexFromRawLexer(Tok))
          if (Tok.is(tok::raw_identifier))
            T.second.addRawId(Tok.getRawIdentifier());
        continue;
      }
      T.second.setKnownMayForwardDecls();
      SourceLocation LastMacro;
      while (true) {
        Token Tok;
        if (Lex.LexFromRawLexer(Tok))
          break;
        if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
          auto MacroLoc = Tok.getLocation();
          Lex.LexFromRawLexer(Tok);
          if (Tok.getRawIdentifier() != "pragma")
            T.second.setMacroInDecl(LastMacro);
          continue;
        }
        if (Tok.isNot(tok::raw_identifier))
          continue;
        // We conservatively check that function does not contain any macro
        // names available in translation unit. If this function should be
        // inlined we should check that after inlining some of local identifiers
        // will not be a macro. So, the mentioned conservative check simplifies
        // this check.
        // void f() { ... X ... }
        // #define X ...
        // void f1() { f(); }
        // In this case `X` will be a macro after inlining of f(), so it is not
        // possible to inline f().
        auto MacroItr = RawMacros.find(Tok.getRawIdentifier());
        if (MacroItr != RawMacros.end())
          T.second.setMacroInDecl(Tok.getLocation(), MacroItr->second);
        if (Tok.getRawIdentifier() == T.first->getName())
          continue;
        if (!mDeclRefLoc.count(Tok.getLocation().getRawEncoding())) {
          // If declaration at this location has not been found previously it is
          // necessary to conservatively check that it does not produce external
          // dependence.
          auto GlobalItr = mGIE.getOutermostDecls().find(Tok.getRawIdentifier());
          if (GlobalItr != mGIE.getOutermostDecls().end()) {
            for (auto &D : GlobalItr->second) {
              T.second.addMayForwardDecl(&D);
            DEBUG(dbgs() << "[INLINE]: potential external declaration for '" <<
              T.first->getName() << "' found '" << D.getDescendant()->getName()
              << "'\n");
            DEBUG(dbgs() << "[INLINE]: reference to '" <<
              D.getDescendant()->getName() << "' in '" << T.first->getName() <<
              "' at "; Tok.getLocation().dump(mSourceManager); dbgs() << "\n");
            }
          }
        }
      }
    }
  }
  checkTemplates(getTemplateCheckers());
  checkTemplateInstantiations(getTemplatInstantiationeCheckers());
  DEBUG(templatesInfoLog(mTs, mTIs, mSourceManager, Context.getLangOpts()));
  // recursive instantiation
  for (auto& TIs : mTIs) {
    // unusable functions are those which are not instantiated
    // meaning they are on top of call hierarchy
    auto isOnTop
      = [&](const std::pair<const clang::FunctionDecl*, Template>& lhs)
      -> bool {
      return TIs.first == lhs.first && !lhs.second.isNeedToInline();
    };
    if (std::find_if(std::begin(mTs), std::end(mTs), isOnTop)
      != std::end(mTs)) {
      bool PCHeader = true;  // seems ExternalDepsChecker filters such cases out
      for (auto& TI : TIs.second) {
        if (TI.mTemplate == nullptr
          || !TI.mTemplate->isNeedToInline()) {
          continue;
        }
        std::set<std::string> LocalDecls;
        for (auto &Id : mTs[TI.mFuncDecl].getRawIds())
          LocalDecls.insert(Id.getKey().str());
        std::vector<std::string> Args(TI.mCallExpr->getNumArgs());
        std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
          std::begin(Args),
          [&](const clang::Expr* Arg) -> std::string {
          return mRewriter.getRewrittenText(getRange(Arg));
        });
        LocalDecls.insert(std::begin(Args), std::end(Args));
        auto Text = compile(TI, Args, LocalDecls);
        auto CallExpr = getSourceText(getRange(TI.mCallExpr));
        if (Text.second.size() == 0) {
          Text.first = "{" + Text.first + ";}";
        } else {
          mRewriter.ReplaceText(getRange(TI.mCallExpr), Text.second);
          Text.first += mRewriter.getRewrittenText(getRange(TI.mStmt));
          Text.first = requiresBraces(TI.mFuncDecl, TI.mStmt)
            ? "{" + Text.first + ";}" : Text.first;
        }
        mRewriter.ReplaceText(getRange(TI.mStmt),
          "/* " + CallExpr + " is inlined below */\n" + Text.first);
      }
    }
  }
  return;
}

std::string FInliner::getSourceText(const clang::SourceRange& SR) const {
  return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(SR),
    mSourceManager, mContext.getLangOpts());
}

template<class T>
clang::SourceRange FInliner::getRange(T *Node) const {
  return getFileRange(mSourceManager, Node->getSourceRange());
}

template<typename _Container>
std::string FInliner::join(
  const _Container& _Cont, const std::string& delimiter) const {
  return _Cont.size() > 0
    ? std::accumulate(std::next(std::begin(_Cont)), std::end(_Cont),
    std::string(*std::begin(_Cont)),
    [&](const std::string& left, const std::string& right) {
    return left + delimiter + right;
  }) : "";
}

template<typename T>
void FInliner::swap(T& LObj, T& RObj) const {
  if (&LObj == &RObj) {
    return;
  }
  char* Tmp = nullptr;
  const int Size = sizeof(T) / sizeof(*Tmp);
  Tmp = new char[Size];
  std::memcpy(Tmp, &LObj, Size);
  std::memcpy(&LObj, &RObj, Size);
  std::memcpy(&RObj, Tmp, Size);
  delete[] Tmp;
  return;
}

std::string FInliner::addSuffix(
  const std::string& Prefix,
  std::set<std::string>& LocalIdentifiers) const {
  int Count = 0;
  std::set<std::string> Identifiers(LocalIdentifiers);
  for (auto &Id : mGlobalIdentifiers)
    Identifiers.insert(Id.getKey().str());
  std::string Identifier(Prefix + std::to_string(Count++));
  bool OK = false;
  while (OK == false) {
    OK = true;
    if (std::find(std::begin(Identifiers), std::end(Identifiers), Identifier)
      != std::end(Identifiers)) {
      OK = false;
      Identifier = Prefix + std::to_string(Count++);
    }
  }
  LocalIdentifiers.insert(Identifier);
  return Identifier;
}

std::vector<std::string> FInliner::tokenize(
  std::string String, std::string Pattern) const {
  std::vector<std::string> Tokens;
  std::regex rgx(Pattern);
  std::smatch sm;
  for (; std::regex_search(String, sm, rgx) == true; String = sm.suffix()) {
    Tokens.push_back(sm.str());
  }
  return Tokens;
}

bool FInliner::requiresBraces(const clang::FunctionDecl* FD,
  const clang::Stmt* S) {
  if (const clang::DeclStmt* DS = clang::dyn_cast<clang::DeclStmt>(S)) {
    std::set<const clang::Decl*> Decls(DS->decl_begin(), DS->decl_end());
    std::set<const clang::Expr*> Refs;
    std::copy_if(std::begin(mExprs[FD]), std::end(mExprs[FD]),
      std::inserter(Refs, std::begin(Refs)),
      [&](const clang::Expr* arg) -> bool {
      if (const clang::DeclRefExpr* DRE
        = llvm::dyn_cast<clang::DeclRefExpr>(arg)) {
        return std::find(std::begin(Decls), std::end(Decls),
          DRE->getFoundDecl()) != std::end(Decls);
      } else {
        return false;
      }
    });
    for (auto E : Refs) {
      if (getRange(DS).getBegin().getRawEncoding()
        <= getRange(E).getBegin().getRawEncoding()
        && getRange(E).getEnd().getRawEncoding()
        <= getRange(DS).getEnd().getRawEncoding()) {
        Refs.erase(E);
      }
    }
    return Refs.size() == 0;
  }
  return true;
}

// for debug
void FunctionInlinerQueryManager::run(llvm::Module* M,
  TransformationContext* Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  Passes.add(createFunctionInlinerPass());
  Passes.run(*M);
  return;
}
