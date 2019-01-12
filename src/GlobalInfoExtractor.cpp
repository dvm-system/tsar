//===- GlobalInfoExtractor.cpp - AST Based Global Information ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements functionality to collect global information about the
// whole translation unit.
//
//===----------------------------------------------------------------------===//

#include "GlobalInfoExtractor.h"
#include "ClangUtils.h"
#include "SourceLocationTraverse.h"
#include "tsar_transformation.h"
#include <clang/Basic/SourceManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>

#ifdef LLVM_DEBUG
#  include <llvm/Support/raw_ostream.h>
#  include <clang/Lex/Lexer.h>
#endif

using namespace clang;
using namespace llvm;
using namespace tsar;

#define DEBUG_TYPE "clang-global-info"

char ClangGlobalInfoPass::ID = 0;
INITIALIZE_PASS_BEGIN(ClangGlobalInfoPass, "clang-global-info",
  "Source-level Globals Information (Clang)", false, false)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangGlobalInfoPass, "clang-global-info",
  "Source-level Globals Information (Clang)", false, false)

llvm::ModulePass * llvm::createClangGlobalInfoPass() {
  return new ClangGlobalInfoPass;
}

void ClangGlobalInfoPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

bool ClangGlobalInfoPass::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto &Context = TfmCtx->getContext();
  auto &Rewriter = TfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  mGIE = make_unique<GlobalInfoExtractor>(SrcMgr, LangOpts);
  mGIE->TraverseDecl(Context.getTranslationUnitDecl());
  for (auto *File : mGIE->getFiles()) {
    StringMap<SourceLocation> RawIncludes;
    const llvm::MemoryBuffer *Buffer =
      const_cast<SourceManager &>(SrcMgr).getMemoryBufferForFile(File);
    FileID FID = SrcMgr.translateFile(File);
    getRawMacrosAndIncludes(FID, Buffer, SrcMgr, LangOpts,
      mRawInfo.Macros, mRawInfo.Includes, mRawInfo.Identifiers);
  }
  return false;
}

bool GlobalInfoExtractor::VisitStmt(Stmt *S) {
  traverseSourceLocation(S, [this](SourceLocation Loc) { visitLoc(Loc); });
  return RecursiveASTVisitor::VisitStmt(S);
}

bool GlobalInfoExtractor::VisitTypeLoc(TypeLoc TL) {
  traverseSourceLocation(TL, [this](SourceLocation Loc) { visitLoc(Loc); });
  return RecursiveASTVisitor::VisitTypeLoc(TL);
}

bool GlobalInfoExtractor::TraverseDecl(Decl *D) {
  if (isa<TranslationUnitDecl>(D))
    return RecursiveASTVisitor::TraverseDecl(D);
  traverseSourceLocation(D, [this](SourceLocation Loc) { visitLoc(Loc); });
#ifdef LLVM_DEBUG
  auto log = [D, this]() {
    dbgs() << "[GLOBAL INFO]: global declaration with name "
      << cast<NamedDecl>(D)->getName() << " has outermost declaration at ";
    mOutermostDecl->getLocation().dump(mSM);
    dbgs() << "\n";
  };
#endif
  if (!mOutermostDecl) {
    mOutermostDecl = D;
    if (auto ND = dyn_cast<NamedDecl>(D)) {
      mOutermostDecls[ND->getName()].emplace_back(ND, mOutermostDecl);
      LLVM_DEBUG(log());
    }
    auto Res = RecursiveASTVisitor::TraverseDecl(D);
    mOutermostDecl = nullptr;
    return Res;
  }
  if (!mLangOpts.CPlusPlus && isa<TagDecl>(mOutermostDecl) &&
      (isa<TagDecl>(D) || isa<EnumConstantDecl>(D))) {
    auto ND = cast<NamedDecl>(D);
    mOutermostDecls[ND->getName()].emplace_back(ND, mOutermostDecl);
    LLVM_DEBUG(log());
  }
  return RecursiveASTVisitor::TraverseDecl(D);
}

void GlobalInfoExtractor::collectIncludes(FileID FID) {
  while (FID.isValid()) {
    // Warning, we use getFileEntryForID() to check that buffer is valid
    // because it seems that dereference of nullptr may occur in getBuffer()
    // (getFile().getContentCash() may return nullptr). It seems that there is
    // an error in getBuffer() method.
    if (auto Entry = mSM.getFileEntryForID(FID)) {
      auto Info = mFiles.insert(Entry);
      if (Info.second) {
        LLVM_DEBUG(dbgs() << "[GLOBAL INFO]: visited file " << Entry->getName() <<
          " with " << mSM.getNumCreatedFIDsForFileID(FID) <<" FIDs\n");
      }
    }
    auto IncLoc = mSM.getIncludeLoc(FID);
    if (IncLoc.isValid()) {
      bool F = mVisitedIncludeLocs.insert(IncLoc.getRawEncoding()).second;
      if (F) {
        LLVM_DEBUG(dbgs() << "[GLOBAL INFO]: visited #include location ";
          IncLoc.dump(mSM); dbgs() << "\n");
      }
    }
    FID = mSM.getFileID(mSM.getIncludeLoc(FID));
  }
}

 void GlobalInfoExtractor::visitLoc(SourceLocation Loc) {
  if (Loc.isInvalid())
    return;
  auto ExpLoc = mSM.getExpansionLoc(Loc);
  mVisitedExpLocs.insert(ExpLoc.getRawEncoding());
  auto FID = mSM.getFileID(ExpLoc);
  collectIncludes(FID);
  if (Loc.isFileID())
    return;
  SmallVector<SourceLocation, 8> LocationStack;
  while (Loc.isMacroID()) {
    // If this is the expansion of a macro argument, point the caret at the
    // use of the argument in the definition of the macro, not the expansion.
    if (mSM.isMacroArgExpansion(Loc)) {
      auto ArgInMacroLoc = mSM.getImmediateExpansionRange(Loc).getBegin();
      LocationStack.push_back(ArgInMacroLoc);
      // Remember file which contains macro definition.
      auto FID = mSM.getFileID(mSM.getSpellingLoc(ArgInMacroLoc));
      collectIncludes(FID);
    } else {
      LocationStack.push_back(Loc);
      // Remember file which contains macro definition.
      auto FID = mSM.getFileID(mSM.getSpellingLoc(Loc));
      collectIncludes(FID);
    }
    Loc = mSM.getImmediateMacroCallerLoc(Loc);
    // Once the location no longer points into a macro, try stepping through
    // the last found location.  This sometimes produces additional useful
    // backtraces.
    if (Loc.isFileID()) {
      Loc = mSM.getImmediateMacroCallerLoc(LocationStack.back());
    }
    assert(Loc.isValid() && "Must have a valid source location here!");
  }
  LLVM_DEBUG(
    dbgs() << "[GLOBAL INFO]: expanded macros:\n";
    for (auto Loc : LocationStack) {
      dbgs() << "  " << Lexer::getImmediateMacroNameForDiagnostics(
        Loc, mSM, mLangOpts) << " at";
      Loc.dump(mSM);
      dbgs() << "\n";
    });
}
