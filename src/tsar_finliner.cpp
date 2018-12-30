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
#include "ASTImportInfo.h"
#include "ClangFormatPass.h"
#include "ClangUtils.h"
#include "Diagnostic.h"
#include "tsar_pragma.h"
#include "tsar_query.h"
#include "SourceLocationTraverse.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"
#include <clang/AST/ASTContext.h>
#include <clang/Analysis/CallGraph.h>
#include <clang/Analysis/CFG.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>

using namespace clang;
using namespace llvm;
using namespace tsar;
using namespace tsar::detail;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-inline"

char ClangInlinerPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangInlinerPass, "clang-inline",
  "Source-level Inliner (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(ImmutableASTImportInfoPass)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangInlinerPass, "clang-inline",
  "Source-level Inliner (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

ModulePass* llvm::createClangInlinerPass() { return new ClangInlinerPass(); }

void ClangInlinerPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.addUsedIfAvailable<ImmutableASTImportInfoPass>();
  AU.setPreservesAll();
}

bool ClangInlinerPass::runOnModule(llvm::Module& M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto &Context = TfmCtx->getContext();
  auto &Rewriter = TfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  if (Context.getLangOpts().CPlusPlus)
    toDiag(Context.getDiagnostics(), diag::warn_inline_support_cpp);
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  ClangInliner Inliner(Rewriter, Context,
    GIP.getGlobalInfo(), GIP.getRawInfo(), *ImportInfo);
  Inliner.HandleTranslationUnit();
  return false;
}

namespace {
#ifdef LLVM_DEBUG
void printLocLog(const SourceManager &SM, SourceRange R) {
  dbgs() << "[";
  R.getBegin().dump(SM);
  dbgs() << ",";
  R.getEnd().dump(SM);
  dbgs() << "]";
}

void templatesInfoLog(const ClangInliner::TemplateMap &Ts,
    const SourceManager &SM, const LangOptions &LangOpts) {
  auto sourceText = [&SM, &LangOpts](const Stmt *S) {
    auto SR = getExpansionRange(SM, S->getSourceRange());
    return Lexer::getSourceText(SR, SM, LangOpts);
  };
  llvm::dbgs() << "[INLINE]: enabled templates (" <<
    std::count_if(std::begin(Ts), std::end(Ts),
      [](const std::pair<
        const clang::FunctionDecl*, std::unique_ptr<Template>> &LHS) {
        return LHS.second->isNeedToInline();
      }) << "):\n";
  for (auto &T : Ts)
    if (T.second->isNeedToInline())
      llvm::dbgs() << " '" << T.first->getName() << "'";
  llvm::dbgs() << '\n';
  llvm::dbgs() << "[INLINE]: disabled templates (" <<
    std::count_if(std::begin(Ts), std::end(Ts),
      [](const std::pair<
        const clang::FunctionDecl*, std::unique_ptr<Template>> &LHS) {
        return !LHS.second->isNeedToInline();
      }) << "):\n";
  for (auto &T : Ts)
    if (!T.second->isNeedToInline())
      llvm::dbgs() << " '" << T.first->getName() << "'";
  llvm::dbgs() << '\n';
  llvm::dbgs() << "[INLINE]: total template instantiations:\n";
  for (auto &T : Ts) {
    if (T.second->getCalls().empty())
      continue;
    llvm::dbgs() << " in '" << T.first->getName() << "':\n";
    for (auto &TI : T.second->getCalls()) {
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
  if (!FD->isThisDeclarationADefinition())
    return true;
  std::unique_ptr<clang::CFG> CFG = clang::CFG::buildCFG(
    nullptr, FD->getBody(), &mContext, clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + FD->getName()).str().data());
  llvm::SmallPtrSet<clang::CFGBlock *, 8> UB;
  unreachableBlocks(*CFG, UB);
  auto &NewT = mTs.try_emplace(FD).first->second;
  if (!NewT)
    NewT = llvm::make_unique<Template>(FD);
  mCurrentT = NewT.get();
  for (auto *BB : UB)
    for (auto &I : *BB)
      if (auto CS = I.getAs<clang::CFGStmt>())
          mCurrentT->addUnreachableStmt(CS->getStmt());
  auto Res = RecursiveASTVisitor::TraverseFunctionDecl(FD);
  return Res;
}

bool ClangInliner::VisitReturnStmt(clang::ReturnStmt* RS) {
  auto ParentI = mScopes.rbegin(), ParentEI = mScopes.rend();
  assert(ParentI != ParentEI &&
    "At least one parent which is not a pragma must exist!");
  for (; ParentI->isClause(); ++ParentI) {
    assert(ParentI + 1 != ParentEI &&
      "At least one parent which is not a pragma must exist!");
  }
  mCurrentT->addRetStmt(RS, !isa<CompoundStmt>(*ParentI));
  return RecursiveASTVisitor::VisitReturnStmt(RS);
}

bool ClangInliner::VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
  if (auto PVD = dyn_cast<ParmVarDecl>(DRE->getDecl()))
    mCurrentT->addParmRef(PVD, DRE);
  auto ND = DRE->getFoundDecl();
  visitNamedDecl(ND);
  LLVM_DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
    mCurrentT->getFuncDecl()->getName() << "' at ";
    DRE->getLocation().dump(mSrcMgr);  dbgs() << "\n");
  mDeclRefLoc.insert(
    mSrcMgr.getExpansionLoc(DRE->getLocation()).getRawEncoding());
  return RecursiveASTVisitor::VisitDeclRefExpr(DRE);
}

