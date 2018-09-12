//===--- tsar_finliner.cpp - Source-level Inliner (Clang) -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods necessary for function source-level inlining.
//
// TODO (kaniander@gmail.com): ASTImporter can break mapping
//   node->source (VLAs, etc) (comments from Jury Zykov).
//===----------------------------------------------------------------------===//

#include "tsar_finliner.h"
#include "ClangUtils.h"
#include "Diagnostic.h"
#include "tsar_pragma.h"
#include "SourceLocationTraverse.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"
#include <clang/AST/ASTContext.h>
#include <clang/Analysis/CallGraph.h>
#include <clang/Analysis/CFG.h>
#include <clang/Format/Format.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <set>

using namespace clang;
using namespace llvm;
using namespace tsar;
using namespace tsar::detail;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-inliner"

char ClangInlinerPass::ID = 0;
INITIALIZE_PASS_BEGIN(ClangInlinerPass, "clang-inliner",
  "Source-level Inliner (Clang)", false, false)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangInlinerPass, "clang-inliner",
  "Source-level Inliner (Clang)", false, false)

ModulePass* llvm::createClangInlinerPass() { return new ClangInlinerPass(); }

void ClangInlinerPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

void FunctionInlinerQueryManager::run(llvm::Module *M,
    TransformationContext* Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  Passes.add(createClangInlinerPass());
  Passes.run(*M);
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

bool ClangInlinerPass::runOnModule(llvm::Module& M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return false;
  }
  auto &Context = TfmCtx->getContext();
  auto &Rewriter = TfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  ClangInliner Inliner(Rewriter, Context);
  Inliner.HandleTranslationUnit();
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
  Rewrite.overwriteChangedFiles();
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

void templatesInfoLog(const ClangInliner::TemplateMap &Ts,
    const ClangInliner::TemplateInstantiationMap &TIs,
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

void ClangInliner::rememberMacroLoc(SourceLocation Loc) {
  if (Loc.isInvalid() || !Loc.isMacroID())
    return;
  mCurrentT->setMacroInDecl(Loc);
  /// Find root of subtree located in macro.
  if (mStmtInMacro.isInvalid())
   mStmtInMacro = Loc;
}

bool ClangInliner::TraverseFunctionDecl(clang::FunctionDecl *FD) {
  // Do not visit functions without body to avoid implicitly created templates
  // after call of mTs[FD] method.
  if (!FD->isThisDeclarationADefinition())
    return true;
  std::unique_ptr<clang::CFG> CFG = clang::CFG::buildCFG(
    nullptr, FD->getBody(), &mContext, clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + FD->getName()).str().data());
  llvm::SmallPtrSet<clang::CFGBlock *, 8> UB;
  unreachableBlocks(*CFG, UB);
  mCurrentT = &mTs.emplace(std::piecewise_construct,
    std::forward_as_tuple(FD),
    std::forward_as_tuple(FD)).first->second;
  for (auto *BB : UB)
    for (auto &I : *BB)
      if (auto CS = I.getAs<clang::CFGStmt>())
          mCurrentT->addUnreachableStmt(CS->getStmt());
  auto Res = RecursiveASTVisitor::TraverseFunctionDecl(FD);
  return Res;
}

bool ClangInliner::VisitReturnStmt(clang::ReturnStmt* RS) {
  mCurrentT->addRetStmt(RS);
  return RecursiveASTVisitor::VisitReturnStmt(RS);
}

bool ClangInliner::VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
  if (auto PVD = dyn_cast<ParmVarDecl>(DRE->getDecl()))
    mCurrentT->addParmRef(PVD, DRE);
  auto ND = DRE->getFoundDecl();
  if (auto OD = mGIE.findOutermostDecl(ND)) {
    DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
      mCurrentT->getFuncDecl()->getName() <<
      "' found '" << ND->getName() << "'\n");
    mCurrentT->addForwardDecl(OD);
  }
  DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
    mCurrentT->getFuncDecl()->getName() << "' at ";
    DRE->getLocation().dump(mSrcMgr);  dbgs() << "\n");
  mDeclRefLoc.insert(
    mSrcMgr.getExpansionLoc(DRE->getLocation()).getRawEncoding());
  return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
}

