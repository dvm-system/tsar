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

//#define FINLINER_PLUGIN

#include "tsar_finliner.h"

#include <algorithm>
#include <numeric>
#include <regex>
#include <map>
#include <set>
#include <vector>

#include "clang/Analysis/CFG.h"
#ifndef FINLINER_PLUGIN
#include "clang/Frontend/FrontendAction.h"
#else
#include "clang/Frontend/FrontendPluginRegistry.h"
#endif
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"

// TODO(jury.zykov@yandex.ru): collect identifiers' scope info for correct placement of closing braces
// TODO(jury.zykov@yandex.ru): fix indentation of all replaced/inserted strings (readable form)
// TODO(jury.zykov@yandex.ru): integration with tsar

static std::vector<llvm::StringRef> gIds;  // TMP: functions names to inline

bool FInliner::VisitFunctionDecl(clang::FunctionDecl* FD) {
  if (FD->isThisDeclarationADefinition() == false) {
    return true;
  }
  mCurrentFD = FD;
  if (std::find(std::begin(gIds), std::end(gIds), FD->getName())
    != std::end(gIds)) {
    // definition of function which must be inlined
    mTs[FD].setFuncDecl(FD);
  }
  // definition of function which _possibly_ contains calls of functions for inlining
  std::unique_ptr<clang::CFG> CFG = clang::CFG().buildCFG(
    FD, FD->getBody(), &mContext, clang::CFG::BuildOptions());
  for (auto B : *CFG) {
    for (auto I1 = B->begin(); I1 != B->end(); ++I1) {
      if (llvm::Optional<clang::CFGStmt> CS = I1->getAs<clang::CFGStmt>()) {
        clang::Stmt* S = const_cast<clang::Stmt*>(CS->getStmt());
        if (llvm::isa<clang::CallExpr>(S) == true) {
          clang::CallExpr* CE = reinterpret_cast<clang::CallExpr*>(S);
          if (std::find(
            gIds.begin(), gIds.end(), CE->getDirectCallee()->getName())
            == gIds.end()) {
            continue;
          }
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
                && (P->getLocEnd() < S->getLocEnd() || P->getLocEnd()
                  == S->getLocEnd())) {
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
          mTIs[FD].push_back({ FD, P, CE });
        }
      }
    }
  }
  return true;
}

