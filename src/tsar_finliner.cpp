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
#include "tsar_pass_provider.h"
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

char FunctionInlinerImmutableWrapper::ID = 0;
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

ModulePass* createFunctionInlinerPass(
  std::vector<std::unique_ptr<clang::SPFPragmaHandler>>&& Handlers) {
  return new FunctionInlinerPass(std::move(Handlers));
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

std::vector<clang::Token> FInliner::getRawTokens(
    const clang::SourceRange& SR) const {
  // these positions are beginings of tokens
  // should include upper bound to capture last token
  unsigned int Offset = SR.getBegin().getRawEncoding();
  unsigned int Length = Offset
    + (getSourceText(SR).size() > 0 ? getSourceText(SR).size() - 1 : 0);
  std::vector<clang::Token> Tokens;
  for (unsigned int Pos = Offset; Pos <= Length;) {
    clang::SourceLocation Loc;
    clang::Token Token;
    Loc = clang::Lexer::GetBeginningOfToken(Loc.getFromRawEncoding(Pos),
      mSourceManager, mRewriter.getLangOpts());
    if (clang::Lexer::getRawToken(Loc, Token, mSourceManager,
      mRewriter.getLangOpts(), false)) {
      ++Pos;
      continue;
    }
    if (Token.getKind() != clang::tok::raw_identifier) {
      Pos += std::max(1u, (Token.isAnnotation() ? 1u : Token.getLength()));
      continue;
    }
    // avoid duplicates for same token
    if (Tokens.empty()
      || Tokens[Tokens.size() - 1].getLocation() != Token.getLocation()) {
      Tokens.push_back(Token);
    }
    Pos += Token.getLength();
  }
  return Tokens;
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
  FInliner Inliner(TfmCtx, mPragmaHandlers);
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


bool operator<=(
  const clang::SourceLocation& LSL, const clang::SourceLocation& RSL) {
  return LSL < RSL || LSL == RSL;
}

template<>
struct std::less<clang::SourceRange> {
  bool operator()(clang::SourceRange LSR, clang::SourceRange RSR) {
    return LSR.getBegin() == RSR.getBegin()
      ? LSR.getEnd() < RSR.getEnd() : LSR.getBegin() < RSR.getBegin();
  }
};

bool FInliner::VisitFunctionDecl(clang::FunctionDecl* FD) {
  if (FD->isThisDeclarationADefinition() == false) {
    return true;
  }
  mCurrentFD = FD;
  // build CFG for function which _possibly_ contains calls of functions
  // which can be inlined
  std::unique_ptr<clang::CFG> CFG = clang::CFG::buildCFG(
    nullptr, FD->getBody(), &mContext, clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + mCurrentFD->getName()).str().data());
  
  // get unreachable blocks for later return statements exclusion
  auto isPred = [](const clang::CFGBlock* BL, const clang::CFGBlock* BR)     
    -> bool {
    for (auto B : BR->preds()) {
      if (B == BL) {
        return true;
      }
    }
    return false;
  };
  std::set<const clang::CFGBlock*> ReachableBlocks;
  ReachableBlocks.insert(&CFG->getEntry());
  bool Changed = true;
  while (Changed) {
    auto NewReachableBlocks(ReachableBlocks);
    for (auto RB : ReachableBlocks) {
      for (auto B : *CFG) {
        if (isPred(RB, B)) {
          NewReachableBlocks.insert(B);
        }
      }
    }
    ReachableBlocks.swap(NewReachableBlocks);
    Changed = ReachableBlocks != NewReachableBlocks;
  }
  for (auto B : *CFG) {
    if (ReachableBlocks.find(B) == std::end(ReachableBlocks)) {
      for (auto I : *B) {
        if (llvm::Optional<clang::CFGStmt> CS = I.getAs<clang::CFGStmt>()) {
          mUnreachableStmts[mCurrentFD].insert(CS->getStmt());
        }
      }
    }
  }
  mTs[mCurrentFD].setSingleReturn(!(CFG->getExit().pred_size() > 1));
  mTs[mCurrentFD].setFuncDecl(mCurrentFD);

  auto isSubStmt = [this](const clang::Stmt* P, const clang::Stmt* S) -> bool {
    clang::SourceLocation BeginP
      = getLoc(P->getSourceRange().getBegin());
    clang::SourceLocation EndP
      = getLoc(P->getSourceRange().getEnd());
    clang::SourceLocation BeginS
      = getLoc(S->getSourceRange().getBegin());
    clang::SourceLocation EndS
      = getLoc(S->getSourceRange().getEnd());
    return BeginS <= BeginP && EndP <= EndS;
  };
  auto& TIs = mTIs[mCurrentFD];
  for (auto B : *CFG) {
    for (auto I1 = B->begin(); I1 != B->end(); ++I1) {
      if (llvm::Optional<clang::CFGStmt> CS = I1->getAs<clang::CFGStmt>()) {
        const clang::Stmt* S = CS->getStmt();
        if (const clang::CallExpr* CE = clang::dyn_cast<clang::CallExpr>(S)) {
          const clang::FunctionDecl* Definition = nullptr;
          CE->getDirectCallee()->hasBody(Definition);
          if (Definition == nullptr) {
            continue;
          }
          mTs[Definition].setFuncDecl(Definition);
          const clang::Stmt* P = S;
          for (auto I2 = I1 + 1; I2 != B->end(); ++I2) {
            if (llvm::Optional<clang::CFGStmt> CS
              = I2->getAs<clang::CFGStmt>()) {
              if (isSubStmt(P, CS->getStmt())) {
                P = CS->getStmt();
              }
            }
          }
          for (auto B : B->succs()) {
            if (S = B->getTerminator()) {
              if (clang::isa<clang::ForStmt>(S) && isSubStmt(P, S)) {
                P = S;
              }
            }
          }
          bool inCondOp = false;
          while (true) {
            S = P;
            for (auto B1 = CFG->begin(); B1 != CFG->end(); ++B1) {
              for (auto I1 = (*B1)->begin(); I1 != (*B1)->end(); ++I1) {
                if (llvm::Optional<clang::CFGStmt> CS
                  = I1->getAs<clang::CFGStmt>()) {
                  if (isSubStmt(P, CS->getStmt())) {
                    P = CS->getStmt();
                    // check if we are in ternary if
                    if (clang::isa<clang::ConditionalOperator>(CS->getStmt())) {
                      inCondOp = true;
                    }
                  }
                }
              }
            }
            if (P == S) {
              break;
            }
          }
          // don't replace function calls in conditional operator (ternary if,
          // ?:)
          if (inCondOp) {
            continue;
          }
          // don't replace function calls in condition expressions of loops
          if (S = B->getTerminator()) {
            if (isSubStmt(P, S)) {
              if (clang::isa<clang::ForStmt>(S) == true
                || clang::isa<clang::WhileStmt>(S) == true
                || clang::isa<clang::DoStmt>(S) == true) {
                continue;
              } else {
                P = S;
              }
            }
          }
          // don't replace function calls in the third section of for-loop
          if (B->getLoopTarget() != nullptr) {
            continue;
          }
          for (auto& stmtPair : CFG->synthetic_stmts()) {
            if (stmtPair.first == P) {
              P = stmtPair.second;
              break;
            }
          }
          TemplateInstantiation TI = { mCurrentFD, P, CE, nullptr };
          if (std::find(std::begin(TIs), std::end(TIs), TI) == std::end(TIs)) {
            TIs.push_back(TI);
          }
        }
      }
    }
  }
  return true;
}

bool FInliner::VisitReturnStmt(clang::ReturnStmt* RS) {
  mTs[mCurrentFD].addRetStmt(RS);
  return true;
}

bool FInliner::VisitExpr(clang::Expr* E) {
  mExprs[mCurrentFD].insert(E);
  // parameter reference
  if (clang::DeclRefExpr* DRE = clang::dyn_cast<clang::DeclRefExpr>(E)) {
    if (clang::ParmVarDecl* PVD
      = clang::dyn_cast<clang::ParmVarDecl>(DRE->getDecl())) {
      mTs[mCurrentFD].addParmRef(PVD, DRE);
    }
  }
  return true;
}

bool FInliner::VisitCompoundStmt(clang::CompoundStmt* CS) {
  auto Loc = clang::SourceLocation::getFromRawEncoding(
    mSourceManager.getFileOffset(CS->getLocStart()));
  for (auto& PH : mPragmaHandlers) {
    if (PH->isPragma(Loc)) {
      mInlineStmts[mCurrentFD].insert(CS);
      break;
    }
  }
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
    for (auto D : mForwardDecls[SrcFD]) {
      Context += getSourceText(getRange(D)) + ";";
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
    for (auto& TI : mTIs[SrcFD]) {
      if (TI.mTemplate == nullptr
        || TI.mTemplate->getFuncDecl() == nullptr) {
        continue;
      }
      if (MatchedTIs.size() > 0) {
        if (MatchedTIs.find(&TI) == std::end(MatchedTIs)) {
          continue;
        }
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

std::set<std::string> FInliner::getIdentifiers(const clang::Decl* D) const {
  std::set<std::string> Identifiers;
  if (const clang::TagDecl* TD = clang::dyn_cast<clang::TagDecl>(D)) {
    Identifiers = std::move(getIdentifiers(TD));
  } else if (const clang::FunctionDecl* FD
    = clang::dyn_cast<clang::FunctionDecl>(D)) {
    Identifiers.insert(FD->getName());
  } else {
    auto DC = D->getDeclContext();
    std::map<const clang::DeclContext*, std::set<std::string>> DCIdentifiers;
    while (DC) {
      for (auto D : DC->decls()) {
        if (const clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(D)) {
          DCIdentifiers[DC].insert(ND->getName());
        }
      }
      DC = DC->getParent();
    }
    for (auto& T : getRawTokens(getRange(D))) {
      auto RawIdentifier = T.getRawIdentifier();
      if (std::find(std::begin(mKeywords), std::end(mKeywords), RawIdentifier)
        != std::end(mKeywords)) {
        continue;
      }
      if (const clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(D)) {
        if (ND->getName() == RawIdentifier) {
          Identifiers.insert(ND->getName());
          continue;
        }
      }
      // match token with outer scopes' declarations
      const clang::DeclContext* DC = D->getDeclContext();
      while (DC) {
        if (DCIdentifiers[DC].find(RawIdentifier)
          != std::end(DCIdentifiers[DC])) {
          break;
        } else {
          DC = DC->getParent();
        }
      }
      if (DC != nullptr) {
        Identifiers.insert(RawIdentifier);
      }
    }
  }
  return Identifiers;
}

std::set<std::string> FInliner::getIdentifiers(const clang::TagDecl* TD) const {
  std::set<std::string> Identifiers;
  Identifiers.insert(TD->getName());
  for (auto D : TD->decls()) {
    std::set<std::string> Tmp(getIdentifiers(D));
    Identifiers.insert(std::begin(Tmp), std::end(Tmp));
  }
  return Identifiers;
}

void FInliner::HandleTranslationUnit(clang::ASTContext& Context) {
  TraverseDecl(Context.getTranslationUnitDecl());
  //Context.getTranslationUnitDecl()->decls_begin();
  // associate instantiations with templates
  std::set<const clang::FunctionDecl*> Callable;
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      const clang::FunctionDecl* Definition = nullptr;
      TI.mCallExpr->getDirectCallee()->hasBody(Definition);
      TI.mTemplate = &mTs.at(Definition);
      Callable.insert(Definition);
    }
  }
  // match pragmas and calls
  for (auto& TIs : mTIs) {
    if (mInlineStmts[TIs.first].size() > 0) {
      // handle only correct pragmas
      //assert(mInlineStmts[TIs.first].size() <= TIs.second.size());
      for (auto S : mInlineStmts[TIs.first]) {
        auto Pos = S->getLocStart().getRawEncoding()
          + getSourceText(getRange(S)).size();
        auto Loc = clang::SourceLocation::getFromRawEncoding(Pos);
        clang::Token Token;
        clang::Lexer::getRawToken(Loc, Token, mSourceManager,
          mRewriter.getLangOpts(), true);
        bool Found = false;
        for (auto& TI : TIs.second) {
          if (TI.mStmt->getLocStart() == Token.getLocation()) {
            Found = true;
            MatchedTIs.insert(&TI);
          }
        }
        if (!Found) {
          auto Loc = S->getLocStart();
          auto FID = mSourceManager.getFileID(Loc);
          auto Filename = mSourceManager.getFilename(Loc);
          auto Offset = mSourceManager.getFileOffset(Loc);
          auto LineNum = mSourceManager.getLineNumber(FID, Offset);
          auto ColumnNum = mSourceManager.getColumnNumber(FID, Offset);
          llvm::dbgs() << "Unmatched #pragma at " << Filename << ':'
            << LineNum << ':' << ColumnNum << ", ignored" << '\n';
        }
      }
    }
  }
  // global declarations (outermost, max enclosed)
  // possible scopes C99: function, function prototype, file, block
  // decl contexts C99: TranslationUnitDecl, FunctionDecl, TagDecl, BlockDecl
  // only TagDecl should be traversed because it doesn't produce own scope
  auto isSubDecl = [this](const clang::Decl* P, const clang::Decl* S) -> bool {
    clang::SourceLocation BeginP
      = getLoc(P->getSourceRange().getBegin());
    clang::SourceLocation EndP
      = getLoc(P->getSourceRange().getEnd());
    clang::SourceLocation BeginS
      = getLoc(S->getSourceRange().getBegin());
    clang::SourceLocation EndS
      = getLoc(S->getSourceRange().getEnd());
    return BeginS <= BeginP && EndP <= EndS;
  };
  std::set<const clang::Decl*> GlobalDecls;
  for (auto D : Context.getTranslationUnitDecl()->decls()) {
    GlobalDecls.insert(D);
  }
  std::map<const clang::Decl*, std::set<const clang::Decl*>> NestedDecls;
  for (auto D : Context.getTranslationUnitDecl()->decls()) {
    for (auto it = std::begin(GlobalDecls); it != std::end(GlobalDecls);) {
      if (*it != D && isSubDecl(*it, D)) {
        // strong correlation with ExternalDepsChecker
        // allow only vardecl groups
        if (D->getKind() != (*it)->getKind()) {
          NestedDecls[D].insert(*it);
        }
        it = GlobalDecls.erase(it);
      } else {
        ++it;
      }
    }
  }
  // mOutermostDecls - have most outer DeclContext
  for (auto D : Context.getTranslationUnitDecl()->decls()) {
    std::set<std::string> Tmp(getIdentifiers(D));
    for (auto Identifier : Tmp) {
      mOutermostDecls[Identifier].insert(D);
    }
    mGlobalIdentifiers.insert(std::begin(Tmp), std::end(Tmp));
  }
  // identifiers in scope
  for (auto& TIs : mTIs) {
    for (auto D : TIs.first->decls()) {
      std::set<std::string> Tmp(getIdentifiers(D));
      mExtIdentifiers[TIs.first].insert(std::begin(Tmp), std::end(Tmp));
      mIntIdentifiers[TIs.first].insert(std::begin(Tmp), std::end(Tmp));
    }
  }
  for (auto& T : mTs) {
    for (auto D : T.first->decls()) {
      std::set<std::string> Tmp(getIdentifiers(D));
      mExtIdentifiers[T.first].insert(std::begin(Tmp), std::end(Tmp));
      mIntIdentifiers[T.first].insert(std::begin(Tmp), std::end(Tmp));
    }
  }
  // compute recursive functions set
  std::set<const clang::FunctionDecl*> Recursive;
  for (auto& TIs : mTIs) {
    bool OK = true;
    std::set<const clang::FunctionDecl*> Callers = {TIs.first};
    std::set<const clang::FunctionDecl*> Callees;
    for (auto& TIs : TIs.second) {
      if (TIs.mTemplate != nullptr && TIs.mTemplate->getFuncDecl() != nullptr) {
        Callees.insert(TIs.mTemplate->getFuncDecl());
      }
    }
    while (OK == true && Callees.size() != 0) {
      std::set<const clang::FunctionDecl*> Intersection;
      std::set_intersection(std::begin(Callers), std::end(Callers),
        std::begin(Callees), std::end(Callees),
        std::inserter(Intersection, std::end(Intersection)));
      if (Intersection.size() != 0) {
        OK = false;
        break;
      } else {
        std::set<const clang::FunctionDecl*> NewCallees;
        for (auto& Caller : Callees) {
          for (auto& TI : mTIs[Caller]) {
            if (TI.mTemplate != nullptr
              && TI.mTemplate->getFuncDecl() != nullptr) {
              NewCallees.insert(TI.mTemplate->getFuncDecl());
            }
          }
        }
        Callees.swap(NewCallees);
      }
    }
    if (OK == false) {
      Recursive.insert(TIs.first);
    }
  }

  // validate source ranges in user files
  // no one-token declarations exist in C, except labels
  // TODO: should traverse TagDecls?
  std::set<const clang::Decl*> BogusDecls;
  for (auto D : Context.getTranslationUnitDecl()->decls()) {
    if (D->getLocStart().isValid()
      && mSourceManager.getFileCharacteristic(D->getLocStart())
      != clang::SrcMgr::C_User) {
      // silently skip because we are not going to instantiate functions from
      // standard libraries
      continue;
    }
    auto R = getRange(D);
    if (R.isValid()
      && R.getBegin().getRawEncoding() == R.getEnd().getRawEncoding()) {
      llvm::dbgs() << "Bogus source range found at "
        << R.getBegin().printToString(mSourceManager) << '\n';
      BogusDecls.insert(D);
    }
  }

  // get external dependencies (entities defined in outer scope)
  // [C99 6.2.1] identifier can denote: object, function, tag/member of
  // struct/union/enum, typedef name, label name, macro name, macro parameter.
  // Label name - by definition has function scope, macro' objects should be
  // processed during preprocessing stage. Other cases are handled below.

  // unfortunately it is impossible to get subtypes of any type
  // (that's the difference between llvm::Type and clang::Type)
  // only way is to exclude corresponding identifier names
  // correctly merged input AST guarantees unambiguity of global identifiers
  // logic: just collect all global identifiers for context
  // even if we have same identifiers locally, they will hide global ones
  // these global declarations become unused
  for (auto& T : mTs) {
    std::set<std::string>& Identifiers = mExtIdentifiers[T.first];
    // intersect local external references in decls with global symbols
    std::set<std::string> ExtIdentifiers;
    std::set_intersection(std::begin(Identifiers), std::end(Identifiers),
      std::begin(mGlobalIdentifiers), std::end(mGlobalIdentifiers),
      std::inserter(ExtIdentifiers, std::end(ExtIdentifiers)));
    Identifiers.swap(ExtIdentifiers);
    for (auto E : mExprs[T.first]) {
      for (auto T : getRawTokens(getRange(E))) {
        Identifiers.insert(T.getRawIdentifier());
      }
    }
    ExtIdentifiers.clear();
    // intersect local external references in exprs with global symbols
    std::set_intersection(std::begin(Identifiers), std::end(Identifiers),
      std::begin(mGlobalIdentifiers), std::end(mGlobalIdentifiers),
      std::inserter(ExtIdentifiers, std::end(ExtIdentifiers)));
    Identifiers.swap(ExtIdentifiers);
  }
  // mForwardDecls - all referenced external declarations including transitive
  // dependencies
  for (auto& T : mTs) {
    std::set<std::string>& Identifiers = mExtIdentifiers[T.first];
    for (auto Identifier : Identifiers) {
      for (auto D : mOutermostDecls[Identifier]) {
        mForwardDecls[T.first].insert(D);
        if (const clang::FunctionDecl* FD
          = clang::dyn_cast<clang::FunctionDecl>(D)) {
          std::set<const clang::FunctionDecl*> Worklist;
          Worklist.insert(FD);
          while (!Worklist.empty()) {
            auto it = std::begin(Worklist);
            FD = *it;
            mForwardDecls[T.first].insert(FD);
            for (auto Identifier : Identifiers) {
              for (auto D : mOutermostDecls[Identifier]) {
                if (*it != FD
                  && (FD = clang::dyn_cast<clang::FunctionDecl>(D))) {
                  Worklist.insert(FD);
                } else {
                  mForwardDecls[T.first].insert(D);
                }
              }
            }
            Worklist.erase(FD);
          }
        }
      }
    }
  }
  // unique decls sharing same soure ranges
  for (auto& T : mTs) {
    auto& ForwardDecls = mForwardDecls[T.first];
    for (auto it = std::begin(ForwardDecls); it != std::end(ForwardDecls);) {
      bool Found = false;
      for (auto D : ForwardDecls) {
        if (*it != D && isSubDecl(*it, D)) {
          Found = true;
          break;
        }
      }
      if (Found) {
        it = ForwardDecls.erase(it);
      } else {
        ++it;
      }
    }
  }

  // all constraint checkers
  auto UnusedTemplateChecker = [&](const Template& T) -> std::string {
    return Callable.find(T.getFuncDecl()) == std::end(Callable)
      ? "Unused template for function \"" + T.getFuncDecl()->getName().str()
      + "\"\n" : "";
  };
  auto UserDefTChecker = [&](const Template& T) -> std::string {
    std::string Result;
    if (mSourceManager.getFileCharacteristic(T.getFuncDecl()->getLocStart())
      != clang::SrcMgr::C_User) {
      Result += "Non-user defined function \""
        + T.getFuncDecl()->getName().str() + "\" for instantiation\n";
    }
    return Result;
  };
  auto UserDefTIChecker = [&](const TemplateInstantiation& TI) -> std::string {
    std::string Result;
    if (mSourceManager.getFileCharacteristic(TI.mFuncDecl->getLocStart())
      != clang::SrcMgr::C_User) {
      Result += "Non-user defined function \""
        + TI.mFuncDecl->getName().str() + "\" for instantiating in\n";
    }
    return Result;
  };
  auto VariadicChecker = [&](const Template& T) -> std::string {
    return T.getFuncDecl()->isVariadic() ? "Variadic function" : "";
  };
  auto RecursiveChecker = [&](const Template& T) -> std::string {
    return Recursive.find(T.getFuncDecl()) != std::end(Recursive)
      ? "Recursive function \"" + T.getFuncDecl()->getNameAsString()
      + "\"\n" : "";
  };
  auto BogusSRChecker = [&](const Template& T) -> std::string {
    std::string Result;
    for (auto D : T.getFuncDecl()->decls()) {
      if (clang::isa<clang::LabelDecl>(D)) {
        continue;
      }
      if (mSourceManager.getFileCharacteristic(D->getLocStart())
        != clang::SrcMgr::C_User) {
        continue;
      }
      auto R = getRange(D);
      if (R.isValid()
        && R.getBegin().getRawEncoding() == R.getEnd().getRawEncoding()) {
        Result += "Bogus source range found at "
          + R.getBegin().printToString(mSourceManager) + "\n";
        BogusDecls.insert(D);
        BogusDecls.insert(T.getFuncDecl());
      }
    }
    return Result;
  };
  auto BogusSRTransitiveChecker = [&](const Template& T) -> std::string {
    std::set<const clang::Decl*> Intersection;
    std::set_intersection(std::begin(BogusDecls), std::end(BogusDecls),
      std::begin(mForwardDecls[T.getFuncDecl()]),
      std::end(mForwardDecls[T.getFuncDecl()]),
      std::inserter(Intersection, std::end(Intersection)));
    return Intersection.size() > 0
      ? "Transitive dependency on bogus declaration for function \""
      + T.getFuncDecl()->getNameAsString() + "\"\n" : "";
  };
  /*auto NonlocalExternalDepsChecker = [&](const Template& T) -> std::string {
    for (auto D : mForwardDecls[T.getFuncDecl()]) {
      if (mSourceManager.getFileID(D->getLocStart())
        != mSourceManager.getFileID(T.getFuncDecl()->getLocStart())) {
        return "Reference to nonlocal global declaration in function \""
          + T.getFuncDecl()->getNameAsString() + "\"\n";
      }
    }
    return "";
  };*/
  auto StaticExternalDepsChecker = [&](const Template& T) -> std::string {
    std::string Result;
    for (auto D : mForwardDecls[T.getFuncDecl()]) {
      const clang::VarDecl* VD = clang::dyn_cast<clang::VarDecl>(D);
      const clang::FunctionDecl* FD
        = clang::dyn_cast<clang::FunctionDecl>(D);
      if (VD && VD->getStorageClass() == clang::StorageClass::SC_Static) {
        Result += "Reference to static global declaration \""
          + VD->getName().str() + "\" in function \""
          + T.getFuncDecl()->getName().str() + "\"\n";
      } else if (FD && (FD->isInlineSpecified()
      || FD->getStorageClass() == clang::StorageClass::SC_Static)) {
        Result += "Reference to static/inline global declaration \""
          + FD->getName().str() + "\" in function \""
          + T.getFuncDecl()->getName().str() + "\"\n";
      }
    }
    return Result;
  };
  auto NestedExternalDepsChecker = [&](const Template& T) -> std::string {
    bool NestedDeps = false;
    const clang::Decl* NestedD = nullptr;
    for (auto D : mForwardDecls[T.getFuncDecl()]) {
      // strong correlation with ExternalDepsChecker
      // nested structs/unions are disallowed as forward declarations below
      if (!NestedDecls[D].empty()) {
        NestedDeps = true;
        NestedD = D;
        break;
      }
    }
    return NestedDeps ? "Reference to nested declaration in function \""
      + T.getFuncDecl()->getName().str() + "\"\n" : "";
  };
  // note: external dependencies include callees
  auto ExternalDepsChecker = [&](const TemplateInstantiation& TI)
    -> std::string {
    std::string Result = StaticExternalDepsChecker(*TI.mTemplate);
    if (Result != "") {
      return Result;
    }
    std::set<const clang::Decl*> AtLocVisibleDecls;
    // find external deps of preceding templates in same file
    // these decls are guaranteed to be visible for instantiation
    for (auto T : mTs) {
      auto SourceLoc = TI.mFuncDecl->getLocStart();
      auto TargetLoc = T.first->getLocStart();
      if (mSourceManager.getFileID(SourceLoc)
        == mSourceManager.getFileID(TargetLoc)
        && TargetLoc.getRawEncoding() <= SourceLoc.getRawEncoding()) {
        AtLocVisibleDecls.insert(std::begin(mForwardDecls[T.first]),
          std::end(mForwardDecls[T.first]));
      }
    }
    std::set<const clang::Decl*> NonSharedDecls;
    std::set_difference(std::begin(mForwardDecls[TI.mTemplate->getFuncDecl()]),
      std::end(mForwardDecls[TI.mTemplate->getFuncDecl()]),
      std::begin(AtLocVisibleDecls), std::end(AtLocVisibleDecls),
      std::inserter(NonSharedDecls, std::end(NonSharedDecls)));
    // function/var/typedef decls can be duplicated, if they are not definitions
    unsigned int unresolvedDeps
      = std::count_if(std::begin(NonSharedDecls), std::end(NonSharedDecls),
        [&](const clang::Decl* D) -> bool {
        return !(clang::isa<clang::FunctionDecl>(D)
          || clang::isa<clang::VarDecl>(D)
          || clang::isa<clang::TypedefDecl>(D));
    });
    Result = unresolvedDeps > 0 ?
      "Some external dependencies are not resolvable at destination position\n"
      : "";
    return Result;
  };
  auto CollidedIdentifiersChecker = [&](const TemplateInstantiation& TI)
    -> std::string {
    std::string Result;
    std::set<std::string> Intersection;
    std::set_intersection(std::begin(mIntIdentifiers[TI.mFuncDecl]),
      std::end(mIntIdentifiers[TI.mFuncDecl]),
      std::begin(mExtIdentifiers[TI.mTemplate->getFuncDecl()]),
      std::end(mExtIdentifiers[TI.mTemplate->getFuncDecl()]),
      std::inserter(Intersection, std::end(Intersection)));
    if (Intersection.size() > 0) {
      Result += "Potential identifier collision between template \""
        + TI.mFuncDecl->getName().str() + "\" and instantiation \""
        + getSourceText(getRange(TI.mCallExpr)) + "\" contexts: "
        + join(Intersection, ", ") + "\n";
    }
    return Result;
  };
  std::function<std::string(const Template&)> TChainChecker[] = {
    UnusedTemplateChecker,
    UserDefTChecker,
    VariadicChecker,
    RecursiveChecker,
    NestedExternalDepsChecker//,
    //BogusSRChecker,
    //BogusSRTransitiveChecker
  };
  for (auto& T : mTs) {
    if (T.second.getFuncDecl() == nullptr) {
      continue;
    }
    std::string Result;
    for (auto Checker : TChainChecker) {
      if (Result != "") break;
      Result += Checker(T.second);
    }
    if (Result != "") {
      T.second.setFuncDecl(nullptr);
      llvm::dbgs() << "Template \"" + T.first->getNameAsString()
        + "\" disabled due to constraint violations:\n" + Result;
    }
  }
  std::function<std::string(const TemplateInstantiation&)> TIChainChecker[] = {
    UserDefTIChecker,
    ExternalDepsChecker,
    CollidedIdentifiersChecker
  };
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      if (TI.mTemplate == nullptr || TI.mTemplate->getFuncDecl() == nullptr) {
        continue;
      }
      std::string Result;
      for (auto Checker : TIChainChecker) {
        if (Result != "") break;
        Result += Checker(TI);
      }
      if (Result != "") {
        TI.mTemplate = nullptr;
        llvm::dbgs() << "Template instantiation \""
          + getSourceText(getRange(TI.mCallExpr))
          + "\" disabled due to constraint violations:\n" + Result;
      }
    }
  }

  // info
  [&]() {
    llvm::dbgs() << '\n';
    llvm::dbgs() << "Total template instantiations:" << '\n';
    for (auto& TIs : mTIs) {
      if (TIs.second.size() == 0) {
        continue;
      }
      llvm::dbgs() << ' ' << "in " << '"' << TIs.first->getName()
        << '"' << ':' << '\n';
      for (auto& TI : TIs.second) {
        if (TI.mTemplate != nullptr) {
          llvm::dbgs() << "  " << '"'
            << getSourceText(getRange(TI.mCallExpr)) << '"' << '\n';
        }
      }
      llvm::dbgs() << '\n';
    }
    llvm::dbgs() << '\n';
    llvm::dbgs() << "Total templates:" << '\n';
    for (auto& T : mTs) {
      if (T.second.getFuncDecl() != nullptr) {
        llvm::dbgs() << ' ' << '"' << T.first->getName() << '"' << '\n';
      }
    }
    llvm::dbgs() << '\n';
    llvm::dbgs() << "Disabled templates ("
      << std::count_if(std::begin(mTs), std::end(mTs),
        [&](const std::pair<const clang::FunctionDecl*, Template>& lhs)
        -> bool {
      return lhs.second.getFuncDecl() == nullptr;
    }) << "):" << '\n';
    for (auto& T : mTs) {
      if (T.second.getFuncDecl() == nullptr) {
        llvm::dbgs() << ' ' << '"' << T.first->getName() << '"' << '\n';
      }
    }
    llvm::dbgs() << '\n';
    llvm::dbgs() << "Disabled template instantiations: ("
      << [&]() -> size_t {
      size_t s = 0;
      for (auto& TI : mTIs) {
        s += std::count_if(std::begin(TI.second), std::end(TI.second),
          [&](const TemplateInstantiation& lhs) -> bool {
          return lhs.mTemplate == nullptr;
        });
      }
      return s;
    }() << "):" << '\n';
    for (auto& TIs : mTIs) {
      if (TIs.second.size() == 0) {
        continue;
      }
      llvm::dbgs() << ' ' << "in " << '"' << TIs.first->getName()
        << '"' << ':' << '\n';
      for (auto& TI : TIs.second) {
        if (TI.mTemplate == nullptr || TI.mTemplate->getFuncDecl() == nullptr) {
          llvm::dbgs() << "  " << '"'
            << getSourceText(getRange(TI.mCallExpr)) << '"' << '\n';
        }
      }
      llvm::dbgs() << '\n';
    }
    llvm::dbgs() << '\n';
  }();

  // recursive instantiation
  for (auto& TIs : mTIs) {
    // unusable functions are those which are not instantiated
    // meaning they are on top of call hierarchy
    auto isOnTop
      = [&](const std::pair<const clang::FunctionDecl*, Template>& lhs)
      -> bool {
      return TIs.first == lhs.first && lhs.second.getFuncDecl() == nullptr;
    };
    if (std::find_if(std::begin(mTs), std::end(mTs), isOnTop)
      != std::end(mTs)) {
      bool PCHeader = true;  // seems ExternalDepsChecker filters such cases out
      for (auto& TI : TIs.second) {
        if (TI.mTemplate == nullptr
          || TI.mTemplate->getFuncDecl() == nullptr) {
          continue;
        }
        if (MatchedTIs.size() > 0) {
          if (MatchedTIs.find(&TI) == std::end(MatchedTIs)) {
            continue;
          }
        }
        if (!PCHeader) {
          // strong correlation with ExternalDepsChecker
          PCHeader = !PCHeader;
          for (auto D : mForwardDecls[TI.mFuncDecl]) {
            if (const clang::FunctionDecl* FD
              = clang::dyn_cast<clang::FunctionDecl>(D)) {
              if (FD->hasBody()) {
                // it's function definition, take part before body
                auto SignatureLength
                  = FD->getBody()->getLocStart().getRawEncoding()
                  - FD->getLocStart().getRawEncoding();
                mRewriter.InsertTextBefore(TI.mFuncDecl->getLocStart(),
                  getSourceText(getRange(FD)).substr(0, SignatureLength) + ";");
              } else {
                // it's function declaration
                mRewriter.InsertTextBefore(TI.mFuncDecl->getLocStart(),
                  getSourceText(getRange(FD)) + ";");
              }
            } else if (const clang::VarDecl* VD
              = clang::dyn_cast<clang::VarDecl>(D)) {
              // var declaration, take part before initializer (if any)
              auto Text = getSourceText(getRange(VD));
              Text = Text.substr(0, Text.find('=') == std::string::npos
                ? Text.size() : Text.find('='));
              mRewriter.InsertTextBefore(TI.mFuncDecl->getLocStart(),
                Text + ";");
            } else if (const clang::TypedefDecl* TD
              = clang::dyn_cast<clang::TypedefDecl>(D)) {
              // typedef can be repeated any number of times
              // if it doesn't have nested decls
              mRewriter.InsertTextBefore(TI.mFuncDecl->getLocStart(),
                getSourceText(getRange(TD)) + ";");
            }
          }
        }
        std::set<std::string>& LocalDecls = mIntIdentifiers[TI.mFuncDecl];
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

template<typename T>
clang::SourceRange FInliner::getRange(T* node) const {
  return{ mSourceManager.getFileLoc(node->getSourceRange().getBegin()),
    mSourceManager.getFileLoc(node->getSourceRange().getEnd()) };
}

clang::SourceLocation FInliner::getLoc(clang::SourceLocation SL) const {
  return mSourceManager.getFileLoc(SL);
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
  Identifiers.insert(std::begin(mGlobalIdentifiers),
    std::end(mGlobalIdentifiers));
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
  Passes.add(createFunctionInlinerPass(std::move(mPragmaHandlers)));
  Passes.run(*M);
  return;
}