bool ClangInliner::VisitDecl(Decl *D) {
  traverseSourceLocation(D,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  return RecursiveASTVisitor::VisitDecl(D);
}

bool ClangInliner::VisitTypeLoc(TypeLoc TL) {
  traverseSourceLocation(TL,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  return RecursiveASTVisitor::VisitTypeLoc(TL);
}

bool ClangInliner::VisitTagTypeLoc(TagTypeLoc TTL) {
  if (auto ND = dyn_cast_or_null<NamedDecl>(TTL.getDecl())) {
    if (auto OD = mGIE.findOutermostDecl(ND)) {
      DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
        mCurrentT->getFuncDecl()->getName() <<
        "' found '" << ND->getName() << "'\n");
      mCurrentT->addForwardDecl(OD);
    }
    DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
      mCurrentT->getFuncDecl()->getName() << "' at ";
      TTL.getNameLoc().dump(mSrcMgr);  dbgs() << "\n");
    mDeclRefLoc.insert(
      mSrcMgr.getExpansionLoc(TTL.getNameLoc()).getRawEncoding());
  }
  return RecursiveASTVisitor::VisitTagTypeLoc(TTL);
}

bool ClangInliner::VisitTypedefTypeLoc(TypedefTypeLoc TTL) {
  if (auto ND = dyn_cast_or_null<NamedDecl>(TTL.getTypedefNameDecl())) {
    if (auto OD = mGIE.findOutermostDecl(ND)) {
      DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
        mCurrentT->getFuncDecl()->getName() <<
        "' found '" << ND->getName() << "'\n");
      mCurrentT->addForwardDecl(OD);
    }
    DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
      mCurrentT->getFuncDecl()->getName() << "' at ";
      TTL.getNameLoc().dump(mSrcMgr);  dbgs() << "\n");
    mDeclRefLoc.insert(
      mSrcMgr.getExpansionLoc(TTL.getNameLoc()).getRawEncoding());
  }
  return RecursiveASTVisitor::VisitTypedefTypeLoc(TTL);
}

bool ClangInliner::TraverseStmt(clang::Stmt *S) {
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
  if (!mScopes.empty()) {
    auto ParentI = mScopes.rbegin(), ParentEI = mScopes.rend();
    for (; ParentI->isClause(); ++ParentI) {
      assert(ParentI + 1 != ParentEI &&
        "At least one parent which is not a pragma must exist!");
    }
    if (ParentI + 1 == ParentEI) {
      DEBUG(dbgs() << "[INLINE]: last statement for '" <<
        mCurrentT->getFuncDecl()->getName() << "' found at ";
      S->getLocStart().dump(mSrcMgr); dbgs() << "\n");
      mCurrentT->setLastStmt(S);
    }
  }
  if (mActiveClause) {
    mScopes.push_back(mActiveClause);
    mActiveClause = { nullptr, false, false };
  }
  mScopes.emplace_back(S);
  auto Res = RecursiveASTVisitor::TraverseStmt(S);
  mScopes.pop_back();
  if (!mScopes.empty() && mScopes.back().isClause()) {
    if (!mScopes.back().isUsed()) {
      toDiag(mSrcMgr.getDiagnostics(), mScopes.back()->getLocStart(),
        diag::warn_unexpected_directive);
      toDiag(mSrcMgr.getDiagnostics(), S->getLocStart(),
        diag::note_inline_no_call);
    }
    mScopes.pop_back();
  }
  // Disable clause at the end of compound statement, body of loop, etc.
  // #pragma ...
  // }
  // <stmt>, pragma should not mark <stmt>
  if (mActiveClause) {
    toDiag(mSrcMgr.getDiagnostics(), mActiveClause->getLocStart(),
      diag::warn_unexpected_directive);
    mActiveClause.reset();
  }
  return Res;
}

