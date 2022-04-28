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
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-init"

static int getDimensionsNum(QualType QT, std::vector<int> &DefaultDimensions,
                            bool &ArrSizeIsKnown) {
  int Res = 0;
  bool SizeIsKnown = true;
  while (1) {
    if (QT->isArrayType()) {
      auto AT = QT->getAsArrayTypeUnsafe();
      auto T = dyn_cast_or_null<ConstantArrayType>(AT);
      if (SizeIsKnown && T) { // get size
        uint64_t Dim = T->getSize().getLimitedValue();
        DefaultDimensions.push_back(Dim);
      }
      QT = AT->getElementType();
      Res++;
    } else if (QT->isPointerType()) {
      SizeIsKnown = false;
      QT = QT->getPointeeType();
      Res++;
    } else {
      ArrSizeIsKnown = SizeIsKnown;
      return Res;
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

// contains information about variables in
// initialize clause
struct Vars {
  bool ErrorFlag = false;
  bool RvalIsArray = false;
  llvm::SmallString<64> LvalName;
  llvm::SmallString<64> RvalName;
  bool RSizeIsKnown;
  bool LSizeIsKnown;
  int LDimensionsNum = 0;
  int RDimensionsNum = 0;
  std::vector<int> Dimensions;
  std::vector<int> LDefaultDimensions;
  std::vector<int> RDefaultDimensions;
};
} // namespace

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
    Pragma P(*S);

    if (findClause(P, ClauseId::Initialize, mClauses)) {
      auto locationForInits = S->getEndLoc();
      mIsInPragma = true;
      bool Ast = RecursiveASTVisitor::TraverseStmt(S);
      mIsInPragma = false;
      if (mErrorFlag) {
        return true;
      }
      std::vector<std::string> Inits;
      for(std::vector<struct Vars>::iterator It = mVarVector.begin(); It != mVarVector.end(); ++It) {
        int LDimensionsNum = It->LDimensionsNum;
        int RDimensionsNum = It->RDimensionsNum;
        llvm::SmallString<128> BeforeFor, ForBody, Lval, Rval, Indeces;
        std::string TxtStr;
        if (LDimensionsNum < RDimensionsNum ||
            It->RvalIsArray && RDimensionsNum < LDimensionsNum) {
          toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
                 tsar::diag::warn_dimensions_do_not_match);
          continue;
        }
        if (LDimensionsNum) { // lvalue is array
          if (It->Dimensions.size() < LDimensionsNum) {
            if ((It->LDefaultDimensions.size() == LDimensionsNum) &&
                (!RDimensionsNum || It->LDefaultDimensions ==
                                        It->RDefaultDimensions)) {
              It->Dimensions = It->LDefaultDimensions;
            } else {
              toDiag(mSrcMgr.getDiagnostics(), S->getBeginLoc(),
                     tsar::diag::warn_unknown_dimensions);
              continue; // dimensions ar mandatory for arrays, skip
            }           // initialization if no dimensions found
          }
          Lval = It->LvalName;
          Rval = It->RvalName;
          for (auto it{It->Dimensions.begin()},
               EI{It->Dimensions.end()};
               it != EI; ++it) {
            int IntCounter = it - It->Dimensions.begin();
            std::string strCounter = "i" + std::to_string(IntCounter);
            Indeces += "[" + strCounter + "]";
            TxtStr += "for (int " + strCounter + " = 0; " + strCounter + " < " +
                      std::to_string(*it) + "; " + strCounter + "++) {\n";
          }
          if (It->RvalIsArray)
            Rval += Indeces;
          Lval += Indeces;
          (Lval + " = " + Rval + ";\n").toStringRef(ForBody);
          TxtStr += ForBody;
          for (int i = 0; i < LDimensionsNum; i++) {
            TxtStr += "}\n";
          }
        } else // Initialize non-array variable
          TxtStr = (It->LvalName + " = " +
                    It->RvalName + ";\n")
                       .str();
        Inits.push_back(TxtStr);
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
      for (std::vector<std::string>::iterator It = Inits.begin();
           It != Inits.end(); ++It) {
        mRewriter.InsertTextAfterToken(locationForInits, *It);
      }
      return Ast;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseDeclRefExpr(clang::DeclRefExpr *Ex) {
    llvm::StringRef VarName;
    if (mIsInPragma) {
      NamedDecl *test = Ex->getFoundDecl();
      auto type = Ex->getDecl()->getType();
      if (type->isFunctionType() || type->isFunctionPointerType() ||
          type->isStructureOrClassType()) {
        toDiag(mSrcMgr.getDiagnostics(), Ex->getBeginLoc(),
               tsar::diag::error_not_var);
        mErrorFlag = true;
      }
      if (mWaitingForDimensions &&
          mCurDimensionNum == mVarVector.back().LDimensionsNum) {
        mWaitingForDimensions = false;
        mCurDimensionNum = 0;
      }
      if (auto *Var{dyn_cast<VarDecl>(Ex->getDecl())})
        VarName = Var->getName();
      ValueDecl *VD = Ex->getDecl();
      QualType QT = VD->getType();
      if (mWaitingForVar) { // get lvalue
        Vars Tmp;
        Tmp.LvalName = VarName;
        mVarVector.push_back(std::move(Tmp));
        mVarVector.back().LDimensionsNum =
            getDimensionsNum(QT, mVarVector.back().LDefaultDimensions,
                             mVarVector.back().LSizeIsKnown);
        mWaitingForDimensions = false;
      } else { // get rvalue
        mVarVector.back().RDimensionsNum =
            getDimensionsNum(QT, mVarVector.back().RDefaultDimensions,
                             mVarVector.back().RSizeIsKnown);
        if (QT->isArrayType() || QT->isPointerType())
          mVarVector.back().RvalIsArray = true;
        mVarVector.back().RvalName = VarName;
        if (mVarVector.back().LDimensionsNum > 0)
          mWaitingForDimensions = true;
      }
      mWaitingForVar = !mWaitingForVar;
    }
    return RecursiveASTVisitor::TraverseDeclRefExpr(Ex);
  }

  bool TraverseIntegerLiteral(IntegerLiteral *IL) {

    if (mIsInPragma) {
      if (mWaitingForDimensions && (mVarVector.back().Dimensions.size() >=
                                        mVarVector.back().LDimensionsNum ||
                                    mVarVector.back().Dimensions.size() >=
                                        mVarVector.back().LDimensionsNum)) {
        toDiag(mSrcMgr.getDiagnostics(), IL->getBeginLoc(),
               tsar::diag::warn_too_many_dimensions);
      }
      if (mWaitingForDimensions &&
          mCurDimensionNum == mVarVector.back().LDimensionsNum) {
        mWaitingForDimensions = false;
        mCurDimensionNum = 0;
      }
      auto Val = IL->getValue();
      if (mWaitingForDimensions) {
        if (mVarVector.size()) {
          mVarVector.back().Dimensions.push_back(Val.getLimitedValue());
          mCurDimensionNum++;
        }
      } else if (!mWaitingForVar) { // get rvalue
        Val.toStringUnsigned(mVarVector.back().RvalName);
        mWaitingForVar = !mWaitingForVar;
        if (mVarVector.back().LDimensionsNum > 0)
          mWaitingForDimensions = true;
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

  bool mErrorFlag = false;
  bool mIsInPragma = false;
  bool mWaitingForVar = true;
  bool mWaitingForDimensions = false;
  int mCurDimensionNum = 0;
  std::vector<Stmt *> mScopes;
  ClangTransformationContext *mTfmCtx;
  const ASTImportInfo &mImportInfo;
  Rewriter &mRewriter;
  ASTContext &mContext;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  SmallVector<Stmt *, 1> mClauses;
  std::vector<Vars> mVarVector;
};
} // namespace

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

FunctionPass *llvm::createClangInitialize() { return new ClangInitialize(); }
