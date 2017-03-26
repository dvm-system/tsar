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

#ifdef FINLINER

#include "tsar_finliner.h"

#include <algorithm>
#include <numeric>
#include <regex>
#include <map>
#include <set>
#include <vector>
#include <type_traits>

#include <clang/Analysis/CFG.h>
#include <clang/Format/Format.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/raw_ostream.h>

// TODO(jury.zykov@yandex.ru): decrease size of context used in construct() (currently works slow)
// TODO(jury.zykov@yandex.ru): add constraints checks for inlinable functions
// TODO(jury.zykov@yandex.ru): direct argument passing without local variables when possible
// TODO(jury.zykov@yandex.ru): add helpful comments near instantiations in user files

using namespace tsar;
using namespace detail;

bool FInliner::VisitFunctionDecl(clang::FunctionDecl* FD) {
  if (FD->isThisDeclarationADefinition() == false) {
    return true;
  }
  mCurrentFD = FD;
  return true;
}

bool FInliner::VisitForStmt(clang::ForStmt* FS) {
  mFSs.push_back(FS);
  // build CFG for function which _possibly_ contains calls of functions
  // which can be inlined
  std::unique_ptr<clang::CFG> CFG = clang::CFG().buildCFG(
    nullptr, FS, &mContext, clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && "CFG construction failed for "
    + mCurrentFD->getName());
  for (auto B : *CFG) {
    for (auto I1 = B->begin(); I1 != B->end(); ++I1) {
      if (llvm::Optional<clang::CFGStmt> CS = I1->getAs<clang::CFGStmt>()) {
        clang::Stmt* S = const_cast<clang::Stmt*>(CS->getStmt());
        if (llvm::isa<clang::CallExpr>(S) == true) {
          clang::CallExpr* CE = reinterpret_cast<clang::CallExpr*>(S);
          bool inLoop = false;
          for (auto it = mFSs.rbegin(); it != mFSs.rend(); ++it) {
            if (((*it)->getLocStart() < CE->getLocStart()
              || (*it)->getLocStart() == CE->getLocStart())
              && (CE->getLocEnd() < (*it)->getLocEnd()
                || CE->getLocEnd() == (*it)->getLocEnd())) {
              inLoop = true;
              break;
            }
          }
          if (inLoop == false) {
            continue;
          }
          const clang::FunctionDecl* definition = nullptr;
          CE->getDirectCallee()->hasBody(definition);
          if (definition == nullptr) {
            continue;
          }
          mTs[const_cast<clang::FunctionDecl*>(definition)].setFuncDecl(
            const_cast<clang::FunctionDecl*>(definition));
          clang::Stmt* P = S;
          for (auto I2 = I1 + 1; I2 != B->end(); ++I2) {
            if (llvm::Optional<clang::CFGStmt> CS
              = I2->getAs<clang::CFGStmt>()) {
              clang::Stmt* S = const_cast<clang::Stmt*>(CS->getStmt());
              // in basic block each instruction can both depend or not
              // on results of previous instructions
              // we are looking for the last statement which on some dependency
              // depth references found callExpr
              if ((S->getLocStart() < P->getLocStart()
                || S->getLocStart() == P->getLocStart())
                && (P->getLocEnd() < S->getLocEnd()
                  || P->getLocEnd() == S->getLocEnd())) {
                P = S;
              }
            }
          }
          for (auto& stmtPair :
            llvm::iterator_range<clang::CFG::synthetic_stmt_iterator>
            (CFG->synthetic_stmt_begin(), CFG->synthetic_stmt_end())) {
            if (stmtPair.first == P) {
              P = const_cast<clang::DeclStmt*>(stmtPair.second);
              break;
            }
          }
          // don't replace function calls in condition expressions of loops
          S = B->getTerminator();
          if (S != nullptr) {
            if ((S->getLocStart() < P->getLocStart()
              || S->getLocStart() == P->getLocStart())
              && (P->getLocEnd() < S->getLocEnd() || P->getLocEnd()
                == S->getLocEnd())) {
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
          TemplateInstantiation TI = {mCurrentFD, P, CE, nullptr};
          if (std::find(std::begin(mTIs[mCurrentFD]),
            std::end(mTIs[mCurrentFD]), TI) == std::end(mTIs[mCurrentFD])) {
            mTIs[mCurrentFD].push_back(TI);
          }
        }
      }
    }
  }
  return true;
}

bool FInliner::VisitDeclRefExpr(clang::DeclRefExpr* DRE) {
  mRefs.insert(DRE);
  if (mTs.find(mCurrentFD) == mTs.end()) {
    return true;
  }
  // parameter reference
  if (llvm::isa<clang::ParmVarDecl>(DRE->getDecl()) == true) {
    mTs[mCurrentFD].addParmRef(
      reinterpret_cast<clang::ParmVarDecl*>(DRE->getDecl()), DRE);
  }
  return true;
}

bool FInliner::VisitReturnStmt(clang::ReturnStmt* RS) {
  if (mTs.find(mCurrentFD) == mTs.end()) {
    return true;
  }
  mTs[mCurrentFD].addRetStmt(RS);
  return true;
}

std::vector<std::string> FInliner::construct(
    const std::string& type, const std::string& identifier,
    std::string& context) {
  const std::string pattern("(struct|union|enum)\\s+\\w+|\\w+|\\S");
  clang::ast_matchers::MatchFinder MatchFinder;
  MatchFinder.addMatcher(clang::ast_matchers::varDecl().bind("varDecl"),
    &varDeclHandler);
  std::vector<std::string> tokens = tokenize(type, pattern);
  varDeclHandler.setParameters(join(tokens, " "), identifier,
    [&](const std::string& s) -> std::string {
    std::vector<std::string> tokens = tokenize(s, pattern);
    return join(tokens, " ");
  });
  tokens.push_back(identifier);
  std::vector<int> counts(tokens.size(), 0);
  swap(llvm::errs(), llvm::nulls());
  for (int i = tokens.size() - 1; i >= 0; --i) {
    varDeclHandler.initCount();
    std::unique_ptr<clang::ASTUnit> ASTUnit
      = clang::tooling::buildASTFromCode(context + join(tokens, " ") + ";");
    assert(ASTUnit.get() != nullptr && "AST construction failed");
    MatchFinder.matchAST(ASTUnit->getASTContext());
    counts[i] = varDeclHandler.getCount();
    std::swap(tokens[i], tokens[std::max(i - 1, 0)]);
  }
  swap(llvm::errs(), llvm::nulls());
  assert(std::find_if(std::begin(counts), std::end(counts),
    [](int arg) -> bool {
    return arg != 0;
  }) != std::end(counts) && "At least one valid position must be found");
  int max = *std::max_element(std::begin(counts), std::end(counts));
  assert(std::count_if(std::begin(counts), std::end(counts),
    [&](int arg) {
    return arg == max;
  }) == 1 && "Multiple equivalent variants are found");
  int position = std::find_if(std::begin(counts), std::end(counts),
    [&](int arg) -> bool {
    return arg == max;
  }) - std::begin(counts);
  tokens.erase(std::begin(tokens));
  tokens.insert(std::begin(tokens) + position, identifier);
  context += join(tokens, " ") + ";";
  return tokens;
}

std::pair<std::string, std::string> FInliner::compile(
  const TemplateInstantiation& TI, const std::vector<std::string>& args,
  std::set<std::string>& decls) {
  assert(TI.mTemplate->getFuncDecl()->getNumParams() == args.size()
    && "Undefined behavior: incorrect number of arguments specified");
  clang::Rewriter lRewriter(mCompiler.getSourceManager(),
    mCompiler.getLangOpts());
  std::string params;
  std::string context;
  for (auto& decl : mGlobalDecls) {
    context += getSourceText(decl->getSourceRange()) + ";";
  }
  decls.insert(std::begin(args), std::end(args));
  for (auto& PVD : TI.mTemplate->getFuncDecl()->params()) {
    std::string identifier = addSuffix(PVD->getName(), decls);
    std::vector<std::string> tokens
      = construct(PVD->getType().getUnqualifiedType().getAsString(), identifier,
        context);
    params.append(join(tokens, " ") + " = " + args[PVD->getFunctionScopeIndex()]
      + ";");
    std::vector<clang::DeclRefExpr*> parameterReferences
      = TI.mTemplate->getParmRefs(PVD);
    for (auto& DRE : parameterReferences) {
      lRewriter.ReplaceText(DRE->getSourceRange(), identifier);
    }
  }

  auto pr = [&](const std::pair<clang::FunctionDecl*,
    std::vector<TemplateInstantiation>> &lhs) -> bool {
    return lhs.first == TI.mTemplate->getFuncDecl();
  };
  if (std::find_if(std::begin(mTIs), std::end(mTIs), pr) != std::end(mTIs)) {
    for (auto& TI : mTIs[TI.mTemplate->getFuncDecl()]) {
      std::vector<std::string> args(TI.mCallExpr->getNumArgs());
      std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
        std::begin(args),
        [&](const clang::Expr* arg) -> std::string {
        return lRewriter.getRewrittenText(arg->getSourceRange());
      });
      std::pair<std::string, std::string> text
        = compile(TI, args, decls);
      if (text.second.size() == 0) {
        lRewriter.ReplaceText(TI.mStmt->getSourceRange(), text.first);
      } else {
        if (requiresBraces(TI.mStmt) == true) {
          text.first.insert(std::begin(text.first), '{');
          lRewriter.InsertTextAfterToken(TI.mStmt->getSourceRange().getEnd(),
            ";}");
        }
        lRewriter.ReplaceText(TI.mCallExpr->getSourceRange(), text.second);
        lRewriter.InsertTextBefore(TI.mStmt->getSourceRange().getBegin(),
          text.first);
      }
    }
  }

  std::string identifier;
  std::string ret;
  std::string retLab = addSuffix("L", decls);
  std::vector<clang::ReturnStmt*> returnStmts = TI.mTemplate->getRetStmts();
  if (TI.mTemplate->getFuncDecl()->getReturnType()->isVoidType() == false) {
    identifier = addSuffix("R", decls);
    context = "";
    std::vector<std::string> tokens
      = construct(TI.mTemplate->getFuncDecl()->getReturnType().getAsString(),
        identifier, context);
    ret = join(tokens, " ") + ";";
    for (auto& RS : returnStmts) {
      std::string text = "{" + identifier + " = "
        + lRewriter.getRewrittenText(RS->getRetValue()->getSourceRange())
        + ";goto " + retLab + ";}";
      lRewriter.ReplaceText(RS->getSourceRange(), text);
    }
    lRewriter.ReplaceText(TI.mCallExpr->getSourceRange(), identifier);
  } else {
    for (auto& RS : returnStmts) {
      lRewriter.ReplaceText(RS->getSourceRange(), "goto " + retLab);
    }
  }
  std::string text = lRewriter.getRewrittenText(
    TI.mTemplate->getFuncDecl()->getBody()->getSourceRange())
    + retLab + ":;";
  text.insert(std::begin(text) + 1, std::begin(params), std::end(params));
  text.insert(std::begin(text), std::begin(ret), std::end(ret));
  return {text, identifier};
}

void FInliner::HandleTranslationUnit(clang::ASTContext& Context) {
  TraverseDecl(Context.getTranslationUnitDecl());
  for (auto& decl : Context.getTranslationUnitDecl()->decls()) {
    mGlobalDecls.insert(decl);
  }
  // associate instantiations with templates
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      const clang::FunctionDecl* definition = nullptr;
      TI.mCallExpr->getDirectCallee()->hasBody(definition);
      TI.mTemplate = &mTs.at(const_cast<clang::FunctionDecl*>(definition));
    }
  }
  // recursive instantiation
  for (auto& TIs : mTIs) {
    auto pr = [&](const std::pair<clang::FunctionDecl*, Template> &lhs)
      -> bool {
      return lhs.first == TIs.first;
    };
    if (std::find_if(std::begin(mTs), std::end(mTs), pr) == std::end(mTs)) {
      for (auto& TI : TIs.second) {
        std::set<std::string> decls;
        for (auto& decl : mGlobalDecls) {
          if (clang::isa<clang::NamedDecl>(decl) == true) {
            decls.insert(reinterpret_cast<clang::NamedDecl*>(decl)->getName());
          }
        }
        for (auto& decl : TI.mFuncDecl->decls()) {
          if (clang::isa<clang::NamedDecl>(decl) == true) {
            decls.insert(reinterpret_cast<clang::NamedDecl*>(decl)->getName());
          }
        }
        std::vector<std::string> args(TI.mCallExpr->getNumArgs());
        std::transform(TI.mCallExpr->arg_begin(), TI.mCallExpr->arg_end(),
          std::begin(args),
          [&](const clang::Expr* arg) -> std::string {
          return getSourceText(arg->getSourceRange());
        });
        decls.insert(std::begin(args), std::end(args));
        std::pair<std::string, std::string> text
          = compile(TI, args, decls);
        if (text.second.size() == 0) {
          mRewriter.ReplaceText(TI.mStmt->getSourceRange(), text.first);
        } else {
          if (requiresBraces(TI.mStmt) == true) {
            text.first.insert(std::begin(text.first), '{');
            mRewriter.InsertTextAfterToken(TI.mStmt->getSourceRange().getEnd(),
              ";}");
          }
          mRewriter.ReplaceText(TI.mCallExpr->getSourceRange(), text.second);
          mRewriter.InsertTextBefore(TI.mStmt->getSourceRange().getBegin(),
            text.first);
        }
      }
    }
  }
  return;
}