bool ClangInliner::TraverseCallExpr(CallExpr *Call) {
  DEBUG(dbgs() << "[INLINE]: traverse call expression '" <<
    getSourceText(getFileRange(Call)) << "' at ";
    Call->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  auto InlineInMacro = mStmtInMacro;
  mStmtInMacro = (Call->getLocStart().isMacroID()) ? Call->getLocStart() :
    Call->getLocEnd().isMacroID() ? Call->getLocEnd() : SourceLocation();
  if (!RecursiveASTVisitor::TraverseCallExpr(Call))
    return false;
  auto &TIs = mTIs[mCurrentT->getFuncDecl()];
  // Some calls may be visited multiple times.
  // For example, struct A A1 = { .X = f() };
  if (TIs.find_as(Call) != TIs.end())
    return true;
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
  assert(ScopeI != ScopeE &&
    "Is compound statement which is function body lost?");
  auto ParentI = (*ScopeI == StmtWithCall) ? ScopeI + 1 : ScopeI;
  for (; ParentI->isClause() && ParentI != ScopeE; ++ParentI);
  assert(ParentI != ScopeE &&
    "Is compound statement which is function body lost?");
  // If statement with call is not inside a compound statement braces should be
  // added after inlining: if(...) f(); -> if (...) { /* inlined f() */ }
  bool IsNeedBraces = !isa<CompoundStmt>(*ParentI);
  DEBUG(dbgs() << "[INLINE]: statement with call '" <<
    getSourceText(getFileRange(StmtWithCall)) << "' at ";
    StmtWithCall->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  DEBUG(dbgs() << "[INLINE]: parent statement '" <<
    getSourceText(getFileRange(ParentI->getStmt())) << "' at ";
    (*ParentI)->getLocStart().dump(mSrcMgr); dbgs() << "\n");
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
    getSourceText(getFileRange(ClauseI->getStmt())) << "' at ";
    (*ClauseI)->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  // We mark this clause here, however checks bellow may disable inline
  // expansion of the current call. It seems the we should not diagnose the
  // clause as unused in this case. We only diagnose that some calls can not be
  // inlined (may be all calls).
  ClauseI->setIsUsed();
  const FunctionDecl* Definition = nullptr;
  Call->getDirectCallee()->hasBody(Definition);
  if (!Definition) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_no_body);
    return true;
  }
  if (InlineInMacro.isValid()) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline);
    toDiag(mSrcMgr.getDiagnostics(), InlineInMacro,
      diag::note_inline_macro_prevent);
    return true;
  }
  if (InCondOp) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_ternary);
    return true;
  }
  if (InLoopCond) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_loop_cond);
    return true;
  }
  if (InForInc) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_for_inc);
    return true;
  }
  // Template may not exist yet if forward declaration of a function is used.
  auto &CalleeT = mTs.emplace(std::piecewise_construct,
    std::forward_as_tuple(Definition),
    std::forward_as_tuple(Definition)).first->second;
  CalleeT.setNeedToInline();
  auto Flags = IsNeedBraces ? TemplateInstantiation::IsNeedBraces :
    TemplateInstantiation::DefaultFlags;
  TIs.insert(
    TemplateInstantiation{ mCurrentT, StmtWithCall, Call, &CalleeT, Flags });
  return true;
}

