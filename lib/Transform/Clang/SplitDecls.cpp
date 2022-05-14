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
struct notSingleDecl {
  bool IsNotSingleFlag = false;
  int VarDeclsNum = 0;
  bool IsFirstVar = true;
  bool PointerFlag = false;
  SourceLocation NotSingleDeclStart;
  SourceLocation NotSingleDeclEnd;
  std::deque<SourceLocation> Starts;
  std::deque<SourceLocation> Ends;
  std::deque<std::string> Names;
  std::string VarDeclType;
};

class ClangSplitter : public RecursiveASTVisitor<ClangSplitter> {
public:
  ClangSplitter(ClangTransformationContext &TfmCtx,
                const ASTImportInfo &ImportInfo,
                const GlobalInfoExtractor &GlobalInfo,
                ClangGlobalInfo::RawInfo &RawInfo)
      : mTfmCtx(&TfmCtx), mImportInfo(ImportInfo), mGlobalInfo(GlobalInfo),
        mRawInfo(&RawInfo), mRewriter(TfmCtx.getRewriter()),
        mContext(TfmCtx.getContext()), mSrcMgr(mRewriter.getSourceMgr()),
        mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) { // to traverse the parse tree and visit each statement
    if (!S) {
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    Pragma P(*S);
    if (findClause(P, ClauseId::SplitDeclaration, mClauses)) {
      llvm::SmallVector<clang::CharSourceRange, 8> ToRemove;
      auto IsPossible = pragmaRangeToRemove(P, mClauses, mSrcMgr, mLangOpts,
                                            mImportInfo, ToRemove);
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
      for (it = mGlobalVarDeclsMap.begin(); it != mGlobalVarDeclsMap.end(); it++) {
        if (it->second.IsNotSingleFlag) {
          SourceRange toInsert(it->second.NotSingleDeclStart,
              it->second.NotSingleDeclEnd);
          ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
          mRewriter.RemoveText(toInsert, RemoveEmptyLine);
        }
      }
      if (mLocalVarDecls.IsNotSingleFlag) {
        SourceRange toInsert(mLocalVarDecls.NotSingleDeclStart,
            mLocalVarDecls.NotSingleDeclEnd);
        mRewriter.RemoveText(toInsert, RemoveEmptyLine);
      }
      return true;
    }
    Rewriter::RewriteOptions RemoveEmptyLine;
    RemoveEmptyLine.RemoveLineIfEmpty = false;
      if (mLocalVarDecls.IsNotSingleFlag) {
        SourceRange toInsert(mLocalVarDecls.NotSingleDeclStart,
            mLocalVarDecls.NotSingleDeclEnd);
        mRewriter.RemoveText(toInsert, RemoveEmptyLine);
        while (mLocalVarDecls.Names.size()) {
          if (mLocalVarDecls.IsFirstVar) {
            mRewriter.InsertTextAfterToken(mLocalVarDecls.NotSingleDeclEnd,
                mLocalVarDecls.Names.back());
            mLocalVarDecls.IsFirstVar = false;
          } else {
            mRewriter.InsertTextAfterToken(mLocalVarDecls.NotSingleDeclEnd,
                mLocalVarDecls.VarDeclType + mLocalVarDecls.Names.back());
          }
          mLocalVarDecls.Names.pop_back();
        }
      }

      std::map<clang::SourceLocation, notSingleDecl>::iterator it;
      for (it = mGlobalVarDeclsMap.begin(); it != mGlobalVarDeclsMap.end(); it++) {
      if (it->second.IsNotSingleFlag) {
        SourceRange toInsert(it->second.NotSingleDeclStart, it->second.Ends.back());
        mRewriter.RemoveText(toInsert, RemoveEmptyLine);
        while (it->second.Names.size()) {
          if (it->second.IsFirstVar) {
            mRewriter.InsertTextAfterToken(it->second.Ends.back(),
                it->second.Names.back());
            it->second.IsFirstVar = false;
          } else {
            if (it->second.Names.size() == 1) {
              mRewriter.InsertTextAfterToken(it->second.Ends.back(),
                  it->second.VarDeclType + it->second.Names.back());
            } else {
              mRewriter.InsertTextAfterToken(it->second.Ends.back(),
                  it->second.VarDeclType + it->second.Names.back() + ";\n");
            }
          }
          it->second.Names.pop_back();
        }
      }
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  std::string getType(SourceLocation beginLoc, SourceLocation endLoc) {
    SourceRange varDeclRange(beginLoc, endLoc);
    std::string type = mRewriter.getRewrittenText(varDeclRange);
    if (type.find("*") != -1) {
      mLocalVarDecls.PointerFlag = true;
      if (mGlobalVarDeclsMap.count(mCurrentSL)) {
        mGlobalVarDeclsMap[mCurrentSL].PointerFlag = true;
      }
    }
    return type;
  }

  void findPointerToConst(QualType qualType, SourceLocation beginLoc) {
    if (mLocalVarDecls.PointerFlag) {
      if (qualType.isConstQualified()) {
        mLocalVarDecls.IsNotSingleFlag = false;
        if (mTypeLocs.empty() || mTypeLocs.front() != beginLoc) {
          toDiag(mSrcMgr.getDiagnostics(), beginLoc,
              tsar::diag::warn_pointers_to_constants_split_prevent);
          mTypeLocs.push_front(beginLoc);
        }
      }
    }
    if (mGlobalVarDeclsMap.count(mCurrentSL) && mGlobalVarDeclsMap[mCurrentSL].PointerFlag) {
      if (qualType.isConstQualified()) {
        mGlobalVarDeclsMap[mCurrentSL].IsNotSingleFlag = false;
        if (mTypeLocs.empty() || mTypeLocs.front() != beginLoc) {
          toDiag(mSrcMgr.getDiagnostics(), beginLoc,
              tsar::diag::warn_pointers_to_constants_split_prevent);
          mTypeLocs.push_front(beginLoc);
        }
      }
    }
  }

  bool TraverseTypeLoc(TypeLoc Loc) {
    findPointerToConst(Loc.getType(), Loc.getBeginLoc());
    if (mLocalVarDecls.IsNotSingleFlag && mLocalVarDecls.VarDeclsNum == 1) {
      mLocalVarDecls.VarDeclType = getType(mLocalVarDecls.NotSingleDeclStart, Loc.getEndLoc());
      return RecursiveASTVisitor::TraverseTypeLoc(Loc);
    }
    if (mGlobalVarDeclsMap.count(mCurrentSL)) {
      if (mGlobalVarDeclsMap[mCurrentSL].VarDeclsNum == 1) {
        mGlobalVarDeclsMap[mCurrentSL].VarDeclType = getType(mCurrentSL,
            Loc.getEndLoc());
        return RecursiveASTVisitor::TraverseTypeLoc(Loc);
      }
    }
    return true;
  }

  void ProcessLocalDeclaration(VarDecl *S, SourceRange toInsert) {
    std::string txtStr;
    ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
    SourceRange Range(S->getLocation());
    mLocalVarDecls.Starts.push_front(S->getBeginLoc());
    mLocalVarDecls.Ends.push_front(S->getEndLoc());
    SourceRange varDeclRange(S->getBeginLoc(), S->getEndLoc());
    if (mLocalVarDecls.VarDeclsNum == 1) {
      mLocalVarDecls.IsFirstVar = true;
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
    }
    if (mLocalVarDecls.VarDeclsNum > 1) {
      SourceRange prevVarDeclRange(mLocalVarDecls.Starts.back(),
          mLocalVarDecls.Ends.back());
      mLocalVarDecls.Starts.pop_back();
      mLocalVarDecls.Ends.pop_back();
      Canvas.ReplaceText(prevVarDeclRange, "");
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
      auto it = std::remove(txtStr.begin(), txtStr.end(), ',');
      txtStr.erase(it, txtStr.end());
    }
    mLocalVarDecls.Names.push_front(txtStr + ";\n");
  }

  void ProcessGlobalDeclaration(VarDecl *S, SourceRange toInsert) {
    std::string txtStr;
    ExternalRewriter Canvas(toInsert, mSrcMgr, mLangOpts);
    SourceRange Range(S->getLocation());
    mGlobalVarDeclsMap[S->getBeginLoc()].Starts.push_front(S->getBeginLoc());
    mGlobalVarDeclsMap[S->getBeginLoc()].Ends.push_front(S->getEndLoc());
    SourceRange varDeclRange(S->getBeginLoc(), S->getEndLoc());
    if (mGlobalVarDeclsMap[S->getBeginLoc()].VarDeclsNum == 1) {
      mGlobalVarDeclsMap[S->getBeginLoc()].IsFirstVar = true;
      txtStr = Canvas.getRewrittenText(varDeclRange).str();
      mGlobalVarDeclsMap[S->getBeginLoc()].NotSingleDeclStart = S->getBeginLoc();
      mGlobalVarDeclsMap[S->getBeginLoc()].Names.push_front(txtStr + ";\n");
    }
    if (mGlobalVarDeclsMap[S->getBeginLoc()].VarDeclsNum > 1) {
      SourceRange prevVarDeclRange(mGlobalVarDeclsMap[S->getBeginLoc()].Starts.back(),
          mGlobalVarDeclsMap[S->getBeginLoc()].Ends.back());
      mGlobalVarDeclsMap[S->getBeginLoc()].Starts.pop_back();
      mGlobalVarDeclsMap[S->getBeginLoc()].Ends.pop_back();
      Canvas.ReplaceText(prevVarDeclRange, "");
      txtStr = Canvas.getRewrittenText(varDeclRange).str();

      auto it = std::remove(txtStr.begin(), txtStr.end(), ',');
      txtStr.erase(it, txtStr.end());

      mGlobalVarDeclsMap[S->getBeginLoc()].Names.push_front(txtStr);
    }
    mGlobalVarDeclsMap[S->getBeginLoc()].NotSingleDeclEnd = S->getEndLoc();
  }

  bool VisitVarDecl(VarDecl *S) { // to traverse the parse tree and visit each statement
    mCurrentSL = S->getBeginLoc();
    if (mGlobalInfo.findOutermostDecl(S)) {
      if (mGlobalVarDeclsMap[S->getBeginLoc()].VarDeclsNum == 0) {
        std::map<clang::SourceLocation, notSingleDecl>::iterator it;
        for (it = mGlobalVarDeclsMap.begin(); it != mGlobalVarDeclsMap.end(); it++) {
          if (it->first != S->getBeginLoc()) {
            // it->second.Starts.clear();
            // it->second.Ends.clear();
            // it->second.Names.clear();
            // mGlobalVarDeclsMap.erase(it->first);
          }
        }
      }
      mGlobalVarDeclsMap[S->getBeginLoc()].VarDeclsNum++;
      if (mGlobalVarDeclsMap[S->getBeginLoc()].VarDeclsNum == 2) {
        mGlobalVarDeclsMap[S->getBeginLoc()].IsNotSingleFlag = true;
      }
      SourceRange toInsert(S->getBeginLoc(), S->getEndLoc());
      ProcessGlobalDeclaration(S, toInsert);
    }
    if (mLocalVarDecls.IsNotSingleFlag) {
      mLocalVarDecls.VarDeclsNum++;
      SourceRange toInsert(mLocalVarDecls.NotSingleDeclStart,
          mLocalVarDecls.NotSingleDeclEnd);
      ProcessLocalDeclaration(S, toInsert);
    }
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S) {
    if(!(S->isSingleDecl())) {
      mLocalVarDecls.VarDeclsNum = 0;
      mLocalVarDecls.Starts.clear();
      mLocalVarDecls.Ends.clear();
      mLocalVarDecls.Names.clear();
      mLocalVarDecls.IsNotSingleFlag = true;
      mLocalVarDecls.NotSingleDeclStart = S->getBeginLoc();
      mLocalVarDecls.NotSingleDeclEnd = S->getEndLoc();
    } else {
      mLocalVarDecls.IsNotSingleFlag = false;
    }
    return true;
  }

  bool VisitParmVarDecl(ParmVarDecl *S) {
    if (mLocalVarDecls.IsNotSingleFlag) {
      toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
          tsar::diag::warn_parm_var_decl_split_prevent);
      mLocalVarDecls.IsNotSingleFlag = false;
    }
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
  ClangTransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  const GlobalInfoExtractor &mGlobalInfo;
  ClangGlobalInfo::RawInfo *mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  std::map<SourceLocation, notSingleDecl> mGlobalVarDeclsMap;
  notSingleDecl mLocalVarDecls;
  SourceLocation mCurrentSL;
  std::deque<SourceLocation> mTypeLocs;
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
    ClangSplitter Vis(*TfmCtx, *ImportInfo, GIP.getGlobalInfo(TfmCtx)->GIE,
        GIP.getGlobalInfo(TfmCtx)->RI);
    Vis.TraverseDecl(TfmCtx->getContext().getTranslationUnitDecl());
  }
  return false;
}