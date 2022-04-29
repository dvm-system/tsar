//===- IncludeTree.h ----- Hierarchy of Include Files -----------*- C++ -*-===//
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

#ifndef TSAR_CLANG_INCLUDE_TREE_H
#define TSAR_CLANG_INCLUDE_TREE_H

#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/convertible_pair.h>
#include <bcl/utility.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Pass.h>
#include <vector>

namespace tsar {
class ClangTransformationContext;
class TransformationContextBase;

/// Represent a file in the file tree. Each file node contains a list of
/// top-level declarations and includes which can be sorted according
/// to a source code location.
class FileNode {
public:
  using OutermostDecl = GlobalInfoExtractor::OutermostDecl;

  /// Child node which can be a declaration or an include file.
  using ChildT =
    llvm::PointerUnion<const GlobalInfoExtractor::OutermostDecl *, FileNode *>;
private:
  using EdgeT = std::pair<ChildT, clang::SourceLocation>;
  using ChildrenT = std::vector<EdgeT>;

  static const ChildT &iterator_helper(const EdgeT &E) { return E.first; }
public:
  /// This iterates over child nodes.
  class iterator : public llvm::iterator_adaptor_base<
    iterator, ChildrenT::iterator,
    typename std::iterator_traits<
    ChildrenT::iterator>::iterator_category,
    ChildT> {
  public:
    iterator() = default;
    iterator(ChildrenT::iterator I)
      : iterator_adaptor_base(std::move(I)) {}
    ChildT &operator*() const { return this->I->first; }
  };

  using iterator_range = llvm::iterator_range<iterator>;

  static bool isFile(const ChildT &Child) { return Child.is<FileNode *>(); }
  static bool isDecl(const ChildT &Child) { return !Child.is<FileNode *>(); }

  static FileNode &asFile(ChildT &Child) {
    assert(isFile(Child) && "Node must be a file!");
    return *Child.get<FileNode *>();
  }
  static const FileNode &asFile(const ChildT &Child) {
    assert(isFile(Child) && "Node must be a file!");
    return *Child.get<FileNode *>();
  }

  static const OutermostDecl &asDecl(const ChildT &Child) {
    assert(isDecl(Child) && "Node must be a declaration!");
    return *Child.get<const OutermostDecl *>();
  }

  FileNode(clang::FileID ID, ClangTransformationContext &TfmCtx);

  auto &getTransformationContext() noexcept { return *mTfmCtx; }
  const auto &getTransformationContext() const noexcept { return *mTfmCtx; }

  /// Return location of corresponding #include directive in a source code.
  clang::SourceLocation getLocation() const { return mIncludeLoc; }

  /// Return true if the current file is an include one.
  bool isInclude() const { return mIncludeLoc.isValid(); }

  /// Return identifier of this file
  clang::FileID getFile() const { return mFile; }

  iterator begin() { return mChildren.begin(); }
  iterator end() { return mChildren.end(); }

  iterator_range children() { return make_range(begin(), end()); }

  std::size_t size() const { return mChildren.size(); }
  bool empty() const { return mChildren.empty(); }

  void setParent(FileNode *Parent) noexcept { mParent = Parent; }
  FileNode *getParent() noexcept { return mParent; }
  const FileNode *getParent() const noexcept { return mParent; }

  void push_back(const OutermostDecl *OD) {
    assert(OD && "Declaration must not be null!");
    mChildren.emplace_back(OD, OD->getDescendant()->getLocation());
  }

  void push_back(FileNode *IN) {
    assert(IN && "Include node must not be null!");
    assert(IN->isInclude() && "Include location must be valid!");
    mChildren.emplace_back(IN, IN->getLocation());
    IN->setParent(this);
  }

  /// Sort all children according to their source code locations in this file.
  void sort();

private:
  ClangTransformationContext *mTfmCtx{nullptr};
  clang::FileID mFile;
  clang::SourceLocation mIncludeLoc;
  ChildrenT mChildren;
  FileNode *mParent = nullptr;
};
}

