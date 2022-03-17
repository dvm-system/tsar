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

#include <clang/AST/TypeLoc.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <string>
#include <stack>
#include <iostream>
#include <deque>
#include <map>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-split-decls"

char ClangSplitDeclsPass::ID = 0;

struct notSingleDecl {
  bool isNotSingleFlag = false;
  int varDeclsNum = 0;
  bool isFirstVar = true;
  SourceLocation notSingleDeclStart;
  SourceLocation notSingleDeclEnd;
  std::deque<SourceLocation> starts;
  std::deque<SourceLocation> ends;
  std::deque<std::string> names;
  std::string varDeclType;
};


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
class ClangSplitter : public RecursiveASTVisitor<ClangSplitter> {
public:
  ClangSplitter(TransformationContext &TfmCtx, const ASTImportInfo &ImportInfo, const GlobalInfoExtractor &GlobalInfo,
      ClangGlobalInfoPass::RawInfo &RawInfo) :
    mTfmCtx(&TfmCtx), mImportInfo(ImportInfo), mGlobalInfo(GlobalInfo),
    mRawInfo(&RawInfo), mRewriter(TfmCtx.getRewriter()),
    mContext(TfmCtx.getContext()), mSrcMgr(mRewriter.getSourceMgr()),
    mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) { // to traverse the parse tree and visit each statement
    if (!S) {
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    Pragma P(*S); // the Pragma class is used to check if a statement is a pragma or not
    splitPragmaFlag = true;
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
      std::map<clang::SourceLocation, notSingleDecl>::iterator it;
      for (it = globalVarDeclsMap.begin(); it != globalVarDeclsMap.end(); it++) {
        if (it->second.isNotSingleFlag) {
          SourceRange toInsert(it->second.notSingleDeclStart, it->second.notSingleDeclEnd);
          ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
          std::cout << "Global range: " << Canvas.getRewrittenText(toInsert).str() << std::endl;
          mRewriter.RemoveText(toInsert, RemoveEmptyLine);
        }
      }
      if (localVarDecls.isNotSingleFlag) {
        SourceRange toInsert(localVarDecls.notSingleDeclStart, localVarDecls.notSingleDeclEnd);
        mRewriter.RemoveText(toInsert, RemoveEmptyLine);
      }
      return true;
    }
    Rewriter::RewriteOptions RemoveEmptyLine;
    RemoveEmptyLine.RemoveLineIfEmpty = false;
    if (splitPragmaFlag) {
      if (localVarDecls.isNotSingleFlag) {
        SourceRange toInsert(localVarDecls.notSingleDeclStart, localVarDecls.notSingleDeclEnd);
        mRewriter.RemoveText(toInsert, RemoveEmptyLine);
        while (localVarDecls.names.size()) {
          if (localVarDecls.isFirstVar) {
            mRewriter.InsertTextAfterToken(localVarDecls.notSingleDeclEnd, localVarDecls.names.back());
            localVarDecls.isFirstVar = false;
          } else {
            mRewriter.InsertTextAfterToken(localVarDecls.notSingleDeclEnd, localVarDecls.varDeclType + localVarDecls.names.back());
          }
          localVarDecls.names.pop_back();
        }
      }

      std::map<clang::SourceLocation, notSingleDecl>::iterator it;
      for (it = globalVarDeclsMap.begin(); it != globalVarDeclsMap.end(); it++) {
        if (it->second.isNotSingleFlag) {
          // SourceRange toInsert(it->second.notSingleDeclStart, it->second.ends.back());
          SourceRange toInsert(it->second.notSingleDeclStart, it->second.ends.back());
          mRewriter.RemoveText(toInsert, RemoveEmptyLine);
          while (it->second.names.size()) {
            if (it->second.isFirstVar) {
              mRewriter.InsertTextAfterToken(it->second.ends.back(), it->second.names.back());
              it->second.isFirstVar = false;
            } else {
              if (it->second.names.size() == 1) {
                mRewriter.InsertTextAfterToken(it->second.ends.back(), it->second.varDeclType + it->second.names.back());
              } else {
                mRewriter.InsertTextAfterToken(it->second.ends.back(), it->second.varDeclType + it->second.names.back() + ";\n");
              }
            }
            it->second.names.pop_back();
          }
        }
      }
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

  std::string getType(SourceLocation start, SourceLocation endLoc) {
    SourceRange varDeclRange(start, endLoc);
    std::string type = mRewriter.getRewrittenText(varDeclRange);
    std::cout << "type = " << type << std::endl;
    return type;
  }

  bool TraverseTypeLoc(TypeLoc Loc) {
    std::map<clang::SourceLocation, notSingleDecl>::iterator it;
    if (localVarDecls.isNotSingleFlag && localVarDecls.varDeclsNum == 1) {
      localVarDecls.varDeclType = getType(start, Loc.getEndLoc());
      return RecursiveASTVisitor::TraverseTypeLoc(Loc);
    }
    for (it = globalVarDeclsMap.begin(); it != globalVarDeclsMap.end(); it++) {
      if (it->second.varDeclsNum == 1) {
        it->second.varDeclType = getType(it->first, Loc.getEndLoc());
        return RecursiveASTVisitor::TraverseTypeLoc(Loc);
      }
    }
    return true;
  }

  void ProcessLocalDeclaration(VarDecl *S, SourceRange toInsert) {
    ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
    SourceRange Range(S->getLocation());
    std::cout << "Range: " << Canvas.getRewrittenText(Range).str() << std::endl;
    localVarDecls.starts.push_front(S->getBeginLoc());
    localVarDecls.ends.push_front(S->getEndLoc());
    SourceRange varDeclRange(S->getBeginLoc(), S->getEndLoc());
    if (localVarDecls.varDeclsNum == 1) {
      localVarDecls.isFirstVar = true;
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
      std::cout << "first localVarDeclsNum = " << localVarDecls.varDeclsNum << " " << txtStr << std::endl;
    }
    if (localVarDecls.varDeclsNum > 1) {
      SourceRange prevVarDeclRange(localVarDecls.starts.back(), localVarDecls.ends.back());
      localVarDecls.starts.pop_back();
      localVarDecls.ends.pop_back();
      Canvas.ReplaceText(prevVarDeclRange, "");
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
      std::cout << "varDeclsNum = " << localVarDecls.varDeclsNum << " " << txtStr << std::endl;
      auto it = std::remove(txtStr.begin(), txtStr.end(), ',');
      txtStr.erase(it, txtStr.end());
      std::cout << "varDeclsNum = " << localVarDecls.varDeclsNum << " " << txtStr << std::endl;
    }
    localVarDecls.names.push_front(txtStr + ";\n");
  }

  void ProcessGlobalDeclaration(VarDecl *S, SourceRange toInsert) {
    ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
    SourceRange Range(S->getLocation());
    std::cout << "Range: " << Canvas.getRewrittenText(Range).str() << std::endl;
    globalVarDeclsMap[S->getBeginLoc()].starts.push_front(S->getBeginLoc());
    globalVarDeclsMap[S->getBeginLoc()].ends.push_front(S->getEndLoc());
    SourceRange varDeclRange(S->getBeginLoc(), S->getEndLoc());
    if (globalVarDeclsMap[S->getBeginLoc()].varDeclsNum == 1) {
      globalVarDeclsMap[S->getBeginLoc()].isFirstVar = true;
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
      std::cout << "first localVarDeclsNum = " << globalVarDeclsMap[S->getBeginLoc()].varDeclsNum << " " << txtStr << std::endl;

      globalVarDeclsMap[S->getBeginLoc()].notSingleDeclStart = S->getBeginLoc();
      globalVarDeclsMap[S->getBeginLoc()].names.push_front(txtStr + ";\n");
    }
    if (globalVarDeclsMap[S->getBeginLoc()].varDeclsNum > 1) {
      SourceRange prevVarDeclRange(globalVarDeclsMap[S->getBeginLoc()].starts.back(), globalVarDeclsMap[S->getBeginLoc()].ends.back());
      globalVarDeclsMap[S->getBeginLoc()].starts.pop_back();
      globalVarDeclsMap[S->getBeginLoc()].ends.pop_back();
      Canvas.ReplaceText(prevVarDeclRange, "");
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
      std::cout << "varDeclsNum = " << globalVarDeclsMap[S->getBeginLoc()].varDeclsNum << " " << txtStr << std::endl;
      auto it = std::remove(txtStr.begin(), txtStr.end(), ',');
      txtStr.erase(it, txtStr.end());
      std::cout << "varDeclsNum = " << globalVarDeclsMap[S->getBeginLoc()].varDeclsNum << " " << txtStr << std::endl;

      globalVarDeclsMap[S->getBeginLoc()].names.push_front(txtStr);
    }
    globalVarDeclsMap[S->getBeginLoc()].notSingleDeclEnd = S->getEndLoc();
  }

  bool VisitVarDecl(VarDecl *S) { // to traverse the parse tree and visit each statement
    if (mGlobalInfo.findOutermostDecl(S)) {
      if (globalVarDeclsMap[S->getBeginLoc()].varDeclsNum == 0) {
        std::map<clang::SourceLocation, notSingleDecl>::iterator it;
        for (it = globalVarDeclsMap.begin(); it != globalVarDeclsMap.end(); it++) {
          if (it->first != S->getBeginLoc()) {
            it->second.starts.clear();
            it->second.ends.clear();
            it->second.names.clear();
            globalVarDeclsMap.erase(it->first);
          }
        }
      }
      if (globalVarDeclsMap[S->getBeginLoc()].varDeclsNum == 1) {
        globalVarDeclsMap[S->getBeginLoc()].isNotSingleFlag = true;
      }
      globalVarDeclsMap[S->getBeginLoc()].varDeclsNum++;
      SourceRange toInsert(S->getBeginLoc(), S->getEndLoc());
      ProcessGlobalDeclaration(S, toInsert);
    }
    if (localVarDecls.isNotSingleFlag) {
      localVarDecls.varDeclsNum++;
      SourceRange toInsert(localVarDecls.notSingleDeclStart, localVarDecls.notSingleDeclEnd);
      ProcessLocalDeclaration(S, toInsert);
    }
    return true;
  }


  bool VisitDeclStmt(DeclStmt *S) {
    if(!(S->isSingleDecl())) {
      start = S->getBeginLoc();
      localVarDecls.varDeclsNum = 0;
      localVarDecls.starts.clear();
      localVarDecls.ends.clear();
      localVarDecls.names.clear();
      localVarDecls.isNotSingleFlag = true;
      std::cout << "IS NOT SINGLE\n";
      localVarDecls.notSingleDeclStart = S->getBeginLoc();
      localVarDecls.notSingleDeclEnd = S->getEndLoc();
      varPositions[S->getBeginLoc()] = S->getEndLoc();
    } else {
      std::cout << "IS SINGLE\n";
      localVarDecls.isNotSingleFlag = false;
    }
    return true;
  }

  bool VisitParmVarDecl(ParmVarDecl *S) {
    std::cout << "is ParmVarDecl" << std::endl;
    localVarDecls.isNotSingleFlag = false;
    return true;
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
  const GlobalInfoExtractor &mGlobalInfo;

  ClangGlobalInfoPass::RawInfo *mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  bool mActiveSplit = false;
  bool splitPragmaFlag = false;

  std::map<SourceLocation, notSingleDecl> globalVarDeclsMap;
  std::map<SourceLocation, SourceLocation> varPositions;
  notSingleDecl localVarDecls;
  std::string txtStr;
  SourceLocation start;
};
}

bool ClangSplitDeclsPass::runOnModule(llvm::Module &M) {
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  if (!TfmInfo) {
    M.getContext().emitError("cannot transform sources"
                             ": transformation context is not available");
    return false;
  }
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto &GIP{getAnalysis<ClangGlobalInfoPass>()};
  auto *CUs{M.getNamedMetadata("llvm.dbg.cu")};
  for (auto *MD : CUs->operands()) {
    auto *CU{cast<DICompileUnit>(MD)};
    auto *TfmCtx{
        dyn_cast_or_null<ClangTransformationContext>(TfmInfo->getContext(*CU))};
    if (!TfmCtx || !TfmCtx->hasInstance()) {
      M.getContext().emitError("cannot transform sources"
                               ": transformation context is not available");
      return false;
    }
    ASTImportInfo ImportStub;
    const auto *ImportInfo = &ImportStub;
    if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
      ImportInfo = &ImportPass->getImportInfo();
    auto &GIP = getAnalysis<ClangGlobalInfoPass>();
    //mGlobalInfo = &GIP.getGlobalInfo();
    const auto &GlobalInfo = GIP.getGlobalInfo();
    ClangSplitter Vis(*TfmCtx, *ImportInfo, GlobalInfo, GIP.getRawInfo());
    Vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
    return false;
  }
}