std::pair<std::string, std::string> ClangInliner::compile(
    const TemplateInstantiation &TI, ArrayRef<std::string> Args,
    const SmallVectorImpl<TemplateInstantiationChecker> &TICheckers,
    InlineStackImpl &CallStack) {
  assert(TI.mCallee->getFuncDecl()->getNumParams() == Args.size()
    && "Undefined behavior: incorrect number of arguments specified");
  auto CalleeFD = TI.mCallee->getFuncDecl();
  ExternalRewriter Canvas(getFileRange(CalleeFD), mSrcMgr, mLangOpts);
  std::string Context;
  // Initialize context to enable usage of tooling::buildASTFromCode function.
  auto initContext = [this, &Context, &TI]() {
    Context.clear();
    for (auto D : TI.mCallee->getForwardDecls())
      Context += (getSourceText(getFileRange(D->getRoot())) + ";").str();
  };
  // Prepare formal parameters' assignments.
  initContext();
  std::string Params;
  StringMap<std::string> Replacements;
  for (auto& PVD : CalleeFD->parameters()) {
    SmallString<32> Identifier;
    addSuffix(PVD->getName(), Identifier);
    Replacements[PVD->getName()] = Identifier.str();
    auto DeclT = PVD->getType().getAsString();
    auto Tokens = buildDeclStringRef(DeclT, Identifier, Context, Replacements);
    SmallString<128> DeclStr;
    Context += join(Tokens.begin(), Tokens.end(), " ", DeclStr); Context += ";";
    Params += (DeclStr + " = " + Args[PVD->getFunctionScopeIndex()] + ";").str();
    std::set<clang::SourceRange, std::less<clang::SourceRange>> ParmRefs;
    for (auto DRE : TI.mCallee->getParmRefs(PVD))
      ParmRefs.insert(std::end(ParmRefs), getFileRange(DRE));
    for (auto& SR : ParmRefs) {
      bool Res = Canvas.ReplaceText(SR, Identifier);
      assert(!Res && "Can not replace text in an external buffer!");
    }
  }
  // Now, we recursively inline all marked calls from the current function and
  // we also update external buffer. Note that we do not change input buffer.
  auto CallsItr = mTIs.find(CalleeFD);
  if (CallsItr != mTIs.end()) {
    for (auto &CallTI : CallsItr->second) {
      /// TODO (kaniandr@gmail.com): print warning in case of unreachable
      /// statements.
      if (TI.mCallee->getUnreachableStmts().count(CallTI.mStmt))
        continue;
      if (!checkTemplateInstantiation(CallTI, CallStack, TICheckers))
        continue;
      SmallVector<std::string, 8> Args(CallTI.mCallExpr->getNumArgs());
      std::transform(CallTI.mCallExpr->arg_begin(), CallTI.mCallExpr->arg_end(),
        std::begin(Args), [this, &Canvas](const clang::Expr* Arg) {
          return Canvas.getRewrittenText(getFileRange(Arg));
      });
      CallStack.push_back(&CallTI);
      auto Text = compile(CallTI, Args, TICheckers, CallStack);
      CallStack.pop_back();
      auto CallExpr = getSourceText(getFileRange(CallTI.mCallExpr));
      if (!Text.second.empty()) {
        bool Res = Canvas.ReplaceText(
          getFileRange(CallTI.mCallExpr), Text.second);
        assert(!Res && "Can not replace text in an external buffer!");
        Text.first += Canvas.getRewrittenText(getFileRange(CallTI.mStmt));
        // We will rewrite CallTI.mStmt without final ';'.
        Text.first += ';';
        if (CallTI.mFlags & TemplateInstantiation::IsNeedBraces)
          Text.first = "{" + Text.first + "}";
      }
      bool Res = Canvas.ReplaceText(getFileRange(CallTI.mStmt),
        ("/* " + CallExpr + " is inlined below */\n" + Text.first).str());
      assert(!Res && "Can not replace text in an external buffer!");
    }
  }
  SmallVector<const ReturnStmt *, 8> UnreachableRetStmts, ReachableRetStmts;
  for (auto *S : TI.mCallee->getRetStmts())
    if (TI.mCallee->getUnreachableStmts().count(S))
      UnreachableRetStmts.push_back(S);
    else
      ReachableRetStmts.push_back(S);
  bool IsNeedLabel = false;
  SmallString<128> RetIdDeclStmt;
  SmallString<8> RetId, RetLab;
  addSuffix("L", RetLab);
  if (!CalleeFD->getReturnType()->isVoidType()) {
    addSuffix("R", RetId);
    initContext();
    StringMap<std::string> Replacements;
    auto RetTy = TI.mCallee->getFuncDecl()->getReturnType().getAsString();
    auto Tokens = buildDeclStringRef(RetTy, RetId, Context, Replacements);
    join(Tokens.begin(), Tokens.end(), " ", RetIdDeclStmt);
    RetIdDeclStmt += ";";
    for (auto *RS : ReachableRetStmts) {
      SmallString<256> Text;
      raw_svector_ostream TextOS(Text);
      auto RetValue = Canvas.getRewrittenText(getFileRange(RS->getRetValue()));
      if (RS == TI.mCallee->getLastStmt()) {
        TextOS << RetId << " = " << RetValue << ";";
      } else {
        IsNeedLabel = true;
        TextOS << "{";
        TextOS << RetId << " = " << RetValue << ";";
        TextOS << "goto " << RetLab << ";";
        TextOS << "}";
      }
      bool Res = Canvas.ReplaceText(getFileRange(RS), Text);
      assert(!Res && "Can not replace text in an external buffer!");
    }
  } else {
    SmallString<16> RetStmt;
    ("goto " + RetLab).toVector(RetStmt);
    for (auto *RS : ReachableRetStmts) {
      if (RS == TI.mCallee->getLastStmt())
        continue;
      IsNeedLabel = true;
      bool Res = Canvas.ReplaceText(getFileRange(RS), RetStmt);
      assert(!Res && "Can not replace text in an external buffer!");
    }
  }
  /// TODO (kaniandr@gmail.com) : add warning for removed unreachable returns.
  for (auto RS : UnreachableRetStmts) {
    bool Res = Canvas.ReplaceText(getFileRange(RS), "");
    assert(!Res && "Can not replace text in an external buffer!");
  }
  std::string Text = Canvas.getRewrittenText(getFileRange(CalleeFD->getBody()));
  if (IsNeedLabel)
    Text.insert(Text.size() - 1, (RetLab + ":;").str());
  Text.insert(Text.begin() + 1, Params.begin(), Params.end());
  Text.insert(Text.begin(), RetIdDeclStmt.begin(), RetIdDeclStmt.end());
  return { Text, RetId.str() };
}

