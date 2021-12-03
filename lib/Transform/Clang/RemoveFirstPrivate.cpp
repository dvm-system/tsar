//=== RemoveFirstPrivate.cpp - RFP (Clang) --*- C++ -*===//
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
// This file implements a pass to initialize firstprivate variables.
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

static bool isNameOfArray(std::string type) {
  if (type.find('[') != std::string::npos || type.find('*') != std::string::npos) {
    return true;
  }
  return false;
}

static void replaceSqrBrWithAsterisk(std::string &str) {
  size_t openingSqrBracket = str.find("[");
  size_t closingSqrBracket = str.find("]");
  str.erase(openingSqrBracket, closingSqrBracket - openingSqrBracket + 1);
  str += "*";
}

namespace {

class ClangRemoveFirstPrivate : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangRemoveFirstPrivate() : FunctionPass(ID) {
    initializeClangRemoveFirstPrivatePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

struct vars {           // contains information about variables in
  std::string var1Type; // removefirstprivate clause
  std::string var2Type;
  std::string var1Name;
  std::string var2Name;
  std::string count = "";
};
}

char ClangRemoveFirstPrivate::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangRemoveFirstPrivate, "remove-firstprivate",
  "Initialize variables in for", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_IN_GROUP_END(ClangRemoveFirstPrivate, "remove-firstprivate",
  "Initialize variables in for", false, false,
  TransformationQueryManager::getPassRegistry())

namespace {

class DeclVisitor : public RecursiveASTVisitor<DeclVisitor> {

  struct DeclarationInfo {
    DeclarationInfo(Stmt *S) : Scope(S) {}

    Stmt *Scope;
  };
public:
  explicit DeclVisitor(TransformationContext &TfmCtx, const ASTImportInfo &ImportInfo,
      ClangGlobalInfoPass::RawInfo &RawInfo) :
    mTfmCtx(&TfmCtx), mImportInfo(ImportInfo),
    mRawInfo(&RawInfo), mRewriter(TfmCtx.getRewriter()),
    mContext(TfmCtx.getContext()), mSrcMgr(mRewriter.getSourceMgr()),
    mLangOpts(mRewriter.getLangOpts()) {}

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;

    bool ast = false;
    Pragma P(*S);

    if (findClause(P, ClauseId::RemoveFirstPrivate, mClauses)) {

      auto locationForInits = S -> getEndLoc();

      isInPragma = true;
      ast = RecursiveASTVisitor::TraverseStmt(S);
      isInPragma = false;

      std::string txtStr, beforeFor, forBody, type1, type2;
      std::vector<std::string> inits;
      while (varStack.size()) {
        if (isNameOfArray(varStack.top().var1Type)) {
          if (varStack.top().count.empty()) {
            varStack.pop();
            continue; // count is mandatory for arrays, skip initialization if no count found
          }
          if (type1.find('[') != std::string::npos) {
            replaceSqrBrWithAsterisk(type1);
          }
          if (type2.find('[') != std::string::npos) {
            replaceSqrBrWithAsterisk(type2);
          }
          if (isNameOfArray(varStack.top().var2Type)) {   // arr1 = arr2
            forBody = varStack.top().var1Name + "[i] = " + varStack.top().var2Name + "[i];\n";
          } else {                                        // arr1 = val
            forBody = varStack.top().var1Name + "[i] = " + varStack.top().var2Name + ";\n";
          }
          txtStr = "for (int i = 0; i < " + varStack.top().count + "; i++) {\n" + forBody + "\n}\n";
        } else {  // Initialize non-array variable
          txtStr = varStack.top().var1Name + " = " + varStack.top().var2Name + ";\n";
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
      if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())}) {
        varName = Var -> getName();
      }
      if (waitingForVar) {
        ValueDecl *vd = Ex -> getDecl();
        QualType qt = vd -> getType();
        std::string typeStr = qt.getCanonicalType().getAsString();

        vars tmp;
        tmp.var1Type = typeStr;
        tmp.var1Name = varName;
        varStack.push(tmp);

      } else {

        ValueDecl *vd = Ex -> getDecl();
        QualType qt = vd -> getType();
        std::string typeStr = qt.getCanonicalType().getAsString();
        varStack.top().var2Type = typeStr;
        varStack.top().var2Name = varName;

      }
      waitingForVar = !waitingForVar;
    }
    return RecursiveASTVisitor::TraverseDeclRefExpr(Ex);
  }

  bool TraverseIntegerLiteral(IntegerLiteral *IL) {
    if (isInPragma && waitingForVar) {
      if (varStack.size()) {
        varStack.top().count = std::to_string(IL -> getValue().getLimitedValue());
      }
    }
    return RecursiveASTVisitor::TraverseIntegerLiteral(IL);
  }

#ifdef LLVM_DEBUG
// debug info
#endif

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
  std::vector<Stmt*> mScopes;

  TransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  ClangGlobalInfoPass::RawInfo *mRawInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;

  std::stack<vars> varStack;

};
}

bool ClangRemoveFirstPrivate::runOnFunction(Function &F) {
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

  DeclVisitor Visitor(*TfmCtx, *ImportInfo, GIP.getRawInfo());
  Visitor.TraverseDecl(FuncDecl);
  return false;
}

void ClangRemoveFirstPrivate::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangRemoveFirstPrivate() {
  return new ClangRemoveFirstPrivate();
}
