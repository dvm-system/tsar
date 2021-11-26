//===- SplitDecls.cpp - Source-level Splitting of Local Objects - *- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
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
// The file declares a pass to perform splitting of objects in a specified scope.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/SplitDecls.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Utils.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include "clang/Rewrite/Core/Rewriter.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <string>
#include <stack>
#include <iostream>
#include <deque>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-split-decls"

char ClangSplitDeclsPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangSplitDeclsPass, "clang-split",
  "Separation of variable declaration statements (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangSplitDeclsPass, "clang-split-decls",
  "Separation of variable declaration statements (Clang)", false, false,
  tsar::TransformationQueryManager::getPassRegistry())

void ClangSplitDeclsPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createClangSplitDeclsPass() {
  return new ClangSplitDeclsPass();
}

namespace {
/// The visitor searches a pragma `split` and performs splitting for a scope
/// after it. It also checks absence a macros in this scope and print some
/// other warnings.
bool isNotSingleFlag = false;
bool isAfterNotSingleFlag = false;
class ClangSplitter : public RecursiveASTVisitor<ClangSplitter> {
public:
  ClangSplitter(TransformationContext &TfmCtx, const ASTImportInfo &ImportInfo,
      ClangGlobalInfoPass::RawInfo &RawInfo) :
    mTfmCtx(&TfmCtx), mImportInfo(ImportInfo),
    mRawInfo(&RawInfo), mRewriter(TfmCtx.getRewriter()),
    mContext(TfmCtx.getContext()), mSrcMgr(mRewriter.getSourceMgr()),
    mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) { // to traverse the parse tree and visit each statement
    if (!S) {
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    Pragma P(*S); // the Pragma class is used to check if a statement is a pragma or not
    if (findClause(P, ClauseId::SplitDeclaration, mClauses)) { // mClauses contains all SplitDeclaration pragmas
      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove; // a vector of statements that will match the root in the tree
      auto IsPossible = pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts,
                                            mImportInfo, ToRemove); // ToRemove - the range of positions we want to remove
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), mClauses.front()->getBeginLoc(),
            tsar::diag::warn_remove_directive);
      Rewriter::RewriteOptions RemoveEmptyLine;
      /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
      /// set to true then removing (in RewriterBuffer) works incorrect.
      RemoveEmptyLine.RemoveLineIfEmpty = false;
      for (auto SR : ToRemove)
        mRewriter.RemoveText(SR, RemoveEmptyLine); // delete each range
      if (isNotSingleFlag) {
        SourceRange toInsert(notSingleDeclStart, notSingleDeclEnd);
        mRewriter.RemoveText(toInsert, RemoveEmptyLine);
      }
      return true;
    }
    Rewriter::RewriteOptions RemoveEmptyLine;
    RemoveEmptyLine.RemoveLineIfEmpty = false;

    if (isNotSingleFlag) {
      SourceRange toInsert(notSingleDeclStart, notSingleDeclEnd);
      mRewriter.RemoveText(toInsert, RemoveEmptyLine);

    }
    if (mClauses.empty() || !isa<CompoundStmt>(S) &&
        !isa<ForStmt>(S) && !isa<DoStmt>(S) && !isa<WhileStmt>(S))
      return RecursiveASTVisitor::TraverseStmt(S);
    // There was a pragma split, so check absence of macros and perform
    // splitting.
    mClauses.clear();
    bool StashSplitState = mActiveSplit;
    // We do not perform search of macros in case of nested 'split'
    // directives and active splitting. The search has been already performed.
    if (!mActiveSplit) {
      bool HasMacro = false;
      for_each_macro(S, mSrcMgr, mContext.getLangOpts(), mRawInfo->Macros,
        [&HasMacro, this](clang::SourceLocation Loc) {
          if (!HasMacro) {
            toDiag(mContext.getDiagnostics(), Loc,
              tsar::diag::warn_splitdeclaration_macro_prevent);
            HasMacro = true;
        }
      });
      // We should not stop traverse because some nested split directives
      // may exist.
      if (HasMacro)
        return RecursiveASTVisitor::TraverseStmt(S);
      mActiveSplit = true;
    }
    auto Res = RecursiveASTVisitor::TraverseStmt(S);
    mActiveSplit = StashSplitState;
    return Res;
  }

  void buildTxtStr(std::string varType, std::string varName) {
    std::vector<std::string> tokens;
    std::string delimiter(" ");
    size_t prev = 0;
    size_t next;
    size_t delta = delimiter.length();
    while((next = varType.find(delimiter, prev)) != std::string::npos){
      tokens.push_back(varType.substr(prev, next-prev));
      prev = next + delta;
    }
    tokens.push_back(varType.substr(prev));
    txtStr = "";
    for (std::string token : tokens) {
      if (token == tokens.back())
        txtStr += varName + token + ";\n";
      else
        txtStr += token + " ";
    }
  }

  // bool VisitVarDecl(VarDecl *S) { // to traverse the parse tree and visit each statement
  //   if (isNotSingleFlag) {
  //     std::string varType = S->getType().getAsString();
  //     SourceLocation locat = S->getLocation();
  //     CharSourceRange txtToInsert(locat, true);
  //     std::string varName = S->getName().str();
  //     int n = varType.length();
  //     char char_array[n + 1];
  //     strcpy(char_array, varType.c_str());
  //     if (strchr(char_array, '[')) {
  //       buildTxtStr(varType, varName);
  //     } else {
  //       txtStr = varType + " " + varName + ";\n";
  //     }
  //     mRewriter.InsertTextAfterToken(notSingleDeclEnd, txtStr);
  //   }
  //   return true;
  // }

  bool VisitVarDecl(VarDecl *S) { // to traverse the parse tree and visit each statement
    if (isNotSingleFlag) {
      varDeclsNum++;
      SourceRange toInsert(notSingleDeclStart, notSingleDeclEnd);
      ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
      SourceRange Range(S->getLocation());
      varDeclsStarts.push_front(S->getBeginLoc());
      varDeclsEnds.push_front(S->getEndLoc());
      SourceRange varDeclRange(S->getBeginLoc(), S->getEndLoc());
      if (varDeclsNum == 1) {
        SourceRange toInsert2(Range.getBegin(), S->getEndLoc());
        txtStr = Canvas.getRewrittenText(varDeclRange).str();
        Canvas.RemoveText(toInsert2);
        varDeclType = Canvas.getRewrittenText(varDeclRange);
      }
      if (varDeclsNum > 1) {
        SourceRange prevVarDeclRange(varDeclsStarts.back(), varDeclsEnds.back());
        varDeclsStarts.pop_back();
        varDeclsEnds.pop_back();
        Canvas.ReplaceText(prevVarDeclRange, varDeclType);
        txtStr = Canvas.getRewrittenText(varDeclRange).str();
        auto it = std::remove(txtStr.begin(), txtStr.end(), ',');
        txtStr.erase(it, txtStr.end());
      }
      mRewriter.InsertTextAfterToken(notSingleDeclEnd, txtStr + ";\n");
    }
    return true;
  }

  bool TraverseDeclStmt(DeclStmt *S) {
    bool tmp;
    if(!(S->isSingleDecl())) {
      if (!isNotSingleFlag)
        varDeclsNum = 0;
      isNotSingleFlag = true;
      notSingleDeclStart = S->getBeginLoc();
      notSingleDeclEnd = S->getEndLoc();
    } else {
      isNotSingleFlag = false;
    }
    return RecursiveASTVisitor::TraverseDeclStmt(S);
  }

  bool VisitStmt(Stmt *S) {
    if (!mClauses.empty()) {
      mClauses.clear();
    }
    return RecursiveASTVisitor::VisitStmt(S);
  }

  bool VisitDecl(Decl * D) {
    if (!mClauses.empty()) {
      toDiag(mContext.getDiagnostics(), mClauses.front()->getBeginLoc(),
        tsar::diag::warn_unexpected_directive);
      mClauses.clear();
    }
    return RecursiveASTVisitor::VisitDecl(D);
  }

  private:

  TransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  ClangGlobalInfoPass::RawInfo *mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  bool mActiveSplit = false;
  DenseSet<DeclStmt*> mMultipleDecls;
  std::deque<SourceLocation> varDeclsStarts;
  std::deque<SourceLocation> varDeclsEnds;
  int varDeclsNum = 0;
  SourceLocation notSingleDeclStart;
  SourceLocation notSingleDeclEnd;
  std::string txtStr;
  std::string varDeclType;
};
}

bool ClangSplitDeclsPass::runOnModule(llvm::Module &M) {
  auto &TfmInfo = getAnalysis<TransformationEnginePass>();
  auto *TfmCtx{TfmInfo ? TfmInfo->getContext(M) : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  ClangSplitter Vis(*TfmCtx, *ImportInfo, GIP.getRawInfo());
  Vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  return false;
}