DenseSet<const clang::FunctionDecl *> ClangInliner::findRecursion() const {
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
      if (TIs.mCallee && TIs.mCallee->isNeedToInline())
        Callees.insert(TIs.mCallee->getFuncDecl());
    while (!Callees.empty() && !isStepRecursion()) {
      DenseSet<const clang::FunctionDecl *> NewCallees;
      for (auto Caller : Callees) {
        auto I = mTIs.find(Caller);
        if (I == mTIs.end())
          continue;
        bool NeedToAdd = false;
        for (auto &TI : I->second)
          if (NeedToAdd = (TI.mCallee && TI.mCallee->isNeedToInline()))
            NewCallees.insert(TI.mCallee->getFuncDecl());
        if (NeedToAdd)
          Callers.insert(Caller);
      }
      Callees.swap(NewCallees);
    }
  }
  return Recursive;
}

void ClangInliner::checkTemplates(
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

auto ClangInliner::getTemplateCheckers() const
    -> SmallVector<TemplateChecker, 8> {
  SmallVector<TemplateChecker, 8> Checkers;
  // Checks that a function is defined by the user.
  Checkers.push_back([this](const Template &T) {
    if (mSrcMgr.getFileCharacteristic(T.getFuncDecl()->getLocStart()) !=
        SrcMgr::C_User) {
      DEBUG(dbgs() << "[INLINE]: non-user defined function '" <<
        T.getFuncDecl()->getName() << "' for instantiation\n");
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline_system);
      return false;
    }
    return true;
  });
  // Checks that there are no macro in a function definition and that macro
  // does not contain function definition.
  Checkers.push_back([this](const Template &T) {
    if (T.isMacroInDecl()) {
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline);
      toDiag(mSrcMgr.getDiagnostics(), T.getMacroInDecl(),
        diag::note_inline_macro_prevent);
      if (T.getMacroSpellingHint().isValid())
        toDiag(mSrcMgr.getDiagnostics(), T.getMacroSpellingHint(),
          diag::note_expanded_from_here);
      return false;
    }
    return true;
  });
  /// Checks that a specified function is not a variadic.
  Checkers.push_back([this](const Template &T) {
    if (T.getFuncDecl()->isVariadic()) {
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline_variadic);
      return false;
    }
    return true;
  });
  /// Checks that a specified function does not contain recursion.
  Checkers.push_back([this](const Template &T) {
    static auto Recursive = findRecursion();
    if (Recursive.count(T.getFuncDecl())) {
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline_recursive);
      return false;
    }
    return true;
  });
  return Checkers;
}

bool ClangInliner::checkTemplateInstantiation(TemplateInstantiation &TI,
    const InlineStackImpl &CallStack,
    const SmallVectorImpl<TemplateInstantiationChecker> &Checkers) {
  assert(TI.mCallee && "Template must not be null!");
  if (!TI.mCallee->isNeedToInline())
    return false;
  for (auto &Checker : Checkers)
    if (!Checker(TI, CallStack))
      return false;
  return true;
}