namespace llvm {
template<> struct GraphTraits<tsar::FileNode::ChildT *> {
  using NodeRef = tsar::FileNode::ChildT *;
  static NodeRef getEntryNode(NodeRef G) { return G; }

  using nodes_iterator = df_iterator<NodeRef>;
  static nodes_iterator nodes_begin(NodeRef G) { return df_begin(G); }
  static nodes_iterator nodes_end(NodeRef G) { return df_end(G); }

  using ChildIteratorType = pointer_iterator<tsar::FileNode::iterator>;
  static ChildIteratorType child_begin(NodeRef N) {
    using tsar::FileNode;
    if (FileNode::isFile(*N))
      return ChildIteratorType(FileNode::asFile(*N).begin());
    return ChildIteratorType();
  }
  static ChildIteratorType child_end(NodeRef N) {
    using tsar::FileNode;
    if (FileNode::isFile(*N))
      return ChildIteratorType(FileNode::asFile(*N).end());
    return ChildIteratorType();
  }
};
}

namespace tsar {
/// Represent hierarchy of the analyzed files
class FileTree {
  using FileMap = llvm::DenseMap<clang::FileID, std::unique_ptr<FileNode>>;

  using DerefFnTy = FileNode & (*)(FileMap::value_type &);
  using ConstDerefFnTy = const FileNode &(*)(const FileMap::value_type &);
  static FileNode &file_helper(FileMap::value_type &V) {
    return *V.second;
  }
  static const FileNode &const_file_helper(const FileMap::value_type &V) {
    return *V.second;
  }

  using DerefRootFnTy = FileNode & (*)(FileNode::ChildT &);
  using ConstDereRootfFnTy = const FileNode &(*)(const FileNode::ChildT &);
  static FileNode &root_helper(FileNode::ChildT &V) {
    return FileNode::asFile(V);
  }
  static const FileNode &const_root_helper(const FileNode::ChildT &V) {
    return FileNode::asFile(V);
  }

  using DerefIntFnTy =
      const FileNode::OutermostDecl &(*)(const FileNode::ChildT &);
  static const FileNode::OutermostDecl &
  internal_helper(const FileNode::ChildT &V) {
    return FileNode::asDecl(V);
  }

  using RootList = std::vector<FileNode::ChildT>;
  using DeclarationMap =
      llvm::DenseMap<const FileNode::OutermostDecl *, FileNode *>;

  /// This iterates over all node (files and top-level declarations) in a
  /// file tree.
  template<class RootItrTy, class ChildPtrTy>
  class iterator_impl
      : public std::iterator<std::input_iterator_tag, FileNode::ChildT> {
    using GT = llvm::GraphTraits<FileNode::ChildT *>;
  public:
    iterator_impl() = default;

    FileNode::ChildT & operator*() const {
      return FileNode::isDecl(*mRootItr) ? *mRootItr : **mChildItr;
    }
    FileNode::ChildT * operator->() const { return &operator*(); }

    iterator &operator++() {
      if (FileNode::isDecl(*mRootItr)) {
        ++mRootItr;
        if (mRootItr != mRootEndItr && FileNode::isFile(*mRootItr)) {
          mChildItr = GT::nodes_begin(&*mRootItr);
          mChildEndItr = GT::nodes_end(&*mRootItr);
        }
        return *this;
      }
      if (mChildItr != mChildEndItr)
        ++mChildItr;
      if (mChildItr == mChildEndItr) {
        ++mRootItr;
        if (mRootItr != mRootEndItr) {
          mChildItr = GT::nodes_begin(&*mRootItr);
          mChildEndItr = GT::nodes_end(&*mRootItr);
        }
      }
      return *this;
    }

    iterator operator++(int) {
      auto Tmp = *this;
      ++Tmp;
      return Tmp;
    }

    bool operator==(const iterator_impl &RHS) const {
      return mRootItr == RHS.mRootItr &&
        (mRootItr == mRootEndItr || mChildItr == RHS.mChildItr);
    }

    bool operator!=(const iterator_impl &RHS) const {
      return !operator==(RHS);
    }

  private:
    iterator_impl(RootItrTy I, RootItrTy EI)
      : mRootItr(I), mRootEndItr(EI)
      , mChildItr(GT::nodes_begin(nullptr))
      , mChildEndItr(GT::nodes_end(nullptr)) {
      if (mRootItr != mRootEndItr && FileNode::isFile(*mRootItr)) {
        mChildItr = GT::nodes_begin(&*mRootItr);
        mChildEndItr = GT::nodes_end(&*mRootItr);
      }
    }
    friend class FileTree;

    RootItrTy mRootItr;
    RootItrTy mRootEndItr;
    GT::nodes_iterator mChildItr;
    GT::nodes_iterator mChildEndItr;
  };

public:
  FileTree() = default;