bool ClangInliner::VisitDecl(Decl *D) {
  traverseSourceLocation(D,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  if (auto ND = dyn_cast<NamedDecl>(D)) {
    LLVM_DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
      mCurrentT->getFuncDecl()->getName() << "' at ";
      ND->getLocation().dump(mSrcMgr);  dbgs() << "\n");
    mDeclRefLoc.insert(
      mSrcMgr.getExpansionLoc(ND->getLocation()).getRawEncoding());
  }
  return RecursiveASTVisitor::VisitDecl(D);
}

bool ClangInliner::VisitTypeLoc(TypeLoc TL) {
  traverseSourceLocation(TL,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  return RecursiveASTVisitor::VisitTypeLoc(TL);
}

bool ClangInliner::VisitTagType(TagType *TT) {
  visitNamedDecl(TT->getDecl());
  return RecursiveASTVisitor::VisitTagType(TT);
}

bool ClangInliner::VisitTagTypeLoc(TagTypeLoc TTL) {
  if (auto ND = dyn_cast_or_null<NamedDecl>(TTL.getDecl())) {
    LLVM_DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
      mCurrentT->getFuncDecl()->getName() << "' at ";
      TTL.getNameLoc().dump(mSrcMgr);  dbgs() << "\n");
    mDeclRefLoc.insert(
      mSrcMgr.getExpansionLoc(TTL.getNameLoc()).getRawEncoding());
  }
  return RecursiveASTVisitor::VisitTagTypeLoc(TTL);
}

bool ClangInliner::VisitTypedefType(clang::TypedefType *TT) {
  visitNamedDecl(TT->getDecl());
  return RecursiveASTVisitor::VisitTypedefType(TT);
}

void ClangInliner::visitNamedDecl(clang::NamedDecl *ND) {
  // We want to search dependence using a pointer to declaration. So,
  // use the first redeclaration and store the same pointers for all
  // redeclarations.
  assert((!mGIE.findOutermostDecl(ND) ||
    mGIE.findOutermostDecl(cast<NamedDecl>(*ND->redecls_begin()))) &&
    "Seems that redeclaration is not presented in AST");
  ND = cast<NamedDecl>(*ND->redecls_begin());
  if (auto OD = mGIE.findOutermostDecl(ND)) {
    LLVM_DEBUG(dbgs() << "[INLINE]: external declaration for '" <<
      mCurrentT->getFuncDecl()->getName() <<
      "' found '" << ND->getName() << "'\n");
    mCurrentT->addForwardDecl(OD);
  }
}