auto ClangInliner::getTemplatInstantiationCheckers() const
    -> SmallVector<TemplateInstantiationChecker, 8> {
  SmallVector<TemplateInstantiationChecker, 8> Checkers;
  // Checks that external dependencies are available at the call location.
  Checkers.push_back([this](const TemplateInstantiation &TI,
      const InlineStackImpl &CallStack) {
    assert(CallStack.back()->mCallee == TI.mCaller &&
      "Function as the top of stack should make a call which is checked!");
    for (auto &Caller : CallStack) {
      auto *CallerT = Caller->mCallee;
      auto CallerLoc = mSrcMgr.getDecomposedExpansionLoc(
        CallerT->getFuncDecl()->getLocStart());
      auto checkFD = [this, &TI, CallerT, &CallerLoc](
        const GlobalInfoExtractor::OutermostDecl *FD) {
        if (CallerT->getForwardDecls().count(FD))
          return true;
        auto FDLoc = mSrcMgr.getDecomposedExpansionLoc(
          FD->getRoot()->getLocEnd());
        while (FDLoc.first.isValid() && FDLoc.first != CallerLoc.first)
          FDLoc = mSrcMgr.getDecomposedIncludedLoc(FDLoc.first);
        if (FDLoc.first.isValid() && FDLoc.second < CallerLoc.second)
          return true;
        toDiag(mSrcMgr.getDiagnostics(), TI.mCallExpr->getLocStart(),
          diag::warn_disable_inline);
        toDiag(mSrcMgr.getDiagnostics(),
          FD->getDescendant()->getLocation(),
          diag::note_inline_unresolvable_extern_dep);
        return false;
      };
      for (auto FD : TI.mCallee->getForwardDecls())
        if (!checkFD(FD))
          return false;
      for (auto FD : TI.mCallee->getMayForwardDecls())
        if (!checkFD(FD))
          return false;
    }
    return true;
  });
  // Checks collision between local declarations of caller and global
  // declarations which is used in callee.
  // In the following example local X will hide global X after inlining.
  // So, it is necessary to disable inline expansion in this case.
  // int X;
  // void f() { X = 5; }
  // void f1() { int X; f(); }
  Checkers.push_back([this](const TemplateInstantiation &TI,
      const InlineStackImpl &CallStack) {
    assert(CallStack.back()->mCallee == TI.mCaller &&
      "Function as the top of stack should make a call which is checked!");
    auto FDs = TI.mCallee->getForwardDecls();
    if (FDs.empty())
      return true;
    /// TODO (kaniandr@gmail.com): we do not check declaration context of the
    /// caller. So, some its declarations may not hide declarations of callee
    /// with the same name. We should make this conservative search
    /// more accurate.
    auto checkCollision = [this, &TI](
        const Decl *D, const Template::DeclSet &FDs) {
      if (auto ND = dyn_cast<NamedDecl>(D)) {
        // Do not use ND in find_as() because it checks that a declaration in
        // the set equals to ND. However, we want to check that there is
        // no local declaration which differs from forward declaration but
        // has the same name.
        auto HiddenItr = FDs.find_as(ND->getName());
        if (HiddenItr != FDs.end() && ND != (*HiddenItr)->getDescendant()) {
          toDiag(mSrcMgr.getDiagnostics(), TI.mCallExpr->getLocStart(),
            diag::warn_disable_inline);
          toDiag(mSrcMgr.getDiagnostics(),
            (*HiddenItr)->getDescendant()->getLocation(),
            diag::note_inline_hidden_extern_dep);
          toDiag(mSrcMgr.getDiagnostics(), D->getLocation(),
            diag::note_decl_hide);
          return false;
        }
      }
      return true;
    };
    for (auto *Caller : CallStack) {
      for (auto D : Caller->mCallee->getFuncDecl()->decls()) {
        if (!checkCollision(D, TI.mCallee->getForwardDecls()))
          return false;
        if (!checkCollision(D, TI.mCallee->getMayForwardDecls()))
          return false;
      }
    }
    return true;
  });
  return Checkers;
}