  /// This iterates over all node (files and top-level declarations) in a
  /// file tree.
  using iterator = iterator_impl<RootList::iterator, FileNode::ChildT *>;
  using iterator_range = llvm::iterator_range<iterator>;

  iterator begin() { return iterator(mRoots.begin(), mRoots.end()); }
  iterator end() { return iterator(mRoots.end(), mRoots.end()); }

  iterator_range nodes() { return make_range(begin(), end()); }

  /// This iterates over all files.
  using file_iterator = llvm::mapped_iterator<FileMap::iterator, DerefFnTy>;
  using const_file_iterator =
      llvm::mapped_iterator<FileMap::const_iterator, ConstDerefFnTy>;

  using file_range = llvm::iterator_range<file_iterator>;
  using const_file_range = llvm::iterator_range<const_file_iterator>;

  /// Create a new file node which is not linked with any other file or
  /// declaration.
  std::pair<file_iterator, bool> insert(clang::FileID ID,
                                        ClangTransformationContext &TfmCtx) {
    auto Info = mFiles.try_emplace(ID);
    bool IsNew = false;
    if (!Info.first->second) {
      IsNew = true;
      Info.first->second = std::make_unique<FileNode>(ID, TfmCtx);
    }
    return std::make_pair(file_iterator(Info.first, file_helper), IsNew);
  }

  file_iterator file_begin() {
    return file_iterator(mFiles.begin(), file_helper);
  }
  file_iterator file_end() { return file_iterator(mFiles.end(), file_helper); }

  const_file_iterator file_begin() const {
    return const_file_iterator(mFiles.begin(), const_file_helper);
  }
  const_file_iterator file_end() const {
    return const_file_iterator(mFiles.end(), const_file_helper);
  }

  file_range files() { return make_range(file_begin(), file_end()); }
  const_file_range files() const { return make_range(file_begin(), file_end());}

  /// Return number of files in the analyzed files.
  std::size_t file_size() const { return mFiles.size(); }

  /// Return true if there is no files.
  bool file_empty() const { return mFiles.empty(); }

  /// Return file which contains a specified global declaration.
  FileNode *find(const FileNode::OutermostDecl *OD) {
    return const_cast<FileNode *>(
        static_cast<const FileTree *>(this)->find(OD));
  }

  /// Return file which contains a specified global declaration.
  const FileNode *find(const FileNode::OutermostDecl *OD) const {
    assert(OD && "Declaration must not be null!");
    auto Itr = mDeclToFile.find(OD);
    return Itr != mDeclToFile.end() ? Itr->second : nullptr;
  }

  /// This iterates over top-level files which are not included in any file.
  using root_iterator =
      llvm::mapped_iterator<RootList::iterator, DerefRootFnTy>;
  using const_root_iterator =
      llvm::mapped_iterator<RootList::const_iterator, ConstDereRootfFnTy>;
  using root_range = llvm::iterator_range<root_iterator>;
  using const_root_range = llvm::iterator_range<const_root_iterator>;

  root_iterator root_begin() {
    return root_iterator(mRoots.begin() + mNumberOfInternals, root_helper);
  }
  root_iterator root_end() { return root_iterator(mRoots.end(), root_helper); }

  const_root_iterator root_begin() const {
    return const_root_iterator(mRoots.begin() + mNumberOfInternals,
                               const_root_helper);
  }
  const_root_iterator root_end() const {
    return const_root_iterator(mRoots.end(), const_root_helper);
  }

  root_range roots() { return make_range(root_begin(), root_end()); }
  const_root_range roots() const { return make_range(root_begin(), root_end());}

  const FileNode *findRoot(const FileNode *File) const {
    if (!File)
      return nullptr;
    while (auto Parent = File->getParent())
      File = Parent;
    return File;
  }

  const FileNode *findRoot(const FileNode::OutermostDecl *OD) const {
    return findRoot(find(OD));
  }

  const FileNode * findRoot(const FileNode::ChildT &Child) const {
    auto *File = FileNode::isFile(Child) ? &FileNode::asFile(Child)
                                         : find(&FileNode::asDecl(Child));
    return findRoot(File);
  }

  FileNode  * findRoot(FileNode *File) {
    return const_cast<FileNode *>(
        findRoot(static_cast<const FileTree *>(this)->findRoot(File)));
  }

