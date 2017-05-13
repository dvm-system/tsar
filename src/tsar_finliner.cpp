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
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

// TODO(jury.zykov@yandex.ru): direct argument passing without local variables when possible

using namespace tsar;
using namespace detail;

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
  return true;
}

bool FInliner::VisitForStmt(clang::ForStmt* FS) {
  mFSs.push_back(FS);
  // build CFG for function which _possibly_ contains calls of functions
  // which can be inlined
  std::unique_ptr<clang::CFG> CFG = clang::CFG().buildCFG(
    nullptr, FS, &mContext, clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + mCurrentFD->getName()).str().data());
  for (auto B : *CFG) {
    for (auto I1 = B->begin(); I1 != B->end(); ++I1) {
      if (llvm::Optional<clang::CFGStmt> CS = I1->getAs<clang::CFGStmt>()) {
        clang::Stmt* S = const_cast<clang::Stmt*>(CS->getStmt());
        if (llvm::isa<clang::CallExpr>(S) == true) {
          clang::CallExpr* CE = reinterpret_cast<clang::CallExpr*>(S);
          bool inLoop = false;
          clang::SourceLocation beginCE
            = getLoc(CE->getSourceRange().getBegin());
          clang::SourceLocation endCE
            = getLoc(CE->getSourceRange().getEnd());
          for (auto it = mFSs.rbegin(); it != mFSs.rend(); ++it) {
            clang::SourceLocation beginCur
              = getLoc((*it)->getSourceRange().getBegin());
            clang::SourceLocation endCur
              = getLoc((*it)->getSourceRange().getEnd());
            if (beginCur <= beginCE && endCE <= endCur) {
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
              clang::SourceLocation beginS
                = getLoc(S->getSourceRange().getBegin());
              clang::SourceLocation endS
                = getLoc(S->getSourceRange().getEnd());
              clang::SourceLocation beginP
                = getLoc(P->getSourceRange().getBegin());
              clang::SourceLocation endP
                = getLoc(P->getSourceRange().getEnd());
              // in basic block each instruction can both depend or not
              // on results of previous instructions
              // we are looking for the last statement which on some dependency
              // depth references found callExpr
              if (beginS <= beginP && endP <= endS) {
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
            clang::SourceLocation beginS
              = getLoc(S->getSourceRange().getBegin());
            clang::SourceLocation endS
              = getLoc(S->getSourceRange().getEnd());
            clang::SourceLocation beginP
              = getLoc(P->getSourceRange().getBegin());
            clang::SourceLocation endP
              = getLoc(P->getSourceRange().getEnd());
            if (beginS <= beginP && endP <= endS) {
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
  mDeclRefs[mCurrentFD].insert(DRE);
  // parameter reference
  if (llvm::isa<clang::ParmVarDecl>(DRE->getDecl()) == true) {
    mTs[mCurrentFD].addParmRef(
      reinterpret_cast<clang::ParmVarDecl*>(DRE->getDecl()), DRE);
  }
  return true;
}

bool FInliner::VisitReturnStmt(clang::ReturnStmt* RS) {
  mTs[mCurrentFD].addRetStmt(RS);
  return true;
}

bool FInliner::VisitExpr(clang::Expr* E) {
  mTypeRefs[mCurrentFD].insert(const_cast<clang::Type*>(
    E->getType().getCanonicalType().getTypePtrOrNull()));
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
  return tokens;
}

std::pair<std::string, std::string> FInliner::compile(
  const TemplateInstantiation& TI, const std::vector<std::string>& args,
  std::set<std::string>& decls) {
  assert(TI.mTemplate->getFuncDecl()->getNumParams() == args.size()
    && "Undefined behavior: incorrect number of arguments specified");
  clang::Rewriter lRewriter(mCompiler.getSourceManager(),
    mCompiler.getLangOpts());
  clang::SourceManager& SM = lRewriter.getSourceMgr();
  std::string params;
  std::string context;
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
    for (auto& decl : mGlobalDecls) {
      if (clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(decl)) {
        if (std::find(std::begin(tokens), std::end(tokens), ND->getName())
          != std::end(tokens)) {
          context += getSourceText(getRange(ND)) + ";";
        }
      }
    }
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

  auto pr = [&](const std::pair<clang::FunctionDecl*,
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
        lRewriter.ReplaceText(getRange(TI.mStmt), text.first);
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
    context = "";
    std::vector<std::string> tokens
      = tokenize(TI.mTemplate->getFuncDecl()->getReturnType().getAsString(),
        mIdentifierPattern);
    for (auto& decl : mGlobalDecls) {
      if (clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(decl)) {
        if (std::find(std::begin(tokens), std::end(tokens), ND->getName())
          != std::end(tokens)) {
          context += getSourceText(getRange(ND)) + ";";
        }
      }
    }
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
  std::set<clang::FunctionDecl*> callable;
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      const clang::FunctionDecl* definition = nullptr;
      TI.mCallExpr->getDirectCallee()->hasBody(definition);
      TI.mTemplate = &mTs.at(const_cast<clang::FunctionDecl*>(definition));
      callable.insert(const_cast<clang::FunctionDecl*>(definition));
    }
  }
  // complete global declarations to avoid redefinitions
  for (auto it = mContext.getTranslationUnitDecl()->decls_begin();
    it != mContext.getTranslationUnitDecl()->decls_end(); ++it) {
    if (std::find_if(it, mContext.getTranslationUnitDecl()->decls_end(),
      [&](clang::Decl* lhs) -> bool {
      return *it != lhs
        && getRange(lhs).getBegin() <= getRange(*it).getBegin()
        && getRange(*it).getEnd() <= getRange(lhs).getEnd();
    }) == mContext.getTranslationUnitDecl()->decls_end()) {
      mGlobalDecls.insert(*it);
    }
  }
  // identifiers in scope
  std::map<clang::FunctionDecl*, std::set<std::string>> decls;
  for (auto& TIs : mTIs) {
    for (auto& decl : Context.getTranslationUnitDecl()->decls()) {
      if (clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(decl)) {
        decls[TIs.first].insert(ND->getName());
      }
    }
    for (auto& decl : TIs.first->decls()) {
      if (clang::NamedDecl* ND = clang::dyn_cast<clang::NamedDecl>(decl)) {
        decls[TIs.first].insert(ND->getName());
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
  // disable instantiation of variadic functions
  for (auto& pair : mTs) {
    if (pair.first->isVariadic() == true) {
      pair.second.setFuncDecl(nullptr);
    }
  }
  // disable instantiation of recursive functions
  std::set<clang::FunctionDecl*> recursive;
  for (auto& pair : mTIs) {
    bool ok = true;
    std::set<clang::FunctionDecl*> callers = { pair.first };
    std::set<clang::FunctionDecl*> callees;
    for (auto& TIs : pair.second) {
      if (TIs.mTemplate->getFuncDecl() != nullptr) {
        callees.insert(TIs.mTemplate->getFuncDecl());
      }
    }
    while (ok == true && callees.size() != 0) {
      std::set<clang::FunctionDecl*> intersection;
      std::set_intersection(std::begin(callers), std::end(callers),
        std::begin(callees), std::end(callees),
        std::inserter(intersection, std::end(intersection)));
      if (intersection.size() != 0) {
        ok = false;
        break;
      } else {
        std::set<clang::FunctionDecl*> tmp;
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
  // get functions' intermodular calls
  std::set<TemplateInstantiation*> intermodularTIs;
  std::set<Template*> intermodularTs;
  for (auto& TIs : mTIs) {
    for (auto& TI : TIs.second) {
      if (TI.mTemplate->getFuncDecl() != nullptr
        && mSourceManager.getFileID(getRange(TI.mFuncDecl).getBegin())
        != mSourceManager.getFileID(
          getRange(TI.mTemplate->getFuncDecl()).getBegin())) {
        intermodularTIs.insert(&TI);
        intermodularTs.insert(TI.mTemplate);
      }
    }
  }
  // check external dependencies in intermodular case
  // [C99 6.2.1] identifier can denote: object, function, tag/member of
  // struct/union/enum, typedef name, label name, macro name, macro parameter.
  // Label name - by definition has function scope, macro' objects should be
  // processed during preprocessing stage. Other cases are handled below.
  for (auto T : intermodularTs) {
    std::set<clang::Type*>& typeDecls = mTypeDecls[T->getFuncDecl()];
    std::set<clang::Type*>& typeRefs = mTypeRefs[T->getFuncDecl()];
    // stack for nested declarations
    std::stack<clang::Decl*> stack;
    for (auto decl : T->getFuncDecl()->decls()) {
      stack.push(decl);
    }
    while (stack.empty() == false) {
      clang::Decl* decl = stack.top();
      stack.pop();
      if (clang::TypeDecl* TD = clang::dyn_cast<clang::TypeDecl>(decl)) {
        typeDecls.insert(const_cast<clang::Type*>(TD->getTypeForDecl()));
        if (clang::RecordDecl* RD = clang::dyn_cast<clang::RecordDecl>(decl)) {
          for (auto& decl : RD->decls()) {
            stack.push(decl);
          }
        }
      } else if (clang::ValueDecl* VD = clang::dyn_cast<clang::ValueDecl>(decl)) {
        typeRefs.insert(const_cast<clang::Type*>(
          VD->getType().getCanonicalType().getTypePtrOrNull()));
      }
    }
    assert(typeRefs.find(nullptr) == std::end(typeRefs)
      && "Non-typed declaration was found");
    for (auto it = std::begin(typeRefs); it != std::end(typeRefs);) {
      if (typeDecls.find(*it) != std::end(typeDecls)) {
        it = typeRefs.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it1 = std::begin(typeRefs); it1 != std::end(typeRefs);) {
      std::vector<std::string> tokens
        = tokenize(clang::QualType(*it1, 0).getAsString(), mIdentifierPattern);
      for (auto it2 = std::begin(tokens); it2 != std::end(tokens);) {
        if (std::find(std::begin(mKeywords), std::end(mKeywords), *it2)
          != std::end(mKeywords)) {
          it2 = tokens.erase(it2);
        } else {
          ++it2;
        }
      }
      if (tokens.size() != 0) {
        ++it1;
      } else {
        it1 = typeRefs.erase(it1);
      }
    }
    // TODO: this type can contain reference to variable
    bool ok = true;
    if (typeRefs.size() != 0) {
      for (auto& ref : typeRefs) {
        llvm::errs() << '"' << T->getFuncDecl()->getName() << '"' << ':'
          << " nonlocal type reference found:" << ' ' << '"'
          << clang::QualType(ref, 0).getAsString() << '"' << ' ' << ref << '\n';
      }
      ok = false;
    }
    for (auto DRE : mDeclRefs[T->getFuncDecl()]) {
      if (std::find(T->getFuncDecl()->decls_begin(),
        T->getFuncDecl()->decls_end(), DRE->getFoundDecl())
        == T->getFuncDecl()->decls_end()) {
        llvm::errs() << '"' << T->getFuncDecl()->getName() << '"' << ':'
          << " nonlocal value reference found:" << ' ' << '"'
          << getSourceText(getRange(DRE)) << '"' << ' ' << DRE << '\n';
        ok = false;
      }
    }
    if (ok == false) {
      for (auto TI : intermodularTIs) {
        if (TI->mTemplate == T) {
          TI->mTemplate = nullptr;
        }
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
        llvm::errs() << "  " << '"'
          << getSourceText(getRange(TI.mCallExpr)) << '"' << '\n';
      }
      llvm::errs() << '\n';
    }
    llvm::errs() << '\n';
    llvm::errs() << "Total templates:" << '\n';
    for (auto& T : mTs) {
      llvm::errs() << ' ' << '"' << T.first->getName() << '"' << '\n';
    }
    llvm::errs() << '\n';
    llvm::errs() << "Unused templates (removed) ("
      << callable.size() << ')' << '\n';
    llvm::errs() << '\n';
    llvm::errs() << "Disabled templates ("
      << std::count_if(std::begin(mTs), std::end(mTs),
        [&](const std::pair<clang::FunctionDecl*, Template>& lhs) -> bool {
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
    auto pr = [&](const std::pair<clang::FunctionDecl*, Template>& lhs)
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
          return getSourceText(getRange(arg));
        });
        fDecls.insert(std::begin(args), std::end(args));
        std::pair<std::string, std::string> text
          = compile(TI, args, fDecls);
        if (text.second.size() == 0) {
          mRewriter->ReplaceText(getRange(TI.mStmt), text.first);
        } else {
          if (requiresBraces(TI.mFuncDecl, TI.mStmt) == true) {
            text.first.insert(std::begin(text.first), '{');
            mRewriter->InsertTextAfterToken(getRange(TI.mStmt).getEnd(),
              ";}");
          }
          mRewriter->ReplaceText(getRange(TI.mCallExpr), text.second);
          mRewriter->InsertTextBefore(getRange(TI.mStmt).getBegin(),
            text.first);
        }
        mRewriter->InsertTextBefore(getRange(TI.mStmt).getBegin(),
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
  return {mSourceManager.getFileLoc(node->getSourceRange().getBegin()),
    mSourceManager.getFileLoc(node->getSourceRange().getEnd())};
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
    const std::string& prefix,
    std::set<std::string>& identifiers) const {
  int count = 0;
  std::string identifier(prefix + std::to_string(count++));
  bool ok = false;
  while (ok == false) {
    ok = true;
    if (std::find(std::begin(identifiers), std::end(identifiers), identifier)
      != std::end(identifiers)) {
      ok = false;
      identifier = prefix + std::to_string(count++);
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

bool FInliner::requiresBraces(clang::FunctionDecl* FD, clang::Stmt* S) {
  if (clang::DeclStmt* DS = clang::dyn_cast<clang::DeclStmt>(S)) {
    std::set<clang::Decl*> decls(DS->decl_begin(), DS->decl_end());
    std::set<clang::DeclRefExpr*> refs;
    std::copy_if(std::begin(mDeclRefs[FD]), std::end(mDeclRefs[FD]),
      std::inserter(refs, std::begin(refs)),
      [&](clang::DeclRefExpr* arg) -> bool {
      return std::find(std::begin(decls), std::end(decls), arg->getFoundDecl())
        != std::end(decls);
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

std::string FInlinerAction::createProjectFile(
    const std::vector<std::string>& sources) {
  mSources = sources;
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
    clang::tooling::Range(Offset, Length)});
  clang::format::FormatStyle FormatStyle
    = clang::format::getStyle("LLVM", "", "LLVM");
  clang::tooling::Replacements Replaces = clang::format::sortIncludes(
    FormatStyle, Code->getBuffer(), Ranges,
    SM.getFileEntryForID(FID)->getName());
  llvm::Expected<std::string> ChangedCode
    = clang::tooling::applyAllReplacements(Code->getBuffer(), Replaces);
  assert(bool(ChangedCode) == true && "Failed to apply replacements");
  for (const auto& R : Replaces) {
    Ranges.push_back({R.getOffset(), R.getLength()});
  }
  clang::tooling::Replacements FormatChanges = clang::format::reformat(
    FormatStyle, ChangedCode.get(), Ranges, SM.getFileEntryForID(FID)->getName());
  Replaces = Replaces.merge(FormatChanges);
  clang::tooling::applyAllReplacements(Replaces, Rewriter);
  return false;
}

std::vector<std::string> FInlinerAction::mSources = {};

void FInlinerAction::EndSourceFileAction() {
  mTfmCtx->release(getFilenameAdjuster());
  clang::Rewriter& Rewriter = mTfmCtx->getRewriter();
  clang::SourceManager& SM = Rewriter.getSourceMgr();
  clang::Rewriter Rewrite(SM, clang::LangOptions());
  llvm::SmallVector<char, 128> cwd;
  llvm::sys::fs::current_path(cwd);
  std::vector<std::string> sources;
  std::transform(std::begin(mSources), std::end(mSources),
    std::inserter(sources, std::end(sources)),
    [&](const std::string& lhs) -> std::string {
    llvm::SmallVector<char, 128> path = cwd;
    llvm::sys::path::append(path, lhs);
    llvm::sys::path::remove_dots(path, true);
    return std::string(path.data(), path.size());
  });
  for (auto I = Rewriter.buffer_begin(), E = Rewriter.buffer_end();
    I != E; ++I) {
    const clang::FileEntry* Entry = SM.getFileEntryForID(I->first);
    std::string Name = getFilenameAdjuster()(Entry->getName());
    clang::FileID FID = SM.createFileID(SM.getFileManager().getFile(Name),
      clang::SourceLocation(), clang::SrcMgr::C_User);
    format(Rewrite, FID);
    mSources[std::find_if(std::begin(sources), std::end(sources),
      [&](const std::string& lhs) -> bool {
      return SM.getFileManager().getFile(lhs)
        == Entry;
    }) - std::begin(sources)] = Name;
    llvm::errs() << Name << ':' << " ready for rewriting" << '\n';
  }
  if (Rewrite.overwriteChangedFiles() == false) {
    llvm::errs() << "All changes were successfully saved" << '\n';
  }
  // rewrite project file with created *.inl.*'s for further analysis
  createProjectFile(mSources);
  return;
}