bool FInliner::VisitDeclRefExpr(clang::DeclRefExpr* DRE) {
  if (std::find(std::begin(gIds), std::end(gIds), mCurrentFD->getName())
    == gIds.end()) {
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
  if (std::find(std::begin(gIds), std::end(gIds), mCurrentFD->getName())
    == gIds.end()) {
    return true;
  }
  mTs[mCurrentFD].addRetStmt(RS);
  return true;
}

bool FInliner::VisitLabelStmt(clang::LabelStmt* LS) {
  mLSs[mCurrentFD].push_back(LS);
  return true;
}

bool FInliner::VisitNamedDecl(clang::NamedDecl* ND) {
  mNDs[mCurrentFD].push_back(ND);
  return true;
}

std::vector<std::string> FInliner::construct(
    const std::string& type, const std::string& identifier) {
  const std::string pattern("\\w+|\\S");
  // [C99 6.7.2, 6.7.3]
  const std::vector<std::string> keywords = { "register",
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Bool", "_Complex", "struct", "union", "enum",
    "const", "restrict", "volatile" };
  clang::ast_matchers::MatchFinder MatchFinder;
  MatchFinder.addMatcher(clang::ast_matchers::varDecl().bind("varDecl"),
    &varDeclHandler);
  std::vector<std::string> tokens = tokenize(type, pattern);
  std::map<std::string, std::vector<int>> positions
    = preprocess(tokens, keywords);
  varDeclHandler.setParameters(join(tokens, " "),
    [&](const std::string& s) -> std::string {
    std::vector<std::string> tokens = tokenize(s, pattern);
    preprocess(tokens, keywords);
    return join(tokens, " ");
  });
  tokens.push_back(identifier);
  std::vector<int> counts(tokens.size());
  swap(llvm::errs(), llvm::nulls());
  for (int i = tokens.size() - 1; i >= 0; --i) {
    varDeclHandler.initCount();
    MatchFinder.matchAST(clang::tooling::buildASTFromCode(join(tokens, " ")
      + ";")->getASTContext());
    counts[i] = varDeclHandler.getCount();
    std::swap(tokens[i], tokens[std::max(i - 1, 0)]);
  }
  swap(llvm::errs(), llvm::nulls());
  assert(std::find_if(std::begin(counts), std::end(counts),
    [](int arg) -> bool {
    return arg != 0;
  }) != std::end(counts)
    && "At least one valid position must be found");
  assert(std::find_if(std::begin(counts), std::end(counts),
    [](int arg) -> bool {
    return arg > 1;
  }) != std::end(counts)
    && "Multiple declarations of one identifier are found");
  assert(std::count_if(std::begin(counts), std::end(counts),
    [](int arg) {
    return arg != 0;
  }) == 1
    && "Multiple equivalent variants are found");
  int position = std::find_if(std::begin(counts), std::end(counts),
    [](int arg) -> bool {
    return arg != 0;
  }) - std::begin(counts);
  tokens.erase(std::begin(tokens));
  for (auto& specifier : positions) {
    for (auto& pos : specifier.second) {
      if (pos >= position) {
        ++pos;
      }
    }
  }
  tokens.insert(std::begin(tokens) + position, identifier);
  postprocess(tokens, positions);
  return tokens;
}

std::pair<std::string, std::string> FInliner::compile(
    const TemplateInstantiation& TI, const std::vector<std::string>& args,
    std::set<std::string>& identifiers, std::set<std::string>& labels) {
  clang::Rewriter lRewriter(mCompiler.getSourceManager(),
    mCompiler.getLangOpts());
  std::string params;
  for (auto& PVD : TI.getTemplate()->getFuncDecl()->params()) {
    std::string identifier = addSuffix(PVD->getName(), identifiers);
    std::vector<std::string> tokens
      = construct(PVD->getType().getAsString(), identifier);
    params.append(join(tokens, " ") + " = "
      + args[PVD->getFunctionScopeIndex()] + ";\n");
    std::vector<clang::DeclRefExpr*> parameterReferences
      = TI.getTemplate()->getParmRefs(PVD);
    for (auto& DRE : parameterReferences) {
      lRewriter.ReplaceText(DRE->getSourceRange(), identifier);
    }
  }

  auto pr = [&](const std::pair<clang::FunctionDecl*,
      std::vector<TemplateInstantiation>> &lhs) -> bool {
    return lhs.first == TI.getTemplate()->getFuncDecl();
  };
  if (std::find_if(std::begin(mTIs), std::end(mTIs), pr) != std::end(mTIs)) {
    for (auto& TI : mTIs[TI.getTemplate()->getFuncDecl()]) {
      std::vector<std::string> args(TI.getCallExpr()->getNumArgs());
      std::transform(TI.getCallExpr()->arg_begin(), TI.getCallExpr()->arg_end(),
        std::begin(args),
        [&](const clang::Expr* arg) -> std::string {
        return lRewriter.getRewrittenText(arg->getSourceRange());
      });
      std::pair<std::string, std::string> text
        = compile(TI, args, identifiers, labels);
      if (text.second.size() == 0) {
        lRewriter.ReplaceText(TI.getStmt()->getSourceRange(), text.first);
      } else {
        lRewriter.ReplaceText(TI.getCallExpr()->getSourceRange(), text.second);
        lRewriter.InsertTextBefore(TI.getStmt()->getSourceRange().getBegin(),
          text.first);
        lRewriter.InsertTextAfterToken(
          TI.getStmt()->getSourceRange().getEnd(), ";}");
      }
    }
  }

  std::string identifier;
  std::string ret;
  std::string retLab = addSuffix("L", labels);
  std::vector<clang::ReturnStmt*> returnStmts = TI.getTemplate()->getRetStmts();
  if (TI.getTemplate()->getFuncDecl()->getReturnType()->isVoidType() == false) {
    identifier = addSuffix("R", identifiers);
    std::vector<std::string> tokens
      = construct(TI.getTemplate()->getFuncDecl()->getReturnType().getAsString(),
        identifier);
    ret = "{\n" + join(tokens, " ") + ";\n";
    for (auto& RS : returnStmts) {
      std::string text = "{" + identifier + " = "
        + lRewriter.getRewrittenText(RS->getRetValue()->getSourceRange())
        + ";goto " + retLab + ";}";
      lRewriter.ReplaceText(RS->getSourceRange(), text);
    }
    lRewriter.ReplaceText(TI.getCallExpr()->getSourceRange(), identifier);
  } else {
    for (auto& RS : returnStmts) {
      lRewriter.ReplaceText(RS->getSourceRange(), "goto " + retLab);
    }
  }
  std::string text = lRewriter.getRewrittenText(
    TI.getTemplate()->getFuncDecl()->getBody()->getSourceRange())
    + "\n" + retLab + ":;\n";
  text.insert(std::begin(text) + 1, std::begin(params), std::end(params));
  text.insert(std::begin(text), std::begin(ret), std::end(ret));
  /*llvm::errs() << "============ START ============" << '\n';
  llvm::errs() << text;
  llvm::errs() << "============= END =============" << '\n';*/
  return {text, identifier};
}

void FInliner::HandleTranslationUnit(clang::ASTContext& Context) {
  TraverseDecl(Context.getTranslationUnitDecl());
  assert(gIds.size() == mTs.size());
  // associate instantiations with templates
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      const clang::FunctionDecl* definition = nullptr;
      TI.getCallExpr()->getDirectCallee()->hasBody(definition);
      TI.setTemplate(&mTs.at(const_cast<clang::FunctionDecl*>(definition)));
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
        std::set<std::string> identifiers;
        std::transform(
            std::begin(mNDs[TI.getFuncDecl()]),
            std::end(mNDs[TI.getFuncDecl()]),
            std::inserter(identifiers, std::end(identifiers)),
            [&](clang::NamedDecl* ND) -> std::string {
            return ND->getName();
        });
        std::set<std::string> labels;
        std::transform(
            std::begin(mLSs[TI.getFuncDecl()]),
            std::end(mLSs[TI.getFuncDecl()]),
            std::inserter(labels, labels.end()),
            [&](clang::LabelStmt* LS) -> std::string {
            return LS->getName();
        });
        std::vector<std::string> args(TI.getCallExpr()->getNumArgs());
        std::transform(
            TI.getCallExpr()->arg_begin(), TI.getCallExpr()->arg_end(),
            std::begin(args),
            [&](const clang::Expr* arg) -> std::string {
            return getSourceText(arg->getSourceRange());
        });
        std::pair<std::string, std::string> text
          = compile(TI, args, identifiers, labels);
        if (text.second.size() == 0) {
          mRewriter.ReplaceText(TI.getStmt()->getSourceRange(), text.first);
        } else {
          mRewriter.ReplaceText(TI.getCallExpr()->getSourceRange(),
            text.second);
          mRewriter.InsertTextBefore(TI.getStmt()->getSourceRange().getBegin(),
            text.first);
          mRewriter.InsertTextAfterToken(TI.getStmt()->getSourceRange().getEnd(), ";}");
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
  char tmp[sizeof(T)];  // stack overflow is probably possible
  std::memcpy(tmp, &lhs, sizeof(T));
  std::memcpy(&lhs, &rhs, sizeof(T));
  std::memcpy(&rhs, tmp, sizeof(T));
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

std::map<std::string, std::vector<int>> FInliner::preprocess(
    std::vector<std::string>& tokens,
    const std::vector<std::string>& keywords) const {
  auto isSpecifier = [&](const std::string& arg) -> bool {
    return arg == "struct" || arg == "union" || arg == "enum";
  };
  std::map<std::string, std::vector<int>> positions;
  for (int i = 0; i < tokens.size(); ++i) {
    if (isSpecifier(tokens[i]) == true) {
      positions[tokens[i]].push_back(i);
    }
  }
  for (int i = 0; i < tokens.size(); ++i) {
    std::locale locale;
    if (std::isalpha(tokens[i][0], locale) == true || tokens[i][0] == '_') {
      if (std::find(std::begin(keywords), std::end(keywords), tokens[i])
        == std::end(keywords)) {
        positions[tokens[i]].push_back(i);
        tokens[i] = "int";
      }
    }
  }
  tokens.erase(
    std::remove_if(std::begin(tokens), std::end(tokens), isSpecifier),
    tokens.end());
  return positions;
}

void FInliner::postprocess(
    std::vector<std::string>& tokens,
    std::map<std::string, std::vector<int>> positions) const {
  for (auto& keyword : positions) {
    for (auto& pos : keyword.second) {
      if (tokens[pos] == "int") {
        tokens[pos] = keyword.first;
      } else {
        tokens.insert(std::begin(tokens) + pos, keyword.first);
      }
    }
  }
  return;
}


/*#ifdef FINLINER_PLUGIN
class FInlinerAction : public clang::PluginASTAction {
public:
  void EndSourceFileAction() {
  }

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance& Compiler, llvm::StringRef InFile) {
    Rewriter = clang::Rewriter(
      Compiler.getSourceManager(), Compiler.getLangOpts());
    return std::unique_ptr<clang::ASTConsumer>(
      new FInliner(Compiler, Rewriter));
  }

  bool ParseArgs(const clang::CompilerInstance& CI,
    const std::vector<std::string> &args) {
    return true;
  }

private:
    clang::Rewriter Rewriter;
};


static clang::FrontendPluginRegistry::Add<FInlinerAction>
X("finliner", "frontend inliner");
#else
class FInlinerAction : public clang::ASTFrontendAction {
public:
  void EndSourceFileAction() {
    clang::SourceManager& SM = mRewriter.getSourceMgr();
    std::error_code EC;
    static int counter = 0;
    llvm::raw_fd_ostream fout("test" + std::to_string(counter++) + ".c", EC, llvm::sys::fs::OpenFlags::F_Text);
    mRewriter.getEditBuffer(SM.getMainFileID()).write(fout);
  }

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance& Compiler, llvm::StringRef InFile) {
    mRewriter = clang::Rewriter(
      Compiler.getSourceManager(), Compiler.getLangOpts());
    return std::unique_ptr<clang::ASTConsumer>(
      new FInliner(Compiler, mRewriter));
  }

private:
  clang::Rewriter mRewriter;
};

static llvm::cl::OptionCategory FInlinerCategory("finliner options");

int main(int argc, const char* argv[]) {
  if (argc > 1) {
    llvm::outs() << "Running FInliner on " << argv[1] << '\n';
    gIds = {"f1", "f2", "f3"};
    clang::tooling::CommonOptionsParser OptionsParser(
      argc, argv, FInlinerCategory);
    clang::tooling::ClangTool Tool(
      OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(
      clang::tooling::newFrontendActionFactory<FInlinerAction>().get());
  }
  return 0;
}
#endif*/