  FileNode  * findRoot(const FileNode::OutermostDecl *OD) {
    return const_cast<FileNode *>(
        findRoot(static_cast<const FileTree *>(this)->findRoot(OD)));
  }

  FileNode  * findRoot(FileNode::ChildT &Child) {
    return const_cast<FileNode *>(
        findRoot(static_cast<const FileTree *>(this)->findRoot(Child)));
  }

  /// Return number of top-level files.
  std::size_t root_size() const { return mRoots.size(); }
  bool root_empty() const { return mRoots.empty(); }

  /// This iterates over internal declarations which are not explicitly declared
  /// in any file.
  using internal_iterator =
      llvm::mapped_iterator<RootList::iterator, DerefIntFnTy>;
  using const_internal_iterator =
      llvm::mapped_iterator<RootList::const_iterator, DerefIntFnTy>;

  using internal_range = llvm::iterator_range<internal_iterator>;
  using const_internal_range = llvm::iterator_range<const_internal_iterator>;

  internal_iterator internal_begin() {
    return internal_iterator(mRoots.begin(), internal_helper);
  }
  internal_iterator internal_end() {
    return internal_iterator(mRoots.begin() + mNumberOfInternals,
                             internal_helper);
  }

  const_internal_iterator internal_begin() const {
    return const_internal_iterator(mRoots.begin(), internal_helper);
  }
  const_internal_iterator internal_end() const {
    return const_internal_iterator(mRoots.begin() + mNumberOfInternals,
                                   internal_helper);
  }

  internal_range internals() {
    return llvm::make_range(internal_begin(), internal_end());
  }
  const_internal_range internals() const {
    return llvm::make_range(internal_begin(), internal_end());
  }

  /// Return number of internal declarations.
  std::size_t internal_size() const { return mNumberOfInternals; }
  bool internal_empty() const { return mNumberOfInternals == 0; }

  /// Rebuild source tree.
  ///
  /// \pre Iterator points to a pair of a transformation context and global
  /// information.
  template<typename ItrT> bool reconstruct(ItrT I, ItrT EI) {
    clear();
    bool IsOk{true};
    for (; I != EI; ++I)
      IsOk &= reconstruct(*I->first, I->second->GIE);
    mNumberOfInternals = mRoots.size();
    for (auto &FI : files()) {
      FI.sort();
      if (!FI.isInclude())
        mRoots.push_back(&FI);
    }
    return IsOk;
  }

  /// Remove all files and declarations from the tree.
  void clear() {
    mRoots.clear();
    mDeclToFile.clear();
    mFiles.clear();
    mNumberOfInternals = 0;
  }

  void view(const FileNode &FN, const llvm::Twine &Name = "include") const;
  std::string write(const FileNode &FN,
                    const llvm::Twine &Name = "include") const;

private:
  /// Rebuild source tree.
  bool reconstruct(TransformationContextBase &TfmCtx,
                   const GlobalInfoExtractor &GIE);

  FileMap mFiles;
  RootList mRoots;
  unsigned mNumberOfInternals = 0;
  DeclarationMap mDeclToFile;
};
}

namespace llvm {
/// This is similar to graph traits for a single node, but also provide access
/// to the whole tree from a graph object.
template <>
struct GraphTraits<
    bcl::convertible_pair<tsar::FileNode::ChildT *, tsar::FileTree *>>
    : public GraphTraits<tsar::FileNode::ChildT *> {};

/// This allow to iterate over all nodes in a file tree.
template <>
struct GraphTraits<tsar::FileTree *>
    : public GraphTraits<tsar::FileNode::ChildT *> {
  using nodes_iterator = pointer_iterator<tsar::FileTree::iterator>;
  static nodes_iterator nodes_begin(tsar::FileTree *G) {
    return nodes_iterator(G->begin());
  }
  static nodes_iterator nodes_end(tsar::FileTree *G) {
    return nodes_iterator(G->end());
  }
};

class ClangIncludeTreePass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangIncludeTreePass() : ModulePass(ID) {
    initializeClangIncludeTreePassPass(*PassRegistry::getPassRegistry());
  }

  const tsar::FileTree & getFileTree() const noexcept { return *mFileTree; }
  tsar::FileTree & getFileTree() noexcept { return *mFileTree; }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() {
    if (mFileTree)
      mFileTree->clear();
  }

private:
  std::unique_ptr<tsar::FileTree> mFileTree;
};
}
#endif//TSAR_CLANG_INCLUDE_TREE_H