std::string FInliner::getSourceText(const clang::SourceRange& SR) const {
  return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(SR),
    mContext.getSourceManager(), mContext.getLangOpts());
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
    const std::string& prefix,
    std::set<std::string>& identifiers) const {
  std::string identifier(prefix);
  int count = 0;
  bool ok = false;
  while (ok == false) {
    ok = true;
    if (std::find(std::begin(identifiers), std::end(identifiers), identifier)
      != std::end(identifiers)) {
      ok = false;
      identifier = prefix + std::to_string(count);
      ++count;
    }
  }
  identifiers.insert(identifier);
  return identifier;
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

bool FInliner::requiresBraces(clang::Stmt* S) const {
  if (clang::isa<clang::DeclStmt>(S) == true) {
    clang::DeclStmt* DS = reinterpret_cast<clang::DeclStmt*>(S);
    std::set<clang::Decl*> decls(DS->decl_begin(), DS->decl_end());
    std::set<clang::DeclRefExpr*> refs;
    std::copy_if(std::begin(mRefs), std::end(mRefs),
      std::inserter(refs, std::begin(refs)),
      [&](clang::DeclRefExpr* arg) -> bool {
      return std::find(std::begin(decls), std::end(decls), arg->getFoundDecl())
        != std::end(decls);
    });
    for (auto obj : refs) {
      if (DS->getSourceRange().getBegin().getRawEncoding()
        <= obj->getSourceRange().getBegin().getRawEncoding()
        && obj->getSourceRange().getEnd().getRawEncoding()
        <= DS->getSourceRange().getEnd().getRawEncoding()) {
        refs.erase(obj);
      }
    }
    return refs.size() == 0;
  }
  return true;
}

std::string FInlinerAction::createProjectFile(
    const std::vector<std::string>& sources) {
  const char projectFile[]{".proj.c"};
  std::error_code ec;
  llvm::raw_fd_ostream out(projectFile, ec, llvm::sys::fs::OpenFlags::F_Text);
  if (out.has_error() == true) {
    return std::string();
  }
  for (auto& source : sources) {
    out << "#include \"" << source << "\"\n";
  }
  out.close();
  return projectFile;
}

std::unique_ptr<clang::ASTConsumer>
FInlinerAction::CreateASTConsumer(
    clang::CompilerInstance& CI, llvm::StringRef InFile) {
  return std::unique_ptr<FInliner>(
    new FInliner(CI, InFile, mTfmCtx.get(), mQueryManager));
}

FInlinerAction::FInlinerAction(
    std::vector<std::string> CL, QueryManager* QM)
  : ActionBase(QM), mTfmCtx(new TransformationContext(CL)) {
}

inline FilenameAdjuster getFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    llvm::SmallString<128> Path = Filename;
    llvm::sys::path::replace_extension(
      Path, ".inl" + llvm::sys::path::extension(Path));
    return Path.str();
  };
}

