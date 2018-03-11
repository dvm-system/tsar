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

std::vector<clang::Token> FInliner::getRawTokens(const clang::SourceRange& SR,
  clang::tok::TokenKind TK) const {
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
    if (Token.getKind() != TK) {
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
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + mCurrentFD->getName()).str().data());
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
          auto B(B);
          const clang::Stmt* P = S;
          bool inCondOp = false;
          while (true) {
            S = P;
            for (auto B1 = CFG->begin(); B1 != CFG->end(); ++B1) {
              for (auto I1 = (*B1)->begin(); I1 != (*B1)->end(); ++I1) {
                if (llvm::Optional<clang::CFGStmt> CS
                  = I1->getAs<clang::CFGStmt>()) {
                  if (isSubStmt(P, CS->getStmt())) {
                    P = CS->getStmt();
                    B = *B1;
                    // check if we are in ternary if
                    if (clang::isa<clang::ConditionalOperator>(CS->getStmt())) {
                      inCondOp = true;
                    }
                  }
                }
              }
            }
            // all sections of for-loop are placed in different blocks
            // initial section is referenced by stmt in successor blocks
            for (auto B1 = CFG->begin(); B1 != CFG->end(); ++B1) {
              if (const clang::Stmt* S = (*B1)->getTerminator()) {
                if (isSubStmt(P, S)) {
                  P = S;
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
  int i = 0;
  for (i = tokens.size() - 1; i >= 0; --i) {
    varDeclHandler.initCount();
    std::unique_ptr<clang::ASTUnit> ASTUnit
      = clang::tooling::buildASTFromCode(context + join(tokens, " ") + ";");
    assert(ASTUnit.get() != nullptr && "AST construction failed");
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
  clang::Rewriter lRewriter(mSourceManager,
    mRewriter.getLangOpts());
  clang::SourceManager& SM = lRewriter.getSourceMgr();
  std::string params;
  std::string context;
  // effective context construction
  auto init_context = [&]() {
    context = "";
    auto decls = mContext.getTranslationUnitDecl()->decls();
    for (auto i : mIdentifiers[TI.mTemplate->getFuncDecl()]) {
      auto it = std::find_if(std::begin(decls), std::end(decls),
        [&](const clang::Decl* D) -> bool {
        if (const clang::NamedDecl* ND = llvm::dyn_cast<clang::NamedDecl>(D)) {
          return ND->getName().str() == i;
        } else {
          return false;
        }
      });
      if (it != std::end(decls)) {
        context += getSourceText(getRange(*it)) + ";";
      }
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
      = tokenize(getSourceText(getRange(PVD)), mIdentifierPattern);
    tokens
      = construct(PVD->getType().getAsString(), identifier, context, replacements);
    context += join(tokens, " ") + ";";
    params.append(join(tokens, " ") + " = " + args[PVD->getFunctionScopeIndex()]
      + ";");
    std::set<clang::SourceRange, std::less<clang::SourceRange>> parameterReferences;
    for (auto DRE : TI.mTemplate->getParmRefs(PVD)) {
      parameterReferences.insert(std::end(parameterReferences), getRange(DRE));
    }
    for (auto& SR : parameterReferences) {
      lRewriter.ReplaceText(SR, identifier);
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
      std::vector<std::string> args(TI.mCallExpr->getNumArgs());
      std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
        std::begin(args),
        [&](const clang::Expr* arg) -> std::string {
        return lRewriter.getRewrittenText(getRange(arg));
      });
      std::pair<std::string, std::string> text
        = compile(TI, args, decls);
      if (text.second.size() == 0) {
        text.first.insert(std::begin(text.first), '{');
        lRewriter.ReplaceText(getRange(TI.mStmt), text.first);
        lRewriter.InsertTextAfterToken(getRange(TI.mStmt).getEnd(),
          ";}");
      } else {
        if (requiresBraces(TI.mFuncDecl, TI.mStmt) == true) {
          text.first.insert(std::begin(text.first), '{');
          lRewriter.InsertTextAfterToken(getRange(TI.mStmt).getEnd(),
            ";}");
        }
        lRewriter.ReplaceText(getRange(TI.mCallExpr), text.second);
        lRewriter.InsertTextBefore(getRange(TI.mStmt).getBegin(),
          text.first);
      }
      lRewriter.InsertTextBefore(getRange(TI.mStmt).getBegin(),
        "/* " + getSourceText(getRange(TI.mCallExpr))
        + " is inlined below */\n");
    }
  }

  std::string identifier;
  std::string ret;
  std::string retLab = addSuffix("L", decls);
  std::vector<clang::ReturnStmt*> returnStmts = TI.mTemplate->getRetStmts();
  if (TI.mTemplate->getFuncDecl()->getReturnType()->isVoidType() == false) {
    identifier = addSuffix("R", decls);
    init_context();
    std::vector<std::string> tokens
      = tokenize(TI.mTemplate->getFuncDecl()->getReturnType().getAsString(),
        mIdentifierPattern);
    tokens
      = construct(TI.mTemplate->getFuncDecl()->getReturnType().getAsString(),
        identifier, context, std::map<std::string, std::string>());
    ret = join(tokens, " ") + ";";
    for (auto& RS : returnStmts) {
      std::string text = "{" + identifier + " = "
        + lRewriter.getRewrittenText(getRange(RS->getRetValue()))
        + ";goto " + retLab + ";}";
      lRewriter.ReplaceText(getRange(RS), text);
    }
    lRewriter.ReplaceText(getRange(TI.mCallExpr), identifier);
  } else {
    for (auto& RS : returnStmts) {
      lRewriter.ReplaceText(getRange(RS), "goto " + retLab);
    }
  }
  std::string text = lRewriter.getRewrittenText(
    getRange(TI.mTemplate->getFuncDecl()->getBody()))
    + retLab + ":;";
  text.insert(std::begin(text) + 1, std::begin(params), std::end(params));
  text.insert(std::begin(text), std::begin(ret), std::end(ret));
  return {text, identifier};
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
  // global identifiers
  for (auto& decl : Context.getTranslationUnitDecl()->decls()) {
    for (auto& token : getRawTokens(getRange(decl))) {
      //if (!token.getIdentifierInfo()->isKeyword(mRewriter.getLangOpts())) {
        mGlobalIdentifiers.insert(token.getRawIdentifier());
      //}
    }
  }
  // identifiers in scope
  std::map<const clang::FunctionDecl*, std::set<std::string>> decls;
  for (auto& TIs : mTIs) {
    for (auto& decl : TIs.first->decls()) {
      for (auto& token : getRawTokens(getRange(decl))) {
        //if (!token.getIdentifierInfo()->isKeyword(mRewriter.getLangOpts())) {
          decls[TIs.first].insert(token.getRawIdentifier());
        //}
      }
    }
  }
  // remove unused templates
  for (auto it = std::begin(mTs); it != std::end(mTs);) {
    if (callable.find(it->first) == std::end(callable)) {
      it = mTs.erase(it);
    } else {
      ++it;
    }
  }
  // disable instantiation of/in non-user-defined functions
  for (auto it = std::begin(mTIs); it != std::end(mTIs); ++it) {
    if (mSourceManager.getFileCharacteristic(it->first->getLocStart())
      != clang::SrcMgr::C_User) {
      for (auto& TI : it->second) {
        TI.mTemplate = nullptr;
      }
    }
  }
  for (auto it = std::begin(mTs); it != std::end(mTs); ++it) {
    if (mSourceManager.getFileCharacteristic(it->first->getLocStart())
      != clang::SrcMgr::C_User) {
      it->second.setFuncDecl(nullptr);
    }
  }
  // disable instantiation of variadic functions
  for (auto& pair : mTs) {
    if (pair.first->isVariadic() == true) {
      pair.second.setFuncDecl(nullptr);
    }
  }
  // disable instantiation of recursive functions
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
  for (auto& FD : recursive) {
    mTs[FD].setFuncDecl(nullptr);
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
  std::set<std::string> globalIdentifiers;
  for (auto D : Context.getTranslationUnitDecl()->decls()) {
    for (auto& token : getRawTokens(getRange(D))) {
      //if (!token.getIdentifierInfo()->isKeyword(mRewriter.getLangOpts())) {
        globalIdentifiers.insert(token.getRawIdentifier());
      //}
    }
  }
  for (auto T : mTs) {
    if (T.second.getFuncDecl() == nullptr) {
      continue;
    }
    std::set<std::string>& identifiers = mIdentifiers[T.first];
    for (auto decl : T.first->decls()) {
      for (auto& token : getRawTokens(getRange(decl))) {
        identifiers.insert(token.getRawIdentifier());
      }
    }
    for (auto expr : mExprs[T.first]) {
      for (auto& token : getRawTokens(getRange(expr))) {
        identifiers.insert(token.getRawIdentifier());
      }
    }
    // intersect local references with global symbols
    std::set<std::string> extIdentifiers;
    std::set_intersection(std::begin(identifiers), std::end(identifiers),
      std::begin(globalIdentifiers), std::end(globalIdentifiers),
      std::inserter(extIdentifiers, std::end(extIdentifiers)));
    identifiers.swap(extIdentifiers);
    for (auto& i : identifiers) {
      std::set<const clang::Decl*> found;
      assert(mGlobalIdentifiers.find(i) != std::end(mGlobalIdentifiers)
        && "No corresponding global identifier found");
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
      return lhs.first == TIs.first;
    };
    if (std::find_if(std::begin(mTs), std::end(mTs), pr) == std::end(mTs)) {
      for (auto& TI : TIs.second) {
        if (TI.mTemplate == nullptr
          || TI.mTemplate->getFuncDecl() == nullptr) {
          continue;
        }
        std::set<std::string>& fDecls = decls[TI.mFuncDecl];
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
          text.first.insert(std::begin(text.first), '{');
          mRewriter.ReplaceText(getRange(TI.mStmt), text.first);
          mRewriter.InsertTextAfterToken(getRange(TI.mStmt).getEnd(),
            ";}");
        } else {
          if (requiresBraces(TI.mFuncDecl, TI.mStmt) == true) {
            text.first.insert(std::begin(text.first), '{');
            mRewriter.InsertTextAfterToken(getRange(TI.mStmt).getEnd(),
              ";}");
          }
          mRewriter.ReplaceText(getRange(TI.mCallExpr), text.second);
          mRewriter.InsertTextBefore(getRange(TI.mStmt).getBegin(),
            text.first);
        }
        mRewriter.InsertTextBefore(getRange(TI.mStmt).getBegin(),
          "/* " + getSourceText(getRange(TI.mCallExpr))
          + " is inlined below */\n");
      }
    }
  }
  return;
}

std::string FInliner::getSourceText(const clang::SourceRange& SR) const {
  return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(SR),
    mContext.getSourceManager(), mContext.getLangOpts());
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
  return std::accumulate(std::next(std::cbegin(_Cont)), std::cend(_Cont),
    std::string(*std::cbegin(_Cont)),
    [&](const std::string& left, const std::string& right) {
    return left + delimiter + right;
  });
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
