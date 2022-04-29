//===- IncludeTree.cpp --- Hierarchy of Include Files -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements passes to build and visualize hierarchy of include
// files in the analyzed project.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/IncludeTree.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <llvm/Analysis/DOTGraphTraitsPass.h>
#include "tsar/Core/Query.h"
#include <clang/Basic/FileManager.h>
#include <llvm/Support/GraphWriter.h>
#include <llvm/Support/DOTGraphTraits.h>
#include <llvm/Support/Path.h>
#include <ctime>

using namespace clang;
using namespace llvm;
using namespace tsar;

#define DEBUG_TYPE "clang-include-tree";

FileNode::FileNode(clang::FileID ID, ClangTransformationContext &TfmCtx)
    : mTfmCtx(&TfmCtx), mFile(ID),
      mIncludeLoc(TfmCtx.getContext().getSourceManager().getIncludeLoc(ID)) {}

void FileNode::sort() {
  auto &SrcMgr{mTfmCtx->getContext().getSourceManager()};
  llvm::sort(
    mChildren.begin(), mChildren.end(),
    [this, &SrcMgr](const EdgeT &LHS, const EdgeT &RHS) {
      auto L = SrcMgr.getDecomposedLoc(SrcMgr.getExpansionLoc(LHS.second));
      auto R = SrcMgr.getDecomposedLoc(SrcMgr.getExpansionLoc(RHS.second));
      assert(L.first == R.first && L.first == mFile &&
        "Declaration or include does not located in this file!");
      return L.second < R.second;
    });
}

bool FileTree::reconstruct(TransformationContextBase &TfmCtx,
                           const GlobalInfoExtractor &GIE) {
  auto *TfmCtxImpl{dyn_cast<ClangTransformationContext>(&TfmCtx)};
  if (!TfmCtxImpl)
    return false;
  auto &SrcMgr{TfmCtxImpl->getContext().getSourceManager()};
  for (auto &OuterDeclList : GIE.getOutermostDecls())
    for (auto &OuterDecl : OuterDeclList.second) {
      auto Loc = SrcMgr.getDecomposedLoc(
        SrcMgr.getExpansionLoc(OuterDecl.getDescendant()->getLocation()));
      if (Loc.first.isInvalid()) {
        mRoots.emplace_back(&OuterDecl);
        continue;
      }
      auto FileInfo = insert(Loc.first, *TfmCtxImpl);
      assert(FileInfo.first != file_end() && "FileNode must not be null!");
      FileInfo.first->push_back(&OuterDecl);
      mDeclToFile.try_emplace(&OuterDecl, &*FileInfo.first);
      Loc = SrcMgr.getDecomposedIncludedLoc(Loc.first);
      // Do not use FileInfo.first iterator after insert() inside the loop
      // because insert() may invalidate iterators.
      auto *FN = &*FileInfo.first;
      while (Loc.first.isValid() && FileInfo.second) {
        FileInfo = insert(Loc.first, *TfmCtxImpl);
        assert(FileInfo.first != file_end() && "FileNode must not be null!");
        FileInfo.first->push_back(FN);
        FN = &*FileInfo.first;
        Loc = SrcMgr.getDecomposedIncludedLoc(Loc.first);
      }
    }
  return true;
}

namespace llvm {
template <> struct DOTGraphTraits<FileTree *> : public DefaultDOTGraphTraits {
  using EdgeItr =
      typename GraphTraits<tsar::FileNode::ChildT *>::ChildIteratorType;

  explicit DOTGraphTraits(bool IsSimple = false)
      : DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const tsar::FileTree *) {
    return "Source Files and Outermost Declarations";
  }

  std::string getNodeLabel(tsar::FileNode::ChildT *N, tsar::FileTree *G) {
    std::string Str;
    raw_string_ostream OS(Str);
    if (FileNode::isFile(*N)) {
      auto &FN = FileNode::asFile(*N);
      auto &SrcMgr{
          FN.getTransformationContext().getContext().getSourceManager()};
      auto *FEntry = SrcMgr.getFileEntryForID(FN.getFile());
      assert(FEntry && "FileEntry must not be null!");
      auto Path = FEntry->getName();
      if (isSimple())
        OS << llvm::sys::path::filename(Path);
      else
        OS << Path;
      auto MT = FEntry->getModificationTime();
      char TimeBuf[100];
      if (std::strftime(TimeBuf, sizeof(TimeBuf), "%c %z", std::localtime(&MT)))
        OS << "\nModified on " << TimeBuf;
      if (FN.isInclude()) {
        auto PLoc = SrcMgr.getPresumedLoc(SrcMgr.getIncludeLoc(FN.getFile()));
        OS << "\nIncluded at " << PLoc.getLine() << ":" << PLoc.getColumn();
      }
    } else {
      auto &DN = FileNode::asDecl(*N);
      auto &SrcMgr{DN.getDescendant()->getASTContext().getSourceManager()};
      OS << DN.getDescendant()->getName();
      auto Loc = DN.getDescendant()->getLocation();
      if (Loc.isValid()) {
        auto PLoc = SrcMgr.getPresumedLoc(SrcMgr.getExpansionLoc(Loc));
        OS << "\nDeclared at " << PLoc.getLine() << ":" << PLoc.getColumn();
      }
    }
    return OS.str();
  }
};

template <>
struct DOTGraphTraits<
    bcl::convertible_pair<tsar::FileNode::ChildT *, FileTree *>>
    : public DOTGraphTraits<FileTree *> {
  explicit DOTGraphTraits(bool IsSimple = false)
      : DOTGraphTraits<FileTree *>(IsSimple) {}
};
}