void ClangInliner::HandleTranslationUnit() {
  mGIE.TraverseDecl(mContext.getTranslationUnitDecl());
  StringMap<SourceLocation> RawMacros, RawIncludes;
  for (auto &File : mGIE.getFiles()) {
    StringSet<> TmpRawIds;
    getRawMacrosAndIncludes(File.second, File.first, mSrcMgr, mLangOpts,
      RawMacros, RawIncludes, mIdentifiers);
  }
  // We check that all includes are mentioned in AST. For example, if there is
  // an include which contains macros only and this macros do not used then
  // there is no FileID for this include. Hence, it has not been parsed
  // by getRawMacrosAndIncludes() function and some macro names are lost.
  // The lost macro names potentially leads to transformation errors.
  for (auto &Include : RawIncludes) {
    // Do not check system files, because the may contains only macros which
    // are never used.
    if (mSrcMgr.getFileCharacteristic(Include.second) !=
        SrcMgr::C_User)
      continue;
    if (!mGIE.getIncludeLocs().count(Include.second.getRawEncoding())) {
      toDiag(mSrcMgr.getDiagnostics(), Include.second,
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
  for (auto *D : mContext.getTranslationUnitDecl()->decls()) {
    if (!isa<FunctionDecl>(D))
      continue;
    mDeclRefLoc.clear();
    mCurrentT = nullptr;
    TraverseDecl(D);
    for (auto &T : mTs) {
      if (mSrcMgr.getFileCharacteristic(T.first->getLocStart()) !=
          clang::SrcMgr::C_User)
        continue;
      if (T.second.isMacroInDecl())
        continue;
      LocalLexer Lex(T.first->getSourceRange(), mSrcMgr, mLangOpts);
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
              "' at "; Tok.getLocation().dump(mSrcMgr); dbgs() << "\n");
            }
          }
        }
      }
    }
  }
  checkTemplates(getTemplateCheckers());
  DEBUG(templatesInfoLog(mTs, mTIs, mSrcMgr, mLangOpts));
  CallGraph CG;
  CG.TraverseDecl(mContext.getTranslationUnitDecl());
  ReversePostOrderTraversal<CallGraph *> RPOT(&CG);
  auto TICheckers = getTemplatInstantiationCheckers();
  for (auto I = RPOT.begin(), EI = RPOT.end(); I != EI; ++I) {
    if (!(*I)->getDecl() || !isa<FunctionDecl>((*I)->getDecl()))
      continue;
    auto CallsItr = mTIs.find(cast<FunctionDecl>((*I)->getDecl()));
    if (CallsItr == mTIs.end())
      continue;
    SmallVector<const TemplateInstantiation *, 8> CallStack;
    // We create a bogus template instantiation to identify a root of call graph
    // subtree which should be inlined.
    TemplateInstantiation BogusTI { nullptr, nullptr, nullptr,
      &mTs[CallsItr->first], TemplateInstantiation::DefaultFlags };
    CallStack.push_back(&BogusTI);
    for (auto &TI : CallsItr->second) {
      if (!checkTemplateInstantiation(TI, CallStack, TICheckers))
        continue;
      SmallVector<std::string, 8> Args(TI.mCallExpr->getNumArgs());
      std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
        std::begin(Args), [this](const clang::Expr* Arg) {
          return mRewriter.getRewrittenText(getFileRange(Arg));
      });
      CallStack.push_back(&TI);
      auto Text = compile(TI, Args, TICheckers, CallStack);
      CallStack.pop_back();
      auto CallExpr = getSourceText(getFileRange(TI.mCallExpr));
      if (!Text.second.empty()) {
        mRewriter.ReplaceText(getFileRange(TI.mCallExpr), Text.second);
        Text.first += mRewriter.getRewrittenText(getFileRange(TI.mStmt));
        // We will rewrite CallTI.mStmt without final ';'.
        Text.first += ';';
        if (TI.mFlags & TemplateInstantiation::IsNeedBraces)
          Text.first = "{" + Text.first + "}";
      }
      mRewriter.ReplaceText(getFileRange(TI.mStmt),
        ("/* " + CallExpr + " is inlined below */\n" + Text.first).str());
    }
  }
}

StringRef ClangInliner::getSourceText(const clang::SourceRange& SR) const {
  return Lexer::getSourceText(
    CharSourceRange::getTokenRange(SR), mSrcMgr, mLangOpts);
}

template<class T>
clang::SourceRange ClangInliner::getFileRange(T *Node) const {
  return tsar::getFileRange(mSrcMgr, Node->getSourceRange());
}

void ClangInliner::addSuffix(StringRef Prefix, SmallVectorImpl<char> &Out) {
  for (unsigned Count = 0;
    mIdentifiers.count((Prefix + Twine(Count)).toStringRef(Out)); ++Count);
  mIdentifiers.insert(StringRef(Out.data(), Out.size()));
}