bool ClangInliner::VisitTypedefTypeLoc(TypedefTypeLoc TTL) {
  if (auto ND = dyn_cast_or_null<NamedDecl>(TTL.getTypedefNameDecl())) {
    LLVM_DEBUG(dbgs() << "[INLINE]: reference to '" << ND->getName() << "' in '" <<
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
  SmallVector<Stmt *, 1> Clauses;
  Pragma P(*S);
  if (findClause(P, ClauseId::Inline, Clauses)) {
    mActiveClause = { Clauses.front(), true, false };
    if (!pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
        mCurrentT->getToRemove()))
      toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
        diag::warn_remove_directive_in_macro);
    return true;
  }
  if (P)
    return true;
  traverseSourceLocation(S,
    [this](SourceLocation Loc) { rememberMacroLoc(Loc); });
  if (!mScopes.empty()) {
    auto ParentI = mScopes.rbegin(), ParentEI = mScopes.rend();
    for (; ParentI->isClause(); ++ParentI) {
      assert(ParentI + 1 != ParentEI &&
        "At least one parent which is not a pragma must exist!");
    }
    if (ParentI + 1 == ParentEI) {
      LLVM_DEBUG(dbgs() << "[INLINE]: last statement for '" <<
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
  LLVM_DEBUG(dbgs() << "[INLINE]: traverse call expression '" <<
    getSourceText(getFileRange(Call)) << "' at ";
    Call->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  auto InlineInMacro = mStmtInMacro;
  mStmtInMacro = (Call->getLocStart().isMacroID()) ? Call->getLocStart() :
    Call->getLocEnd().isMacroID() ? Call->getLocEnd() : SourceLocation();
  if (!RecursiveASTVisitor::TraverseCallExpr(Call))
    return false;
  // Some calls may be visited multiple times.
  // For example, struct A A1 = { .X = f() };
  if (mCurrentT->findCall(Call) != mCurrentT->getCalls().end())
    return true;
  std::swap(InlineInMacro, mStmtInMacro);
  if (mStmtInMacro.isInvalid())
    mStmtInMacro = InlineInMacro;
  assert(!mScopes.empty() && "At least one parent statement must exist!");
  auto ScopeI = mScopes.rbegin(), ScopeE = mScopes.rend();
  clang::Stmt *StmtWithCall = Call;
  auto ClauseI = mScopes.rend();
  bool InCondOp = false, InLoopCond = false, InForInc = false;
  bool InLogicRHS = false;
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
    } else if (auto BO = dyn_cast<clang::BinaryOperator > (*ScopeI)) {
      if (BO->getRHS() == *(ScopeI - 1))
        InLogicRHS = BO->isLogicalOp() || BO->isBitwiseOp();
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
  LLVM_DEBUG(dbgs() << "[INLINE]: statement with call '" <<
    getSourceText(getFileRange(StmtWithCall)) << "' at ";
    StmtWithCall->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  LLVM_DEBUG(dbgs() << "[INLINE]: parent statement at ";
    (*ParentI)->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  if (ClauseI == mScopes.rend()) {
    for (auto I = ScopeI + 1, PrevI = ScopeI; I != ScopeE; ++I, ++PrevI) {
      if (!I->isClause() || !isa<CompoundStmt>(*PrevI))
        continue;
      ClauseI = I;
      break;
    }
    if (ClauseI == mScopes.rend()) {
      LLVM_DEBUG(dbgs() << "[INLINE]: clause not found\n");
      return true;
    }
  }
  LLVM_DEBUG(dbgs() << "[INLINE]: clause found '" <<
    getSourceText(getFileRange(ClauseI->getStmt())) << "' at ";
    (*ClauseI)->getLocStart().dump(mSrcMgr); dbgs() << "\n");
  // We mark this clause here, however checks bellow may disable inline
  // expansion of the current call. It seems the we should not diagnose the
  // clause as unused in this case. We only diagnose that some calls can not be
  // inlined (may be all calls).
  ClauseI->setIsUsed();
  if (mCurrentT->getUnreachableStmts().count(Call)) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_unreachable);
    return true;
  }
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
  if (mSrcMgr.getDecomposedExpansionLoc(StmtWithCall->getLocStart()).first !=
      mSrcMgr.getDecomposedExpansionLoc(StmtWithCall->getLocEnd()).first) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline);
    toDiag(mSrcMgr.getDiagnostics(), StmtWithCall->getLocStart(),
      diag::note_source_range_not_single_file);
    toDiag(mSrcMgr.getDiagnostics(), StmtWithCall->getLocEnd(),
      diag::note_end_location);
  }
  // Now, we search macro definitions in the call expression.
  // f(
  //   #include ...
  // );
  // We also search for raw macros which locations have not been visited.
  LocalLexer Lex(StmtWithCall->getSourceRange(), mSrcMgr, mLangOpts);
  while (true) {
    Token Tok;
    if (Lex.LexFromRawLexer(Tok))
      break;
    if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
      auto MacroLoc = Tok.getLocation();
      Lex.LexFromRawLexer(Tok);
      if (Tok.getRawIdentifier() != "pragma") {
        toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
          diag::warn_disable_inline);
        toDiag(mSrcMgr.getDiagnostics(), MacroLoc,
          diag::note_inline_macro_prevent);
        return true;
      }
    }
    if (Tok.isNot(tok::raw_identifier))
      continue;
    if (mDeclRefLoc.count(Tok.getLocation().getRawEncoding()))
      continue;
    auto MacroItr = mRawInfo.Macros.find(Tok.getRawIdentifier());
    if (MacroItr == mRawInfo.Macros.end())
      continue;
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline);
    toDiag(mSrcMgr.getDiagnostics(), Tok.getLocation(),
      diag::note_inline_macro_prevent);
    toDiag(mSrcMgr.getDiagnostics(), MacroItr->second,
      diag::note_expanded_from_here);
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
  if (InLogicRHS) {
    toDiag(mSrcMgr.getDiagnostics(), Call->getLocStart(),
      diag::warn_disable_inline_in_logic_rhs);
    return true;
  }
  // Template may not exist yet if forward declaration of a function is used.
  auto &CalleeT = mTs.try_emplace(Definition).first->second;
  if (!CalleeT)
    CalleeT = llvm::make_unique<Template>(Definition);
  CalleeT->setNeedToInline();
  auto F = IsNeedBraces ? TemplateInstantiation::IsNeedBraces :
    TemplateInstantiation::DefaultFlags;
  mCurrentT->addCall(
    TemplateInstantiation{ mCurrentT, StmtWithCall, Call, CalleeT.get(), F });
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
    for (auto D : TI.mCallee->getMayForwardDecls())
      Context += (getSourceText(getFileRange(D->getRoot())) + ";").str();
  };
  // Prepare formal parameters' assignments.
  initContext();
  std::string Params;
  StringMap<std::string> Replacements;
  std::size_t ParamIdx = 0;
  for (auto& PVD : CalleeFD->parameters()) {
    SmallString<32> Identifier;
    addSuffix(PVD->getName(), Identifier);
    Replacements[PVD->getName()] = Identifier.str();
    auto DeclT = PVD->getType().getAsString();
    auto Tokens = buildDeclStringRef(DeclT, Identifier, Context, Replacements);
    assert(!Tokens.empty() && "Unable to build parameter declaration!");
    SmallString<128> DeclStr;
    Context += join(Tokens.begin(), Tokens.end(), " ", DeclStr); Context += ";";
    /// TODO (kaniandr@gmail.com): we use ParamIdx instead of method
    /// PVD->getFunctionScopeIndex() because it may return incorrect value.
    /// Seems that AST merge action may break parameters numbering.
    Params += (DeclStr + " = " + Args[ParamIdx++] + ";").str();
    for (auto DRE : TI.mCallee->getParmRefs(PVD)) {
      bool Res = Canvas.ReplaceText(getFileRange(DRE), Identifier);
      assert(!Res && "Can not replace text in an external buffer!");
    }
  }
  // Now, we recursively inline all marked calls from the current function and
  // we also update external buffer. Note that we do not change input buffer.
  for (auto &CallTI : TI.mCallee->getCalls()) {
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
      if (CallTI.mFlags & TemplateInstantiation::IsNeedBraces)
        Text.first = "{" + Text.first;
      Canvas.InsertTextAfter(
        mSrcMgr.getFileLoc(CallTI.mStmt->getLocStart()).getLocWithOffset(-1),
          ("/* " + CallExpr + " is inlined below */\n" + Text.first).str());
      if (CallTI.mFlags & TemplateInstantiation::IsNeedBraces)
        Canvas.InsertTextAfter(
          mSrcMgr.getFileLoc(CallTI.mStmt->getLocEnd()), "}");
    } else {
      bool Res = Canvas.ReplaceText(getFileRange(CallTI.mStmt),
        ("/* " + CallExpr + " is inlined below */\n" + Text.first).str());
      assert(!Res && "Can not replace text in an external buffer!");
      Token SemiTok;
      if (!getRawTokenAfter(mSrcMgr.getFileLoc(CallTI.mStmt->getLocEnd()),
        mSrcMgr, mLangOpts, SemiTok) && SemiTok.is(tok::semi))
        Canvas.RemoveText(SemiTok.getLocation(), true);
    }
  }
  SmallVector<Template::ReturnStmts::value_type, 8> UnreachableRetStmts;
  SmallVector<Template::ReturnStmts::value_type, 8> ReachableRetStmts;
  for (auto &S : TI.mCallee->getRetStmts())
    if (TI.mCallee->getUnreachableStmts().count(S.first))
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
    assert(!Tokens.empty() && "Unable to build return value declaration!");
    join(Tokens.begin(), Tokens.end(), " ", RetIdDeclStmt);
    RetIdDeclStmt += ";\n";
    for (auto &RS : ReachableRetStmts) {
      SmallString<256> Text;
      raw_svector_ostream TextOS(Text);
      auto RetValue =
        Canvas.getRewrittenText(getFileRange(RS.first->getRetValue()));
      if (RS.first == TI.mCallee->getLastStmt()) {
        TextOS << RetId << " = " << RetValue << ";";
      } else {
        IsNeedLabel = true;
        if (RS.second)
          TextOS << "{";
        TextOS << RetId << " = " << RetValue << ";";
        TextOS << "goto " << RetLab << ";";
        if (RS.second)
          TextOS << "}";
      }
      bool Res = Canvas.ReplaceText(getFileRange(RS.first), Text);
      assert(!Res && "Can not replace text in an external buffer!");
      Token SemiTok;
      if (!getRawTokenAfter(mSrcMgr.getFileLoc(RS.first->getLocEnd()),
          mSrcMgr, mLangOpts, SemiTok) && SemiTok.is(tok::semi))
        Canvas.RemoveText(SemiTok.getLocation(), true);
    }
  } else {
    SmallString<16> RetStmt;
    ("goto " + RetLab).toVector(RetStmt);
    for (auto &RS : ReachableRetStmts) {
      if (RS.first == TI.mCallee->getLastStmt())
        continue;
      IsNeedLabel = true;
      bool Res = Canvas.ReplaceText(getFileRange(RS.first), RetStmt);
      assert(!Res && "Can not replace text in an external buffer!");
    }
  }
  if (!UnreachableRetStmts.empty())
    toDiag(mSrcMgr.getDiagnostics(), TI.mCallExpr->getLocStart(),
      diag::remark_inline);
  for (auto &RS : UnreachableRetStmts) {
    bool Res = Canvas.ReplaceText(getFileRange(RS.first), "");
    assert(!Res && "Can not replace text in an external buffer!");
    if (!RS.second) {
      // If braces is not needed we can remover ending `;`.
      // if (...) return; => IsNeedBraces == true => do not remove `;`.
      Token SemiTok;
      if (!getRawTokenAfter(mSrcMgr.getFileLoc(RS.first->getLocEnd()),
          mSrcMgr, mLangOpts, SemiTok) && SemiTok.is(tok::semi))
        Canvas.RemoveText(SemiTok.getLocation(), true);
    }
    toDiag(mSrcMgr.getDiagnostics(), getFileRange(RS.first).getBegin(),
      diag::remark_remove_unreachable);
  }
  for (auto SR : TI.mCallee->getToRemove())
    Canvas.RemoveText(SR, true);
  std::string Text = Canvas.getRewrittenText(getFileRange(CalleeFD->getBody()));
  Text += "\n";
  if (IsNeedLabel)
    Text.insert(Text.size() - 1, (RetLab + ":;").str());
  Text.insert(Text.begin() + 1, Params.begin(), Params.end());
  // We should check that all includes are mentioned in AST.
  // For example, if there is an include which contains macros only and
  // this macros do not used then there is no FileID for this include.
  // Hence, it has not been parsed by getRawMacrosAndIncludes() function and
  // some macro names are lost. The lost macro names potentially leads to
  // transformation errors.
  //
  // However, it is not possible to establish correspondence between #include
  // directives and available file entries due to complexity of the search
  // of files that should be included. So, we disable this check. Instead we
  // add `#pragma spf assert nomacro` before the body of inlined function to
  // perform the check using the analyzer at the next time.
  SmallString<64> NoMacroPragma;
  getPragmaText(ClauseId::AssertNoMacro, NoMacroPragma);
  Text.insert(Text.begin(), NoMacroPragma.begin(), NoMacroPragma.end());
  Text.insert(Text.begin(), RetIdDeclStmt.begin(), RetIdDeclStmt.end());
  return { Text, RetId.str() };
}