bool FInlinerAction::format(
    clang::Rewriter& Rewriter, clang::FileID FID) const {
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
  std::string ChangedCode = clang::tooling::applyAllReplacements(
    Code->getBuffer(), Replaces);
  for (const auto& R : Replaces) {
    Ranges.push_back({ R.getOffset(), R.getLength() });
  }
  clang::tooling::Replacements FormatChanges = clang::format::reformat(
    FormatStyle, ChangedCode, Ranges, SM.getFileEntryForID(FID)->getName());
  Replaces = clang::tooling::mergeReplacements(Replaces, FormatChanges);
  clang::tooling::applyAllReplacements(Replaces, Rewriter);
  return false;
}

void FInlinerAction::EndSourceFileAction() {
  mTfmCtx->release(getFilenameAdjuster());
  clang::Rewriter& Rewriter = mTfmCtx->getRewriter();
  clang::SourceManager& SM = Rewriter.getSourceMgr();
  for (auto I = Rewriter.buffer_begin(), E = Rewriter.buffer_end();
    I != E; ++I) {
    const clang::FileEntry* Entry = SM.getFileEntryForID(I->first);
    std::string Name = getFilenameAdjuster()(Entry->getName());
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> CodeOrErr
      = llvm::MemoryBuffer::getFile(Name);
    if (std::error_code EC = CodeOrErr.getError()) {
      llvm::errs() << EC.message() << '\n';
      return;
    }
    clang::FileID FID = SM.createFileID(SM.getFileManager().getFile(Name),
      clang::SourceLocation(), clang::SrcMgr::C_User);
    clang::Rewriter Rewrite(SM, clang::LangOptions());
    format(Rewrite, FID);
    if (Rewrite.overwriteChangedFiles() == false) {
      llvm::outs() << "File " << '"' << Name << '"' << " was created" << '\n';
    }
  }
  return;
}

#endif
