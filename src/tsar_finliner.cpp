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
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

// 05.03 TODO(jury.zykov@yandex.ru): handle case when local declarations hide outer (f.e., f1 with declaration 'int s' calls f2 which references outer 'char s[]')
// (?) two solutions:
//   conservative: don't inline functions whose references to outer declarations become 'hidden' after inlining
//   or transformation: as preprocess (pass) - make all functions conform rule 'no hidden outer declarations' (renaming local decls/refs through rewriter)
// renaming requires preprocessor (for mapping from source locs to IdentifierInfo, which can be later matched with NamedDecl, thus giving correct mapping
// of _all_ identifier references, which AST lacks of)
// getRawToken doesn't fill IdentifierInfo in raw lexer mode
// 05.03 TODO(jury.zykov@yandex.ru): gen forward declarations for external dependencies (per-node-specific, handle cases with statics and same/different TU)
// 05.03 TODO(jury.zykov@yandex.ru): ternary ifstmt - rewrite as simple ifstmt (transformation) or disable inlining (conservative) - currently conservative
// 12.03 TODO(jury.zykov@yandex.ru): pragma handler pass for inlining


// 26.03 TODO(jury.zykov@yandex.ru): copy propagation/elimination pass

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

ModulePass* llvm::createFunctionInlinerPass() {
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
    FormatStyle, ChangedCode.get(), Ranges, SM.getFileEntryForID(FID)->getName());
  Replaces = Replaces.merge(FormatChanges);
  clang::tooling::applyAllReplacements(Replaces, Rewriter);
  return false;
}

std::vector<clang::Token> FInliner::getRawTokens(
    const clang::SourceRange& SR) const {
  // these positions are beginings of tokens
  // should include upper bound to capture last token
  unsigned int Offset = SR.getBegin().getRawEncoding();
  unsigned int Length = SR.getEnd().getRawEncoding();
  std::vector<clang::Token> Tokens;
  for (unsigned int i = Offset; i <= Length; ++i) {
    clang::SourceLocation Loc;
    clang::Token Token;
    Loc = clang::Lexer::GetBeginningOfToken(Loc.getFromRawEncoding(i),
      mSourceManager, mRewriter.getLangOpts());
    if (clang::Lexer::getRawToken(Loc, Token, mSourceManager,
      mRewriter.getLangOpts(), false)) {
      continue;
    }
    if (Token.getKind() != clang::tok::raw_identifier) {
      continue;
    }
    // avoid duplicates for same token
    if (!Tokens.empty()
      && Tokens[Tokens.size() - 1].getLocation() == Token.getLocation()) {
      continue;
    } else {
      Tokens.push_back(Token);
    }
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
  FInliner inliner(TfmCtx);
  inliner.HandleTranslationUnit(Context);
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
    llvm::errs() << "All changes were successfully saved" << '\n';
  }
  return false;
}


bool operator<=(
  const clang::SourceLocation& lhs, const clang::SourceLocation& rhs) {
  return lhs < rhs || lhs == rhs;
}