void tsar::FileTree::view(const FileNode &FN, const llvm::Twine &Name) const {
  auto Root = FileNode::ChildT(const_cast<FileNode *>(&FN));
  bcl::convertible_pair<tsar::FileNode::ChildT *, FileTree *> Graph(
      &Root, const_cast<FileTree *>(this));
  llvm::ViewGraph(Graph, Name, false,
    llvm::DOTGraphTraits<decltype(Graph)>::getGraphName(this));
}

std::string tsar::FileTree::write(const FileNode &FN,
                                  const llvm::Twine &Name) const {
  auto Root = FileNode::ChildT(const_cast<FileNode *>(&FN));
  bcl::convertible_pair<tsar::FileNode::ChildT *, FileTree *> Graph(
      &Root, const_cast<FileTree *>(this));
  return llvm::WriteGraph(Graph, Name, false,
    llvm::DOTGraphTraits<decltype(Graph)>::getGraphName(this));
}

char ClangIncludeTreePass::ID = 0;

INITIALIZE_PASS_BEGIN(ClangIncludeTreePass, "clang-include-tree",
  "Source File Hierarchy (Clang)", true, true)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_END(ClangIncludeTreePass, "clang-include-tree",
  "Source File Hierarchy (Clang)", true, true)

ModulePass *createClangIncludeTreePass() { return new ClangIncludeTreePass; }

void ClangIncludeTreePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

bool ClangIncludeTreePass::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  if (!mFileTree)
    mFileTree = std::make_unique<FileTree>();
  if (!mFileTree->reconstruct(GIP.getGlobalInfo().begin(),
                              GIP.getGlobalInfo().end()))
    M.getContext().emitError("cannot analyze sources"
                             ": construction of an include tree is only "
                             "implemented for can C/C++ sources");
  return false;
}

namespace {
struct ClangIncludeTreePassGraphTraits {
  static tsar::FileTree *getGraph(ClangIncludeTreePass *P) {
    return &P->getFileTree();
  }
};

struct ClangIncludeTreePrinter : public DOTGraphTraitsModulePrinterWrapperPass<
    ClangIncludeTreePass, false, tsar::FileTree *,
    ClangIncludeTreePassGraphTraits> {
  static char ID;
  ClangIncludeTreePrinter() : DOTGraphTraitsModulePrinterWrapperPass<
      ClangIncludeTreePass, false, tsar::FileTree *,
      ClangIncludeTreePassGraphTraits>("files", ID) {
    initializeClangIncludeTreePrinterPass(*PassRegistry::getPassRegistry());
  }
};
char ClangIncludeTreePrinter::ID = 0;

struct ClangIncludeTreeOnlyPrinter : public DOTGraphTraitsModulePrinterWrapperPass<
    ClangIncludeTreePass, true, tsar::FileTree *,
    ClangIncludeTreePassGraphTraits> {
  static char ID;
  ClangIncludeTreeOnlyPrinter() : DOTGraphTraitsModulePrinterWrapperPass<
      ClangIncludeTreePass, true, tsar::FileTree *,
      ClangIncludeTreePassGraphTraits>("files-only", ID) {
    initializeClangIncludeTreeOnlyPrinterPass(*PassRegistry::getPassRegistry());
  }
};
char ClangIncludeTreeOnlyPrinter::ID = 0;

struct ClangIncludeTreeViewer : public DOTGraphTraitsModuleViewerWrapperPass<
    ClangIncludeTreePass, false, tsar::FileTree *,
    ClangIncludeTreePassGraphTraits> {
  static char ID;
  ClangIncludeTreeViewer() : DOTGraphTraitsModuleViewerWrapperPass<
      ClangIncludeTreePass, false, tsar::FileTree *,
      ClangIncludeTreePassGraphTraits>("files", ID) {
    initializeClangIncludeTreeViewerPass(*PassRegistry::getPassRegistry());
  }
};
char ClangIncludeTreeViewer::ID = 0;

struct ClangIncludeTreeOnlyViewer : public DOTGraphTraitsModuleViewerWrapperPass<
    ClangIncludeTreePass, true, tsar::FileTree *,
    ClangIncludeTreePassGraphTraits> {
  static char ID;
  ClangIncludeTreeOnlyViewer() : DOTGraphTraitsModuleViewerWrapperPass<
      ClangIncludeTreePass, true, tsar::FileTree *,
      ClangIncludeTreePassGraphTraits>("files-only", ID) {
    initializeClangIncludeTreeOnlyViewerPass(*PassRegistry::getPassRegistry());
  }
};
char ClangIncludeTreeOnlyViewer::ID = 0;
}

INITIALIZE_PASS_IN_GROUP(ClangIncludeTreeViewer, "clang-view-files",
  "View Source File Tree (Clang)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(ClangIncludeTreeOnlyViewer, "clang-view-files-only",
  "View Source File Tree (Filenames, Clang)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(ClangIncludeTreePrinter, "clang-print-files",
  "Print Source File Tree (Clang)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(ClangIncludeTreeOnlyPrinter, "clang-print-files-only",
  "Print Source File Tree (Filenames, Clang)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

ModulePass *llvm::createClangIncludeTreePrinter() {
  return new ClangIncludeTreePrinter;
}

ModulePass *llvm::createClangIncludeTreeOnlyPrinter() {
  return new ClangIncludeTreeOnlyPrinter;
}

ModulePass *llvm::createClangIncludeTreeViewer() {
  return new ClangIncludeTreeViewer;
}

ModulePass *llvm::createClangIncludeTreeOnlyViewer() {
  return new ClangIncludeTreeOnlyViewer;
}