DenseSet<const clang::FunctionDecl *> ClangInliner::findRecursion() const {
  DenseSet<const clang::FunctionDecl*> Recursive;
  for (auto &T : mTs) {
    if (Recursive.count(T.first))
      continue;
    DenseSet<const clang::FunctionDecl *> Callers = { T.first };
    DenseSet<const clang::FunctionDecl *> Callees;
    auto isStepRecursion = [&Callers, &Callees, &Recursive]() {
      for (auto Caller : Callers)
        if (Callees.count(Caller)) {
          Recursive.insert(Caller);
          return true;
        }
      return false;
    };
    for (auto &TIs : T.second->getCalls())
      if (TIs.mCallee && TIs.mCallee->isNeedToInline())
        Callees.insert(TIs.mCallee->getFuncDecl());
    while (!Callees.empty() && !isStepRecursion()) {
      DenseSet<const clang::FunctionDecl *> NewCallees;
      for (auto Caller : Callees) {
        auto I = mTs.find(Caller);
        bool NeedToAdd = false;
        for (auto &TI : I->second->getCalls())
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
    if (!T.second->isNeedToInline())
      continue;
    for (auto &Checker : Checkers)
      if (!Checker(*T.second)) {
        T.second->disableInline();
        break;
      }
  }
}

auto ClangInliner::getTemplateCheckers() const
    -> SmallVector<TemplateChecker, 8> {
  SmallVector<TemplateChecker, 8> Checkers;
  // Checks that start and end of function definition are located at the same
  // file.
  Checkers.push_back([this](const Template &T) {
    auto SR = mSrcMgr.getExpansionRange(T.getFuncDecl()->getSourceRange());
    if (!mSrcMgr.isWrittenInSameFile(SR.getBegin(), SR.getEnd())) {
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocation(),
        diag::warn_disable_inline);
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocStart(),
        diag::note_source_range_not_single_file);
      toDiag(mSrcMgr.getDiagnostics(), T.getFuncDecl()->getLocEnd(),
        diag::note_end_location);
      return false;
    }
    return true;
  });
  // Checks that a function is defined by the user.
  Checkers.push_back([this](const Template &T) {
    if (mSrcMgr.getFileCharacteristic(T.getFuncDecl()->getLocStart()) !=
        SrcMgr::C_User) {
      LLVM_DEBUG(dbgs() << "[INLINE]: non-user defined function '" <<
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

bool ClangInliner::checkTemplateInstantiation(const TemplateInstantiation &TI,
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
  // Disables inline expansion into #include files.
  Checkers.push_back([this](const TemplateInstantiation &TI,
      const InlineStackImpl &CallStack) {
    assert(CallStack.back()->mCallee == TI.mCaller &&
      "Function at the top of stack should make a call which is checked!");
    // We already made the check earlier.
    if (CallStack.size() > 1)
      return true;
    auto StartLoc = mSrcMgr.getDecomposedExpansionLoc(TI.mStmt->getLocStart());
    auto EndLoc = mSrcMgr.getDecomposedExpansionLoc(TI.mStmt->getLocEnd());
    assert(StartLoc.first == EndLoc.first &&
      "Statements which starts and ends in different files must be already discarded!");
    if (mSrcMgr.getDecomposedIncludedLoc(StartLoc.first).first.isValid() ||
        mSrcMgr.getDecomposedIncludedLoc(EndLoc.first).first.isValid()) {
      toDiag(mSrcMgr.getDiagnostics(), TI.mCallExpr->getLocStart(),
        diag::warn_disable_inline_in_include);
      return false;
    }
    return true;
  });
  // Checks that external dependencies are available at the call location.
  Checkers.push_back([this](const TemplateInstantiation &TI,
      const InlineStackImpl &CallStack) {
    assert(CallStack.back()->mCallee == TI.mCaller &&
      "Function at the top of stack should make a call which is checked!");
    auto isInAnyForwardDecls = [&CallStack](
      const GlobalInfoExtractor::OutermostDecl *FD) {
      for (auto *Caller : CallStack)
        if (Caller->mCallee->getForwardDecls().count(FD))
          return true;
      return false;
    };
    auto isInAvailableForwardDecl = [this](std::pair<FileID, unsigned> Bound,
        const GlobalInfoExtractor::OutermostDecl *FD) {
      for (auto *Redecl : FD->getRoot()->redecls()) {
        auto FDLoc = mSrcMgr.getDecomposedExpansionLoc(Redecl->getLocEnd());
        while (FDLoc.first.isValid() && FDLoc.first != Bound.first)
          FDLoc = mSrcMgr.getDecomposedIncludedLoc(FDLoc.first);
        if (FDLoc.first.isValid() && FDLoc.second < Bound.second)
          return true;
        /// Importer merges imported external declarations to the existing one:
        /// Import(FileID of From) != FileID of To
        /// So, it is not possible to find include which makes the From location
        /// visible at some point. Therefore we use additional information about
        /// import process (if it is available).
        auto RedeclLocItr =
          mImportInfo.RedeclLocs.find(Redecl->getLocEnd().getRawEncoding());
        if (RedeclLocItr != mImportInfo.RedeclLocs.end())
          for (auto RedeclLoc : RedeclLocItr->second) {
            auto FDLoc = mSrcMgr.getDecomposedExpansionLoc(RedeclLoc);
            while (FDLoc.first.isValid() && FDLoc.first != Bound.first)
              FDLoc = mSrcMgr.getDecomposedIncludedLoc(FDLoc.first);
            if (FDLoc.first.isValid() && FDLoc.second < Bound.second)
              return true;
          }
      }
      return false;
    };
    auto checkFD = [this, &TI, &isInAnyForwardDecls, &isInAvailableForwardDecl](
        std::pair<FileID, unsigned> Bound,
        const GlobalInfoExtractor::OutermostDecl *FD) {
      if (isInAnyForwardDecls(FD))
        return true;
      if (isInAvailableForwardDecl(Bound, FD))
        return true;
      toDiag(mSrcMgr.getDiagnostics(), TI.mCallExpr->getLocStart(),
        diag::warn_disable_inline);
      toDiag(mSrcMgr.getDiagnostics(), FD->getDescendant()->getLocation(),
        diag::note_inline_unresolvable_extern_dep);
      return false;
    };
    auto TargetFuncStart = mSrcMgr.getDecomposedExpansionLoc(
      CallStack.front()->mCallee->getFuncDecl()->getLocStart());
    for (auto FD : TI.mCallee->getForwardDecls())
      if (!checkFD(TargetFuncStart, FD))
        return false;
    for (auto FD : TI.mCallee->getMayForwardDecls())
      if (!checkFD(TargetFuncStart, FD))
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
  // We perform conservative search of external dependencies and macros for
  // each function. Functions from system library will be ignored. If there is
  // a global declaration with a name equal to an identifier and location of
  // this identifier has not be visited in TraverseDecl(D),
  // we conservatively assume dependence from this declaration.
  // We also collects all raw identifiers mentioned in the body of each
  // user-defined function.
  // We also ignores functions with macro in body or functions with bounds in
  // different files.
  for (auto *D : mContext.getTranslationUnitDecl()->decls()) {
    if (!isa<FunctionDecl>(D))
      continue;
    mDeclRefLoc.clear();
    mCurrentT = nullptr;
    TraverseDecl(D);
    // Some declarations may be ignored, and mCurrentT will be null.
    // For example, forward declarations which are not definitions are ignored.
    if (!mCurrentT)
      continue;
    if (mSrcMgr.getFileCharacteristic(mCurrentT->getFuncDecl()->getLocStart())
        != clang::SrcMgr::C_User)
      continue;
    if (mCurrentT->isMacroInDecl())
      continue;
    auto ExpRange =
      mSrcMgr.getExpansionRange(mCurrentT->getFuncDecl()->getSourceRange());
    if (!mSrcMgr.isWrittenInSameFile(ExpRange.getBegin(), ExpRange.getEnd()))
      continue;
    LocalLexer Lex(ExpRange, mSrcMgr, mLangOpts);
    mCurrentT->setKnownMayForwardDecls();
    while (true) {
      Token Tok;
      if (Lex.LexFromRawLexer(Tok))
        break;
      if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
        auto MacroLoc = Tok.getLocation();
        Lex.LexFromRawLexer(Tok);
        if (Tok.getRawIdentifier() != "pragma")
          mCurrentT->setMacroInDecl(MacroLoc);
        continue;
      }
      if (Tok.isNot(tok::raw_identifier))
        continue;
      if (!mDeclRefLoc.count(Tok.getLocation().getRawEncoding())) {
        // We conservatively check that function does not contain any macro
        // names available in translation unit. If this function should be
        // inlined we should check that after inlining some of local identifiers
        // will not be a macro.
        // Example:
        //   void f() { ... X ... }
        //   #define X ...
        //   void f1() { f(); }
        // In this case `X` will be a macro after inlining of f(), so it is not
        // possible to inline f().
        // For this check it is necessary to process all locations (instead of
        // only not visited). However, such check is too conservative and
        // can be done after transformation (`assert nomacro` can be used).
        auto MacroItr = mRawInfo.Macros.find(Tok.getRawIdentifier());
        if (MacroItr != mRawInfo.Macros.end())
          mCurrentT->setMacroInDecl(Tok.getLocation(), MacroItr->second);
        if (Tok.getRawIdentifier() == mCurrentT->getFuncDecl()->getName())
          continue;
        // If declaration at this location has not been found previously it is
        // necessary to conservatively check that it does not produce external
        // dependence.
        auto GlobalItr = mGIE.getOutermostDecls().find(Tok.getRawIdentifier());
        if (GlobalItr != mGIE.getOutermostDecls().end()) {
          for (auto &D : GlobalItr->second) {
            // We want to search dependence using a pointer to declaration. So,
            // use the first redeclaration and store the same pointers for all
            // redeclarations.
            auto *ND = cast<NamedDecl>(*D.getDescendant()->redecls_begin());
            auto *OD = mGIE.findOutermostDecl(ND);
            assert(OD && "Seems that redeclaration is not presented in AST");
            if (mCurrentT->getForwardDecls().count(OD))
              continue;
            mCurrentT->addMayForwardDecl(OD);
            LLVM_DEBUG(dbgs() << "[INLINE]: potential external declaration for '" <<
              mCurrentT->getFuncDecl()->getName() << "' found '" <<
              D.getDescendant()->getName() << "'\n");
            LLVM_DEBUG(dbgs() << "[INLINE]: reference to '" <<
              D.getDescendant()->getName() << "' in '" <<
              mCurrentT->getFuncDecl()->getName() << "' at ";
              Tok.getLocation().dump(mSrcMgr); dbgs() << "\n");
          }
        }
      }
    }
  }
  checkTemplates(getTemplateCheckers());
  LLVM_DEBUG(templatesInfoLog(mTs, mSrcMgr, mLangOpts));
  CallGraph CG;
  CG.TraverseDecl(mContext.getTranslationUnitDecl());
  ReversePostOrderTraversal<CallGraph *> RPOT(&CG);
  auto TICheckers = getTemplatInstantiationCheckers();
  Rewriter::RewriteOptions RemoveEmptyLine;
  RemoveEmptyLine.RemoveLineIfEmpty = true;
  for (auto I = RPOT.begin(), EI = RPOT.end(); I != EI; ++I) {
    if (!(*I)->getDecl() || !isa<FunctionDecl>((*I)->getDecl()))
      continue;
    const auto *Definition = cast<FunctionDecl>((*I)->getDecl());
    if (!Definition->hasBody(Definition))
      continue;
    auto CallsItr = mTs.find(Definition);
    if (CallsItr == mTs.end())
      continue;
    if (CallsItr->second->getCalls().empty()) {
      for (auto SR : CallsItr->second->getToRemove())
        mRewriter.RemoveText(SR, RemoveEmptyLine);
      continue;
    }
    LLVM_DEBUG(dbgs() << "[INLINE]: inline calls from '" << Definition->getName() << "'\n");
    SmallVector<const TemplateInstantiation *, 8> CallStack;
    // We create a bogus template instantiation to identify a root of call graph
    // subtree which should be inlined.
    TemplateInstantiation BogusTI { nullptr, nullptr, nullptr,
      mTs[CallsItr->first].get(), TemplateInstantiation::DefaultFlags };
    CallStack.push_back(&BogusTI);
    // In case of statements with multiple calls we accumulate inlining result
    // in a separate strings and then update a source code.
    std::string InsertBeforeStmt, InsertAfterStmt;
    const Stmt *CurrStmt = nullptr;
    bool NeedReplace = false;
    for (auto &TI : CallsItr->second->getCalls()) {
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
        if (TI.mFlags & TemplateInstantiation::IsNeedBraces)
          Text.first = "{" + Text.first;
        auto BeforeLoc = mSrcMgr.getFileLoc(TI.mStmt->getLocStart());
        mRewriter.InsertTextAfter(BeforeLoc.getLocWithOffset(-1),
          ("/* " + CallExpr + " is inlined below */\n" + Text.first).str());
        if (TI.mFlags & TemplateInstantiation::IsNeedBraces)
          mRewriter.InsertTextAfter(
            mSrcMgr.getFileLoc(TI.mStmt->getLocEnd()), "}");
        if (TI.mFlags & TemplateInstantiation::IsNeedBraces)
          InsertAfterStmt += "}";
      } else {
        mRewriter.ReplaceText(getFileRange(TI.mStmt),
          ("/* " + CallExpr + " is inlined below */\n" + Text.first).str());
        Token SemiTok;
        if (!getRawTokenAfter(mSrcMgr.getFileLoc(TI.mStmt->getLocEnd()),
          mSrcMgr, mLangOpts, SemiTok) && SemiTok.is(tok::semi))
          mRewriter.RemoveText(SemiTok.getLocation(), RemoveEmptyLine);
      }
    }
    for (auto SR : CallsItr->second->getToRemove())
      mRewriter.RemoveText(SR, RemoveEmptyLine);
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
    mRawInfo.Identifiers.count((Prefix + Twine(Count)).toStringRef(Out));
    ++Count, Out.clear());
  mRawInfo.Identifiers.insert(StringRef(Out.data(), Out.size()));
}