template<>
struct std::less<clang::SourceRange> {
  bool operator()(clang::SourceRange lhs, clang::SourceRange rhs) {
    return lhs.getBegin() == rhs.getBegin()
      ? lhs.getEnd() < rhs.getEnd() : lhs.getBegin() < rhs.getBegin();
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
  //CFG->dump(mContext.getLangOpts(), true);
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + mCurrentFD->getName()).str().data());
  
  auto isPred = [this](const clang::CFGBlock* BL, const clang::CFGBlock* BR) -> bool {
    for (auto B : BR->preds()) {
      if (B == BL) {
        return true;
      }
    }
    return false;
  };
  std::set<const clang::CFGBlock*> reachableBlocks;
  reachableBlocks.insert(&CFG->getEntry());
  bool changed = true;
  while (changed) {
    auto newReachableBlocks(reachableBlocks);
    for (auto RB : reachableBlocks) {
      for (auto B : *CFG) {
        if (isPred(RB, B)) {
          newReachableBlocks.insert(B);
        }
      }
    }
    reachableBlocks.swap(newReachableBlocks);
    changed = reachableBlocks != newReachableBlocks;
  }
  for (auto B : *CFG) {
    if (reachableBlocks.find(B) == std::end(reachableBlocks)) {
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
    clang::SourceLocation beginP
      = getLoc(P->getSourceRange().getBegin());
    clang::SourceLocation endP
      = getLoc(P->getSourceRange().getEnd());
    clang::SourceLocation beginS
      = getLoc(S->getSourceRange().getBegin());
    clang::SourceLocation endS
      = getLoc(S->getSourceRange().getEnd());
    return beginS <= beginP && endP <= endS;
  };
  auto& TIs = mTIs[mCurrentFD];
  for (auto B : *CFG) {
    for (auto I1 = B->begin(); I1 != B->end(); ++I1) {
      if (llvm::Optional<clang::CFGStmt> CS = I1->getAs<clang::CFGStmt>()) {
        const clang::Stmt* S = CS->getStmt();
        if (const clang::CallExpr* CE = clang::dyn_cast<clang::CallExpr>(S)) {
          const clang::FunctionDecl* definition = nullptr;
          CE->getDirectCallee()->hasBody(definition);
          if (definition == nullptr) {
            continue;
          }
          mTs[definition].setFuncDecl(definition);
          const clang::Stmt* P = S;
          for (auto I2 = I1 + 1; I2 != B->end(); ++I2) {
            if (llvm::Optional<clang::CFGStmt> CS = I2->getAs<clang::CFGStmt>()) {
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
          // don't replace function calls in conditional operator (ternary if, ?:)
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
    if (clang::ParmVarDecl* PVD = clang::dyn_cast<clang::ParmVarDecl>(DRE->getDecl())) {
      mTs[mCurrentFD].addParmRef(PVD, DRE);
    }
  }
  return true;
}

std::vector<std::string> FInliner::construct(
  const std::string& type, const std::string& identifier,
  const std::string& context,
  std::map<std::string, std::string>& replacements) {
  // custom tokenizer is needed because ASTUnit doesn't have
  // properly setuped Lexer/Rewriter
  const std::string pattern(
    "[(struct|union|enum)\\s+]?" + mIdentifierPattern + "|\\S");
  clang::ast_matchers::MatchFinder MatchFinder;
  MatchFinder.addMatcher(clang::ast_matchers::varDecl().bind("varDecl"),
    &varDeclHandler);
  std::vector<std::string> tokens = tokenize(type, pattern);
  for (auto& token : tokens) {
    if (replacements.find(token) != std::end(replacements)) {
      token = replacements[token];
    }
  }
  varDeclHandler.setParameters(join(tokens, " "), identifier,
    [&](const std::string& s) -> std::string {
    return join(tokenize(s, pattern), " ");
  });
  tokens.push_back(identifier);
  // multiple positions can be found in cases like 'unsigned' and 'unsigned int'
  // which mean same type; since it's part of declaration-specifiers in grammar, it is
  // guaranteed to be before declared identifier, just choose far position (meaning
  // choosing longest type string)
  // optimization: match in reverse order until success
  std::vector<int> counts(tokens.size(), 0);
  swap(llvm::errs(), llvm::nulls());
  for (int i = tokens.size() - 1; i >= 0; --i) {
    varDeclHandler.initCount();
    std::unique_ptr<clang::ASTUnit> ASTUnit
      = clang::tooling::buildASTFromCode(context + join(tokens, " ") + ";");
    assert(ASTUnit.get() != nullptr && "AST construction failed");
    if (ASTUnit->getDiagnostics().hasErrorOccurred()) {
      std::swap(tokens[i], tokens[std::max(i - 1, 0)]);
      continue;
    }
    MatchFinder.matchAST(ASTUnit->getASTContext());
    counts[i] = varDeclHandler.getCount();
    if (counts[i])
      break;
    std::swap(tokens[i], tokens[std::max(i - 1, 0)]);
  }
  swap(llvm::errs(), llvm::nulls());
  assert(std::find_if(std::begin(counts), std::end(counts),
    [](int arg) -> bool {
    return arg != 0;
  }) != std::end(counts) && "At least one valid position must be found");
  return tokens;
}

std::pair<std::string, std::string> FInliner::compile(
  const TemplateInstantiation& TI, const std::vector<std::string>& args,
  std::set<std::string>& decls) {
  assert(TI.mTemplate->getFuncDecl()->getNumParams() == args.size()
    && "Undefined behavior: incorrect number of arguments specified");
  // smart buffer
  std::string canvas(getSourceText(getRange(TI.mTemplate->getFuncDecl()->getBody())));
  std::vector<unsigned int> mapping(canvas.size());
  std::iota(std::begin(mapping), std::end(mapping), 0);
  unsigned int base = getRange(TI.mTemplate->getFuncDecl()->getBody()).getBegin().getRawEncoding();
  auto get = [&](const clang::SourceRange& SR) -> std::string {
    unsigned int begin = mapping[SR.getBegin().getRawEncoding() - base];
    unsigned int end = mapping[SR.getBegin().getRawEncoding() + getSourceText(SR).size() - base];
    return canvas.substr(begin, end - begin);
  };
  auto update = [&](const clang::SourceRange& SR, std::string value) -> void {
    unsigned int mbegin = SR.getBegin().getRawEncoding() - base;
    unsigned int mend = SR.getBegin().getRawEncoding() + getSourceText(SR).size() - base;
    unsigned int begin = mapping[mbegin];
    unsigned int end = mapping[mend];
    if (end - begin == value.size()) {
      canvas.replace(begin, end - begin, value);
    } else if (end - begin < value.size()) {
      for (auto i = mend; i < mapping.size(); ++i) {
        mapping[i] += value.size() - (end - begin);
      }
      canvas.replace(begin, end - begin, value);
    } else {
      for (auto i = mend; i < mapping.size(); ++i) {
        mapping[i] -= (end - begin) - value.size();
      }
      canvas.replace(begin, end - begin, value);
    }
    return;
  };

  std::string params;
  std::string context;
  // effective context construction
  auto init_context = [&]() {
    context = "";
    for (auto decl : mForwardDecls[TI.mTemplate->getFuncDecl()]) {
      context += getSourceText(getRange(decl)) + ";";
    }
  };
  init_context();
  std::map<std::string, std::string> replacements;
  decls.insert(std::begin(args), std::end(args));
  for (auto decl : TI.mTemplate->getFuncDecl()->decls()) {
    if (clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(decl)) {
      decls.insert(ND->getName());
    }
  }
  for (auto& PVD : TI.mTemplate->getFuncDecl()->parameters()) {
    std::string identifier = addSuffix(PVD->getName(), decls);
    replacements[PVD->getName()] = identifier;
    std::vector<std::string> tokens
      = construct(PVD->getType().getAsString(), identifier, context, replacements);
    context += join(tokens, " ") + ";";
    params.append(join(tokens, " ") + " = " + args[PVD->getFunctionScopeIndex()]
      + ";");
    std::set<clang::SourceRange, std::less<clang::SourceRange>> parameterReferences;
    for (auto DRE : TI.mTemplate->getParmRefs(PVD)) {
      parameterReferences.insert(std::end(parameterReferences), getRange(DRE));
    }
    for (auto& SR : parameterReferences) {
      update(SR, identifier);
    }
  }

  auto pr = [&](const std::pair<const clang::FunctionDecl*,
    std::vector<TemplateInstantiation>> &lhs) -> bool {
    return TI.mTemplate != nullptr && TI.mTemplate->getFuncDecl() != nullptr
      && lhs.first == TI.mTemplate->getFuncDecl();
  };
  if (std::find_if(std::begin(mTIs), std::end(mTIs), pr) != std::end(mTIs)) {
    for (auto& TI : mTIs[TI.mTemplate->getFuncDecl()]) {
      if (TI.mTemplate == nullptr
        || TI.mTemplate->getFuncDecl() == nullptr) {
        continue;
      }
      if (mUnreachableStmts[TI.mFuncDecl].find(TI.mStmt)
        != std::end(mUnreachableStmts[TI.mFuncDecl])) {
        continue;
      }
      std::vector<std::string> args(TI.mCallExpr->getNumArgs());
      std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
        std::begin(args),
        [&](const clang::Expr* arg) -> std::string {
        return get(getRange(arg));
      });
      std::pair<std::string, std::string> text
        = compile(TI, args, decls);
      if (text.second.size() == 0) {
        text.first = "{" + text.first + ";}";
      } else {
        update(getRange(TI.mCallExpr), text.second);
        text.first += get(getRange(TI.mStmt));
        text.first = requiresBraces(TI.mFuncDecl, TI.mStmt) ? "{" + text.first + ";}" : text.first;
      }
      update(getRange(TI.mStmt), "/* " + get(getRange(TI.mCallExpr))
        + " is inlined below */\n" + text.first);
    }
  }

  std::set<const clang::ReturnStmt*> retStmts(TI.mTemplate->getRetStmts());
  std::set<const clang::ReturnStmt*> unreachableRetStmts;
  std::set<const clang::ReturnStmt*> reachableRetStmts;
  for (auto S : mUnreachableStmts[TI.mTemplate->getFuncDecl()]) {
    if (const clang::ReturnStmt* RS = clang::dyn_cast<clang::ReturnStmt>(S)) {
      unreachableRetStmts.insert(RS);
    }
  }
  std::set_difference(std::begin(retStmts), std::end(retStmts),
    std::begin(unreachableRetStmts), std::end(unreachableRetStmts),
    std::inserter(reachableRetStmts, std::end(reachableRetStmts)));
  bool isSingleReturn = TI.mTemplate->isSingleReturn() && reachableRetStmts.size() < 2;

  std::string identifier;
  std::string ret;
  std::string retLab = addSuffix("L", decls);
  if (TI.mTemplate->getFuncDecl()->getReturnType()->isVoidType() == false) {
    identifier = addSuffix("R", decls);
    init_context();
    std::vector<std::string> tokens
      = construct(TI.mTemplate->getFuncDecl()->getReturnType().getAsString(),
        identifier, context, std::map<std::string, std::string>());
    ret = join(tokens, " ") + ";";
    for (auto& RS : reachableRetStmts) {
      std::string text = identifier + " = " + get(getRange(RS->getRetValue())) + ";";
      if (!isSingleReturn) {
        text += "goto " + retLab + ";";
        text = "{" + text + "}";
      }
      update(getRange(RS), text);
    }
  } else {
    std::string s(isSingleReturn ? "" : ("goto " + retLab));
    for (auto& RS : reachableRetStmts) {
      update(getRange(RS), s);
    }
  }
  for (auto RS : unreachableRetStmts) {
    update(getRange(RS), "\n#if 0\n" + get(getRange(RS)) +"\n#endif\n");
  }
  if (!isSingleReturn) {
    canvas += retLab + ":;";
  }
  canvas.insert(std::begin(canvas) + 1, std::begin(params), std::end(params));
  canvas.insert(std::begin(canvas), std::begin(ret), std::end(ret));
  return {canvas, identifier};
}

std::set<std::string> FInliner::getIdentifiers(const clang::Decl* D) const {
  std::set<std::string> identifiers;
  if (const clang::TagDecl* TD = clang::dyn_cast<clang::TagDecl>(D)) {
    std::set<std::string> tmp = getIdentifiers(TD);
    identifiers.insert(std::begin(tmp), std::end(tmp));
  } else if (const clang::FunctionDecl* FD = clang::dyn_cast<clang::FunctionDecl>(D)) {
    identifiers.insert(FD->getName());
  } else {
    for (auto& token : getRawTokens(getRange(D))) {
      if (std::find(std::begin(mKeywords), std::end(mKeywords), token.getRawIdentifier()) != std::end(mKeywords)) {
        continue;
      }
      if (const clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(D)) {
        if (ND->getName() == token.getRawIdentifier()) {
          identifiers.insert(ND->getName());
          continue;
        }
      }
      const clang::DeclContext* DC = D->getDeclContext();
      while (DC) {
        if (std::find_if(DC->decls_begin(), DC->decls_end(),
          [&](const clang::Decl* D) -> bool {
          if (const clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(D)) {
            return ND->getName().str() == token.getRawIdentifier().str();
          } else {
            return false;
          }
        }) != DC->decls_end()) {
          break;
        } else {
          DC = DC->getParent();
        }
      }
      if (DC != nullptr) {
        identifiers.insert(token.getRawIdentifier());
      }
    }
  }
  return identifiers;
}

std::set<std::string> FInliner::getIdentifiers(const clang::TagDecl* TD) const {
  std::set<std::string> identifiers;
  identifiers.insert(TD->getName());
  for (auto decl : TD->decls()) {
    if (const clang::TagDecl* TD = clang::dyn_cast<clang::TagDecl>(decl)) {
      std::set<std::string> tmp = getIdentifiers(TD);
      identifiers.insert(std::begin(tmp), std::end(tmp));
    } else {
      std::set<std::string> tmp = getIdentifiers(decl);
      identifiers.insert(std::begin(tmp), std::end(tmp));
    }
  }
  return identifiers;
}

void FInliner::HandleTranslationUnit(clang::ASTContext& Context) {
  TraverseDecl(Context.getTranslationUnitDecl());
  // associate instantiations with templates
  std::set<const clang::FunctionDecl*> callable;
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      const clang::FunctionDecl* definition = nullptr;
      TI.mCallExpr->getDirectCallee()->hasBody(definition);
      TI.mTemplate = &mTs.at(definition);
      callable.insert(definition);
    }
  }
  // global declarations (outermost, max enclosed)
  // possible scopes C99: function, function prototype, file, block
  // decl contexts C99: TranslationUnitDecl, FunctionDecl, TagDecl, BlockDecl
  // only TagDecl should be traversed because it doesn't produce own scope
  auto isSubDecl = [this](const clang::Decl* P, const clang::Decl* S) -> bool {
    clang::SourceLocation beginP
      = getLoc(P->getSourceRange().getBegin());
    clang::SourceLocation endP
      = getLoc(P->getSourceRange().getEnd());
    clang::SourceLocation beginS
      = getLoc(S->getSourceRange().getBegin());
    clang::SourceLocation endS
      = getLoc(S->getSourceRange().getEnd());
    return beginS <= beginP && endP <= endS;
  };
  std::set<const clang::Decl*> GlobalDecls;
  for (auto decl : Context.getTranslationUnitDecl()->decls()) {
    GlobalDecls.insert(decl);
  }
  for (auto decl : Context.getTranslationUnitDecl()->decls()) {
    for (auto it = std::begin(GlobalDecls); it != std::end(GlobalDecls);) {
      if (*it != decl && isSubDecl(*it, decl)) {
        it = GlobalDecls.erase(it);
      } else {
        ++it;
      }
    }
  }
  // mOutermostDecls - have most outer DeclContext
  for (auto decl : Context.getTranslationUnitDecl()->decls()) {
    std::set<std::string> tmp = getIdentifiers(decl);
    for (auto identifier : tmp) {
      mOutermostDecls[identifier].insert(decl);
    }
    mGlobalIdentifiers.insert(std::begin(tmp), std::end(tmp));
  }
  // identifiers in scope
  for (auto& TIs : mTIs) {
    for (auto& decl : TIs.first->decls()) {
      std::set<std::string> tmp = getIdentifiers(decl);
      mExtIdentifiers[TIs.first].insert(std::begin(tmp), std::end(tmp));
      mIntIdentifiers[TIs.first].insert(std::begin(tmp), std::end(tmp));
    }
  }
  for (auto& T : mTs) {
    for (auto& decl : T.first->decls()) {
      std::set<std::string> tmp = getIdentifiers(decl);
      mExtIdentifiers[T.first].insert(std::begin(tmp), std::end(tmp));
      mIntIdentifiers[T.first].insert(std::begin(tmp), std::end(tmp));
    }
  }
  // compute recursive functions set
  std::set<const clang::FunctionDecl*> recursive;
  for (auto& pair : mTIs) {
    bool ok = true;
    std::set<const clang::FunctionDecl*> callers = { pair.first };
    std::set<const clang::FunctionDecl*> callees;
    for (auto& TIs : pair.second) {
      if (TIs.mTemplate != nullptr && TIs.mTemplate->getFuncDecl() != nullptr) {
        callees.insert(TIs.mTemplate->getFuncDecl());
      }
    }
    while (ok == true && callees.size() != 0) {
      std::set<const clang::FunctionDecl*> intersection;
      std::set_intersection(std::begin(callers), std::end(callers),
        std::begin(callees), std::end(callees),
        std::inserter(intersection, std::end(intersection)));
      if (intersection.size() != 0) {
        ok = false;
        break;
      } else {
        std::set<const clang::FunctionDecl*> tmp;
        for (auto& caller : callees) {
          for (auto& pair : mTIs[caller]) {
            if (pair.mTemplate->getFuncDecl() != nullptr) {
              tmp.insert(pair.mTemplate->getFuncDecl());
            }
          }
        }
        callees.swap(tmp);
      }
    }
    if (ok == false) {
      recursive.insert(pair.first);
    }
  }

  // validate source ranges in user files
  // no one-token declarations exist in C, except labels
  // TODO: should traverse TagDecls?
  std::set<const clang::Decl*> BogusDecls;
  for (auto D : Context.getTranslationUnitDecl()->decls()) {
    if (D->getLocStart().isValid() && mSourceManager.getFileCharacteristic(D->getLocStart())
      != clang::SrcMgr::C_User) {
      // silently skip because we are not going to instantiate functions from standard libraries
      continue;
    }
    auto R = getRange(D);
    if (R.isValid() && R.getBegin().getRawEncoding() == R.getEnd().getRawEncoding()) {
      llvm::errs() << "Bogus source range found at " << R.getBegin().printToString(mSourceManager) << '\n';
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
    std::set<std::string>& identifiers = mExtIdentifiers[T.first];
    for (auto& decl : T.first->decls()) {
      std::set<std::string> tmp = getIdentifiers(decl);
      identifiers.insert(std::begin(tmp), std::end(tmp));
    }
    // intersect local external references in decls with global symbols
    std::set<std::string> extIdentifiers;
    std::set_intersection(std::begin(identifiers), std::end(identifiers),
      std::begin(mGlobalIdentifiers), std::end(mGlobalIdentifiers),
      std::inserter(extIdentifiers, std::end(extIdentifiers)));
    identifiers.swap(extIdentifiers);
    for (auto expr : mExprs[T.first]) {
      for (auto& token : getRawTokens(getRange(expr))) {
        identifiers.insert(token.getRawIdentifier());
      }
    }
    extIdentifiers.clear();
    // intersect local external references in exprs with global symbols
    std::set_intersection(std::begin(identifiers), std::end(identifiers),
      std::begin(mGlobalIdentifiers), std::end(mGlobalIdentifiers),
      std::inserter(extIdentifiers, std::end(extIdentifiers)));
    identifiers.swap(extIdentifiers);
  }
  // mForwardDecls - all referenced external declarations including transitive dependencies
  for (auto& T : mTs) {
    std::set<std::string>& identifiers = mExtIdentifiers[T.first];
    for (auto identifier : identifiers) {
      for (auto decl : mOutermostDecls[identifier]) {
        mForwardDecls[T.first].insert(decl);
        if (const clang::FunctionDecl* FD = clang::dyn_cast<clang::FunctionDecl>(decl)) {
          std::set<const clang::FunctionDecl*> worklist;
          worklist.insert(FD);
          while (!worklist.empty()) {
            auto it = std::begin(worklist);
            FD = *it;
            mForwardDecls[T.first].insert(FD);
            for (auto identifier : identifiers) {
              for (auto decl : mOutermostDecls[identifier]) {
                if (*it != FD && (FD = clang::dyn_cast<clang::FunctionDecl>(decl))) {
                  worklist.insert(FD);
                } else {
                  mForwardDecls[T.first].insert(decl);
                }
              }
            }
            worklist.erase(FD);
          }
        }
      }
    }
  }

  // all constraint checkers
  auto UnusedTemplateChecker = [&](const Template& T) -> std::string {
    return callable.find(T.getFuncDecl()) == std::end(callable)
      ? "Unused template for function \"" + T.getFuncDecl()->getName().str() + "\"\n" : "";
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
    return recursive.find(T.getFuncDecl()) != std::end(recursive)
      ? "Recursive function \"" + T.getFuncDecl()->getNameAsString() + "\"\n" : "";
  };
  auto BogusSRChecker = [&](const Template& T) -> std::string {
    std::string Result;
    for (auto D : T.getFuncDecl()->decls()) {
      if (clang::isa<clang::LabelDecl>(D)) {
        continue;
      }
      auto R = getRange(D);
      if (R.isValid() && R.getBegin().getRawEncoding() == R.getEnd().getRawEncoding()) {
        Result += "Bogus source range found at " + R.getBegin().printToString(mSourceManager) + "\n";
        BogusDecls.insert(D);
        BogusDecls.insert(T.getFuncDecl());
      }
    }
    return Result;
  };
  auto BogusSRTransitiveChecker = [&](const Template& T) -> std::string {
    std::set<const clang::Decl*> tmp;
    std::set_intersection(std::begin(BogusDecls), std::end(BogusDecls),
      std::begin(mForwardDecls[T.getFuncDecl()]),
      std::end(mForwardDecls[T.getFuncDecl()]),
      std::inserter(tmp, std::end(tmp)));
    return tmp.size() > 0
      ? "Transitive dependency on bogus declaration for function \""
      + T.getFuncDecl()->getNameAsString() + "\"\n" : "";
  };
  auto NonlocalExternalDepsChecker = [&](const Template& T) -> std::string {
    for (auto decl : mForwardDecls[T.getFuncDecl()]) {
      if (mSourceManager.getFileID(decl->getLocStart())
        != mSourceManager.getFileID(T.getFuncDecl()->getLocStart())) {
        return "Reference to nonlocal global declaration in function \"" + T.getFuncDecl()->getNameAsString() + "\"\n";
      }
    }
    return "";
  };
  auto StaticExternalDepsChecker = [&](const Template& T) -> std::string {
    std::string Result;
    for (auto decl : mForwardDecls[T.getFuncDecl()]) {
      const clang::VarDecl* VD = clang::dyn_cast<clang::VarDecl>(decl);
      const clang::FunctionDecl* FD = clang::dyn_cast<clang::FunctionDecl>(decl);
      if (VD && VD->getStorageClass() == clang::StorageClass::SC_Static) {
        Result += "Reference to static global declaration \"" + VD->getNameAsString() + "\" in function \"" + T.getFuncDecl()->getNameAsString() + "\"\n";
      } else if (FD && FD->getStorageClass() == clang::StorageClass::SC_Static) {
        Result += "Reference to static global declaration \"" + FD->getNameAsString() + "\" in function \"" + T.getFuncDecl()->getNameAsString() + "\"\n";
      }
    }
    return Result;
  };
  auto NestedExternalDepsChecker = [&](const Template& T) -> std::string {
    bool NestedDeps = false;
    for (auto D : mForwardDecls[T.getFuncDecl()]) {
      if (const clang::TagDecl* TD = clang::dyn_cast<clang::TagDecl>(D)) {
        for (auto D : TD->decls()) {
          if (clang::isa<clang::TagDecl>(D)) {
            NestedDeps = true;
            break;
          }
        }
      }
      if (GlobalDecls.find(D) == std::end(GlobalDecls)) {
        NestedDeps = true;
        break;
      }
    }
    return NestedDeps ? "Reference to nested declaration in function \"" + T.getFuncDecl()->getNameAsString() + "\"\n" : "";
  };
  auto ExternalDepsChecker = [&](const TemplateInstantiation& TI) -> std::string {
    std::string Result = NonlocalExternalDepsChecker(*TI.mTemplate)
      + StaticExternalDepsChecker(*TI.mTemplate);
    return Result != "" ? "Unresolvable dependencies in function \"" + TI.mTemplate->getFuncDecl()->getNameAsString() + "\":\n" + Result : "";
  };
  auto CollidedIdentifiersChecker = [&](const TemplateInstantiation& TI) -> std::string {
    std::string Result;
    std::set<std::string> tmp;
    std::set_intersection(std::begin(mIntIdentifiers[TI.mFuncDecl]), std::end(mIntIdentifiers[TI.mFuncDecl]),
      std::begin(mExtIdentifiers[TI.mTemplate->getFuncDecl()]), std::end(mExtIdentifiers[TI.mTemplate->getFuncDecl()]),
      std::inserter(tmp, std::end(tmp)));
    if (tmp.size() > 0) {
      Result += "Potential identifier collision between template \""
        + TI.mTemplate->getFuncDecl()->getNameAsString() + "\" and instantiation \""
        + getSourceText(getRange(TI.mCallExpr)) + "\" contexts\n";
    }
    return Result;
  };
  // if function has external dependencies:
  //   if external dependencies collide with function where specific instantiation is:
  //     disable this instantiation (perspective - fix collisions through renaming)
  //   if atleast one external decl in different file OR atleast one external decl is static:
  //     disable all instantiations in files except one where function decl is
  //   else:
  //     generate forward decls for all external dependencies and insert them before all function decls
  //       with instantiations
  // else:
  //   allow instantiations
  // note: external dependencies include callees
  std::function<std::string(const Template&)> TChainChecker[] = {
    UnusedTemplateChecker,
    UserDefTChecker,
    VariadicChecker,
    RecursiveChecker,
    BogusSRChecker,
    BogusSRTransitiveChecker,
    NestedExternalDepsChecker
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
      llvm::errs() << "Template \"" + T.first->getNameAsString() + "\" disabled due to constraint violations:\n" + Result;
    }
  }
  std::function<std::string(const TemplateInstantiation&)> TIChainChecker[] = {
    UserDefTIChecker,
    ExternalDepsChecker,
    CollidedIdentifiersChecker
  };
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      if (TI.mTemplate == nullptr) {
        continue;
      }
      std::string Result;
      for (auto Checker : TIChainChecker) {
        if (Result != "") break;
        Result += Checker(TI);
      }
      if (Result != "") {
        TI.mTemplate = nullptr;
        llvm::errs() << "Template instantiation \"" + getSourceText(getRange(TI.mCallExpr)) + "\" disabled due to constraint violations:\n" + Result;
      }
    }
  }

  // info
  [&]() {
    llvm::errs() << '\n';
    llvm::errs() << "Total template instantiations:" << '\n';
    for (auto& TIs : mTIs) {
      if (TIs.second.size() == 0) {
        continue;
      }
      llvm::errs() << ' ' << "in " << '"' << TIs.first->getName()
        << '"' << ':' << '\n';
      for (auto& TI : TIs.second) {
        if (TI.mTemplate != nullptr) {
          llvm::errs() << "  " << '"'
            << getSourceText(getRange(TI.mCallExpr)) << '"' << '\n';
        }
      }
      llvm::errs() << '\n';
    }
    llvm::errs() << '\n';
    llvm::errs() << "Total templates:" << '\n';
    for (auto& T : mTs) {
      if (T.second.getFuncDecl() != nullptr) {
        llvm::errs() << ' ' << '"' << T.first->getName() << '"' << '\n';
      }
    }
    llvm::errs() << '\n';
    llvm::errs() << "Disabled templates ("
      << std::count_if(std::begin(mTs), std::end(mTs),
        [&](const std::pair<const clang::FunctionDecl*, Template>& lhs) -> bool {
      return lhs.second.getFuncDecl() == nullptr;
    }) << "):" << '\n';
    for (auto& T : mTs) {
      if (T.second.getFuncDecl() == nullptr) {
        llvm::errs() << ' ' << '"' << T.first->getName() << '"' << '\n';
      }
    }
    llvm::errs() << '\n';
    llvm::errs() << "Disabled template instantiations: ("
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
      llvm::errs() << ' ' << "in " << '"' << TIs.first->getName()
        << '"' << ':' << '\n';
      for (auto& TI : TIs.second) {
        if (TI.mTemplate == nullptr || TI.mTemplate->getFuncDecl() == nullptr) {
          llvm::errs() << "  " << '"'
            << getSourceText(getRange(TI.mCallExpr)) << '"' << '\n';
        }
      }
      llvm::errs() << '\n';
    }
    llvm::errs() << '\n';
  }();
  // recursive instantiation
  for (auto& TIs : mTIs) {
    auto pr = [&](const std::pair<const clang::FunctionDecl*, Template>& lhs)
      -> bool {
      return TIs.first == lhs.first && lhs.second.getFuncDecl() == nullptr;
    };
    if (std::find_if(std::begin(mTs), std::end(mTs), pr) != std::end(mTs)) {
      for (auto& TI : TIs.second) {
        if (TI.mTemplate == nullptr
          || TI.mTemplate->getFuncDecl() == nullptr) {
          continue;
        }
        std::set<std::string>& fDecls = mIntIdentifiers[TI.mFuncDecl];
        std::vector<std::string> args(TI.mCallExpr->getNumArgs());
        std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
          std::begin(args),
          [&](const clang::Expr* arg) -> std::string {
          return mRewriter.getRewrittenText(getRange(arg));
        });
        fDecls.insert(std::begin(args), std::end(args));
        std::pair<std::string, std::string> text
          = compile(TI, args, fDecls);
        if (text.second.size() == 0) {
          text.first = "{" + text.first + ";}";
        } else {
          mRewriter.ReplaceText(getRange(TI.mCallExpr), text.second);
          text.first += mRewriter.getRewrittenText(getRange(TI.mStmt));
          text.first = requiresBraces(TI.mFuncDecl, TI.mStmt) ? "{" + text.first + ";}" : text.first;
        }
        mRewriter.ReplaceText(getRange(TI.mStmt),
          "/* " + getSourceText(getRange(TI.mCallExpr))
          + " is inlined below */\n" + text.first);
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
  return _Cont.size() > 0 ? std::accumulate(std::next(std::cbegin(_Cont)), std::cend(_Cont),
    std::string(*std::cbegin(_Cont)),
    [&](const std::string& left, const std::string& right) {
    return left + delimiter + right;
  }) : "";
}

template<typename T>
void FInliner::swap(T& lhs, T& rhs) const {
  if (&lhs == &rhs) {
    return;
  }
  char* tmp = nullptr;
  const int size = sizeof(T) / sizeof(*tmp);
  tmp = new char[size];
  std::memcpy(tmp, &lhs, size);
  std::memcpy(&lhs, &rhs, size);
  std::memcpy(&rhs, tmp, size);
  delete[] tmp;
  return;
}

std::string FInliner::addSuffix(
  const std::string& Prefix,
  std::set<std::string>& LocalIdentifiers) const {
  int Count = 0;
  std::set<std::string> Identifiers(LocalIdentifiers);
  Identifiers.insert(std::begin(mGlobalIdentifiers), std::end(mGlobalIdentifiers));
  std::string Identifier(Prefix + std::to_string(Count++));
  bool ok = false;
  while (ok == false) {
    ok = true;
    if (std::find(std::begin(Identifiers), std::end(Identifiers), Identifier)
      != std::end(Identifiers)) {
      ok = false;
      Identifier = Prefix + std::to_string(Count++);
    }
  }
  LocalIdentifiers.insert(Identifier);
  return Identifier;
}

std::vector<std::string> FInliner::tokenize(
  std::string s, std::string p) const {
  std::vector<std::string> tokens;
  std::regex rgx(p);
  std::smatch sm;
  for (; std::regex_search(s, sm, rgx) == true; s = sm.suffix()) {
    tokens.push_back(sm.str());
  }
  return tokens;
}

bool FInliner::requiresBraces(const clang::FunctionDecl* FD, const clang::Stmt* S) {
  if (const clang::DeclStmt* DS = clang::dyn_cast<clang::DeclStmt>(S)) {
    std::set<const clang::Decl*> decls(DS->decl_begin(), DS->decl_end());
    std::set<const clang::Expr*> refs;
    std::copy_if(std::begin(mExprs[FD]), std::end(mExprs[FD]),
      std::inserter(refs, std::begin(refs)),
      [&](const clang::Expr* arg) -> bool {
      if (const clang::DeclRefExpr* DRE = llvm::dyn_cast<clang::DeclRefExpr>(arg)) {
        return std::find(std::begin(decls), std::end(decls), DRE->getFoundDecl())
          != std::end(decls);
      } else {
        return false;
      }
    });
    for (auto obj : refs) {
      if (getRange(DS).getBegin().getRawEncoding()
        <= getRange(obj).getBegin().getRawEncoding()
        && getRange(obj).getEnd().getRawEncoding()
        <= getRange(DS).getEnd().getRawEncoding()) {
        refs.erase(obj);
      }
    }
    return refs.size() == 0;
  }
  return true;
}

// for debug
void FunctionInlinerQueryManager::run(llvm::Module* M, TransformationContext* Ctx) {
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

bool FunctionInlinerQueryManager::beginSourceFile(
    clang::CompilerInstance& CI, llvm::StringRef File) {
  auto& PP = CI.getPreprocessor();
  mIPH = new InlinePragmaHandler();
  PP.AddPragmaHandler(mIPH);
  return true;
}

void InlinePragmaHandler::HandlePragma(clang::Preprocessor& PP,
  clang::PragmaIntroducerKind Introducer, clang::Token& FirstToken) {
  PP.CheckEndOfDirective("pragma inline");
  return;
}
