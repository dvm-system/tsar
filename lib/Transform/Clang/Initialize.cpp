//=== Initialize.cpp (Clang) --*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file implements a pass to initialize arrays and variables.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <stack>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-rfp"

static int getDimensionsNum(QualType qt, std::vector<int>& default_dimensions) {
  int res = 0;
  bool sizeIsKnown = true;
  while(1) {
    if (qt -> isArrayType()) {
      auto at = qt -> getAsArrayTypeUnsafe();
      auto t =  dyn_cast_or_null<ConstantArrayType>(at);
      if (sizeIsKnown && t) { // get size
        uint64_t dim = t -> getSize().getLimitedValue();
        default_dimensions.push_back(dim);
      }
      qt = at -> getElementType();
      res++;
    } else if (qt -> isPointerType()) {
        sizeIsKnown = false;
        qt = qt -> getPointeeType();
        res++;
    } else {
        return res;
    }
  }
}

namespace {

class ClangInitialize : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangInitialize() : FunctionPass(ID) {
    initializeClangInitializePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

struct vars {                   // contains information about variables in
  bool rvalIsArray = false;     // initialize clause
  std::string lvalName;
  std::string rvalName;
  int dimensionsNum;
  std::vector<int> dimensions;
  std::vector<int> default_dimensions;
};
}

char ClangInitialize::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangInitialize, "initialize",
  "Initialize variables in for", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangInitialize, "initialize",
  "Initialize variables in for", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {

class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {

  struct DeclarationInfo {
    DeclarationInfo(Stmt *S) : Scope(S) {}

    Stmt *Scope;
  };
public:
  explicit DeclVisitor(ClangTransformationContext &TfmCtx,
                       const ASTImportInfo &ImportInfo)
      : mTfmCtx(&TfmCtx), mImportInfo(ImportInfo),
        mRewriter(TfmCtx.getRewriter()), mContext(TfmCtx.getContext()),
        mSrcMgr(mRewriter.getSourceMgr()), mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;

    bool ast = false;
    Pragma P(*S);

    if (findClause(P, ClauseId::Initialize, mClauses)) {
      auto locationForInits = S -> getEndLoc();
      isInPragma = true;
      ast = RecursiveASTVisitor::TraverseStmt(S);
      isInPragma = false;

      std::string txtStr, beforeFor, forBody, lval, rval, indeces;
      std::vector<std::string> inits;
      while (varStack.size()) {
        if (varStack.top().dimensionsNum) {   // lvalue is array
          if (varStack.top().dimensions.size() < varStack.top().dimensionsNum) {
            if (varStack.top().default_dimensions.size() == varStack.top().dimensionsNum) {
              varStack.top().dimensions = varStack.top().default_dimensions;
            } else {
              varStack.pop();
              continue;         // dimensions ar mandatory for arrays, skip
            }                   // initialization if no dimensions found
          }
          forBody = std::string();
          indeces = std::string();
          lval = varStack.top().lvalName;
          rval = varStack.top().rvalName;
          txtStr = std::string();
          for (std::vector<int>::iterator it = varStack.top().dimensions.begin();
             it != varStack.top().dimensions.end();
             it ++) {
            int intCounter = it - varStack.top().dimensions.begin();
            std::string strCounter = "i" + std::to_string(intCounter);
            indeces += "[" + strCounter + "]";
            txtStr += "for (int " + strCounter + " = 0; " + strCounter + " < " +
                      std::to_string(*it) + "; " + strCounter + "++) {\n";
          }
          if (varStack.top().rvalIsArray) {
            rval += indeces;
          }
          lval += indeces;
          forBody = lval + " = " + rval + ";\n";
          txtStr += forBody;
          for (int i = 0; i < varStack.top().dimensionsNum; i++) {
            txtStr += "}\n";
          }
        } else {  // Initialize non-array variable
          txtStr = varStack.top().lvalName + " = " + varStack.top().rvalName + ";\n";
        }
        inits.push_back(txtStr);
        varStack.pop();
      }

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

      for (std::vector<std::string>::iterator it = inits.begin(); it != inits.end(); ++it) {
        mRewriter.InsertTextAfterToken(locationForInits, *it);
      }
      return ast;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseDeclRefExpr(clang::DeclRefExpr *Ex) {
    std::string varName;
    if (isInPragma) {
      if (waitingForDimensions && curDimensionNum == varStack.top().dimensionsNum) {
        waitingForDimensions = false;
        curDimensionNum = 0;
      }
      if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())}) {
        varName = Var -> getName();
      }
      if (waitingForVar) {  // get lvalue
        ValueDecl *vd = Ex -> getDecl();
        QualType qt = vd -> getType();
        vars tmp;

        tmp.lvalName = varName;
        varStack.push(tmp);
        varStack.top().dimensionsNum = getDimensionsNum(qt, varStack.top().default_dimensions);
        waitingForDimensions = false;
      } else {              // get rvalue
        ValueDecl *vd = Ex -> getDecl();
        QualType qt = vd -> getType();
        if (qt -> isArrayType() || qt -> isPointerType()) {
          varStack.top().rvalIsArray = true;
        }
        varStack.top().rvalName = varName;
        if (varStack.top().dimensionsNum > 0) {
          waitingForDimensions = true;
        }

      }
      waitingForVar = !waitingForVar;
    }
    return RecursiveASTVisitor::TraverseDeclRefExpr(Ex);
  }

  bool TraverseIntegerLiteral(IntegerLiteral *IL) {

    if (isInPragma) {
      if (waitingForDimensions && curDimensionNum == varStack.top().dimensionsNum) {
        waitingForDimensions = false;
        curDimensionNum = 0;
      }
      int val = IL -> getValue().getLimitedValue();
      if (waitingForDimensions) {
        if (varStack.size()) {
          varStack.top().dimensions.push_back(val);
          curDimensionNum++;
        }
      } else if (!waitingForVar) {    // get rvalue
        varStack.top().rvalName = std::to_string(val);
        waitingForVar = !waitingForVar;
        if (varStack.top().dimensionsNum > 0) {
          waitingForDimensions = true;
        }
      }
    }
    return RecursiveASTVisitor::TraverseIntegerLiteral(IL);
  }

private:
  /// Return current scope.
  Stmt *getScope() {
    for (auto I = mScopes.rbegin(), EI = mScopes.rend(); I != EI; ++I)
      if (isa<ForStmt>(*I) || isa<CompoundStmt>(*I))
        return *I;
    return nullptr;
  }

  bool isInPragma = false;
  bool waitingForVar = true;
  bool waitingForDimensions = false;
  int curDimensionNum = 0;
  std::vector<Stmt*> mScopes;
  ClangTransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  std::stack<vars> varStack;
};
}

bool ClangInitialize::runOnFunction(Function &F) {
  auto *M = F.getParent();
  auto *DISub{findMetadata(&F)};
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (isC(CU->getSourceLanguage()) && isCXX(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<ClangTransformationContext>(
                             TfmInfo->getContext(*CU))
                       : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    F.getContext().emitError(
        "cannot transform sources"
        ": transformation context is not available for the '" +
        F.getName() + "' function");
    return false;
  }
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  ASTImportInfo ImportStub;
  const auto *ImportInfo = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    ImportInfo = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto *GI{GIP.getGlobalInfo(TfmCtx)};
  assert(GI && "Global declaration must be collected!");
  DeclVisitor Visitor(*TfmCtx, *ImportInfo);

  Visitor.TraverseDecl(FuncDecl);
  return false;
}

void ClangInitialize::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangInitialize() {
  return new ClangInitialize();
}
