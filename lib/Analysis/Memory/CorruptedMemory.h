//===- CorruptedMemory.h -- Memory Hierarchy (Metadata) ---------*- C++ -*-===//
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
// This file defines classes to determine insertion hints for debug alias nodes
// containing promoted and corrupted memory locations. This is an internal file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CORRUPTED_MEMORY_H
#define TSAR_CORRUPTED_MEMORY_H

#include "tsar/ADT/DataFlow.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/PointerIntPair.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/GraphWriter.h>
#include <memory>

namespace llvm {
class DominatorTree;
class DataLayout;
class Function;
class MDNode;
class DIVariable;
class DIExpression;
}

namespace tsar {
/// \brief List of memory locations which have been transformed during some
/// passes.
///
/// The basis to construct memory locations from this list is a debug alias
/// tree that was build before transformations. This memory locations do not
/// attached to any alias tree and can be safely moved to a reconstructed tree.
///
/// Two items can be connected using `setPredecessor()` and `setSuccessor()`
/// methods of tsar::SmallDFNode class. One of connected items can be forward to
/// another item.
///
/// Each item represents an unknown node in a new debug alias tree. To simply
/// merge two nodes if necessary connecting and forwarding are used.
class CorruptedMemoryItem : public SmallDFNode<CorruptedMemoryItem, 4> {
  using CorruptedList = llvm::SmallVector<std::unique_ptr<DIMemory>, 4>;

public:
  /// This represents number of memory locations in the list.
  using size_type = CorruptedList::size_type;

  /// Forwards this item to a specified item and move all memory locations to
  /// this item.
  void setForward(CorruptedMemoryItem *To) {
    mForward = To;
    if (this == To)
      return;
    for (CorruptedList::size_type I = 0, E = mMemory.size(); I < E; ++I)
      To->push(pop());
  }

  /// Returns target of forwarding.
  CorruptedMemoryItem * getForward() noexcept { return mForward; }

  /// Returns size of this item (not target of forwarding).
  size_type size() const { return mMemory.size(); }

  /// Returns true if this item is empty (non target of forwarding).
  bool empty() const { return mMemory.empty(); }

  /// Extracts memory location from this item (not target of forwarding).
  std::unique_ptr<DIMemory> pop() {
    auto M = std::move(mMemory.back());
    mMemory.pop_back();
    return M;
  }

  /// Appends memory location to this item (not target of forwarding).
  void push(std::unique_ptr<DIMemory> &&M) {
    mMemory.push_back(std::move(M));
  }

  /// Removes locations with MDNode metadata equal to one of specified values.
  template<class ItrTy> void erase(ItrTy I, ItrTy E) {
    for (; I != E; ++I)
      for (size_type Idx = size(); Idx > 0; --Idx)
        if (mMemory[Idx - 1]->getBaseAsMDNode() == *I)
          mMemory.erase(&mMemory[Idx - 1]);
  }
private:
  CorruptedList mMemory;
  CorruptedMemoryItem *mForward = nullptr;
};

/// Represent a node in a replacement graph.
///
/// Replacement graph is constructed while memory from the previous alias tree
/// is rebuilt for the new alias tree. This graph describes memory locations
/// which should be RAUWd. A node contains two memory locations with the same
/// raw metadata-level base. The first one (getSuccessorFrom()) is from
/// the previous alias tree and the second one (getPredecessorTo()) is build
/// for the new alias tree.
///
/// An edge from node A(From, To) to B(From, To) means that A(From) memory
/// should be RAUWd with B(To) memory. An edge from A to A is also possible.
/// If B(To) is null this means that A(From) is corrupted.
///
/// Replacement graph is built for a single node in a previous alias tree and
/// it may contains separate subgraphs which is joined with an utility root
/// node. Each subgraph determines sequence of RAUW operations which do not
/// depend to other subgraphs.
class ReplacementNode : public SmallDFNode<ReplacementNode, 4> {
public:
  explicit ReplacementNode(llvm::Optional<unsigned> DWLang) :
    mDWLang(DWLang ? *DWLang : llvm::dwarf::DW_LANG_C) {}

  DIMemory * getSuccessorFrom() noexcept { return mSuccFrom; }
  const DIMemory * getSuccessorFrom() const noexcept { return mSuccFrom; }

  void setSuccessorFrom(DIMemory *SuccFrom) noexcept {
    assert(!getSuccessorFrom() && SuccFrom && (!getPredecessorTo() ||
     getPredecessorTo()->getAsMDNode() == SuccFrom->getAsMDNode()) &&
     "Invalid replacement!");
    mSuccFrom = SuccFrom;
  }

  DIMemory *getPredecessorTo() { return mPredTo.getPointer(); }
  const DIMemory *getPredecessorTo() const { return mPredTo.getPointer(); }

  void setPredecessorTo(DIMemory *PredTo) {
    assert(!getPredecessorTo() && PredTo && (!getSuccessorFrom() ||
      PredTo->getAsMDNode() == getSuccessorFrom()->getAsMDNode()) &&
      "Invalid replacement!");
    mPredTo.setPointer(PredTo);
  }

  bool isCorrupted() const {
    return getSuccessorFrom() && numberOfSuccessors() == 0;
  }

  bool isRoot() const { return !getSuccessorFrom() && !getPredecessorTo(); }

  bool isSelfReplacement() const {
    return numberOfSuccessors() == 1 && mSuccFrom &&
           (*succ_begin())->getPredecessorTo() &&
           mSuccFrom->getAsMDNode() ==
               (*succ_begin())->getPredecessorTo()->getAsMDNode();
  }

  bool isActiveReplacement() const { return mPredTo.getInt(); }
  void activateReplacement() { mPredTo.setInt(true); }

  unsigned getLanguage() const noexcept { return mDWLang; }

  void view(const llvm::Twine &Name = "emr") const;
  std::string write(const llvm::Twine &Name = "emr") const;
private:
  // This memory should be changed to (succ::mPredTo) a memory in a successor.
  DIMemory * mSuccFrom = nullptr;
  // A memory from (pred::mSuccFrom) predecessor must be change to this one.
  llvm::PointerIntPair<DIMemory *, 1, bool> mPredTo = { nullptr, false };
  unsigned mDWLang;
};

/// Determines insertion hints for nodes containing promoted and corrupted
/// memory locations.
class CorruptedMemoryResolver {
  /// Set of memory locations.
  using DIMemorySet =
    llvm::DenseMap<tsar::DIMemoryLocation, AssertingDIMemoryHandle<DIMemory>>;

public:
  /// Map from a variable to its fragments.
  using DIFragmentMap = llvm::DenseMap <
    llvm::DIVariable *,
    std::tuple<llvm::DILocation *, llvm::TinyPtrVector<llvm::DIExpression *>>,
    llvm::DenseMapInfo<llvm::DIVariable *>,
    tsar::TaggedDenseMapTuple<
      bcl::tagged<llvm::DIVariable *, llvm::DIVariable>,
      bcl::tagged<llvm::DILocation *, llvm::DILocation>,
      bcl::tagged<llvm::TinyPtrVector<llvm::DIExpression *>, llvm::DIExpression>>>;

private:
  /// \brief A small set to simplify check whether a memory location
  /// is corrupted.
  ///
  /// A bit flag indicates whether some values are bound to the location.
  using CorruptedSet = llvm::SmallPtrSet<
    llvm::PointerIntPair<const llvm::MDNode *, 1, bool>, 16>;

  /// This is a storage for lists of corrupted locations.
  using CorruptedPool =
    llvm::SmallVector<std::unique_ptr<CorruptedMemoryItem>, 8>;

  /// Already constructed memory locations for a new debug alias tree.
  using DIMemoryCache =
    llvm::DenseMap<const EstimateMemory *, std::unique_ptr<DIMemory>>;

  /// Already constructed memory locations for a new debug alias tree.
  ///
  /// A bit flag (`true`) indicates whether a value it is an instruction which
  /// accesses some memory while it is executed or (`false`) it is a pointer to
  /// memory.
  std::unique_ptr<DIMemory> buildDIMemory(llvm::Value &V,
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    DIMemory::Property = DIMemory::Explicit,
    DIUnknownMemory::Flags = DIUnknownMemory::NoFlags);

  using DIUnknownMemoryCache = llvm::DenseMap<
    llvm::PointerIntPair<const llvm::Value *, 1, bool>,
    std::unique_ptr<DIMemory>>;

  /// \brief Hints to attach new debug unknown node to its child in constructed
  /// debug alias tree.
  ///
  /// Each alias node in existing alias tree produces some debug alias node.
  /// Each node produced by key in this map should be a child of debug unknown
  /// alias node which contains specified corrupted memory locations. In case
  /// of unknown alias node produced node will be also unknown. So it should
  /// contains specified corrupted memory by itself.
  using AliasNodeUnknownChildren =
    llvm::DenseMap<const AliasNode *, CorruptedMemoryItem *>;

  /// \brief Hints to attach new debug unknown node to its child in constructed
  /// debug alias tree.
  ///
  /// A promoted alloca produces subtree in debug alias tree. The root of this
  /// subtree is a node which contains a variable associated with this alloca.
  /// Each key of this map produces root of some subtree.
  /// Each node produced by key in this map should be a child for debug unknown
  /// alias node which contains specified corrupted memory locations.
  using VariableUnknownChildren =
    llvm::DenseMap<const llvm::DIVariable *, CorruptedMemoryItem *>;

  /// \brief Hints to attach new debug unknown node to its child in constructed
  /// debug alias tree.
  ///
  /// A promoted fragment of memory locations produces some debug alias node.
  /// Each node produced by key in this map should be a child for debug unknown
  /// alias node which contains specified corrupted memory locations.
  using FragmentUnknownChildren =
    llvm::DenseMap<DIMemoryLocation, CorruptedMemoryItem *>;

  /// Cached information about traversed nodes.
  struct NodeInfo {
    NodeInfo() = default;

    /// \brief Parent of unknown node that must contain corrupted locations.
    ///
    /// The special case occurs if all location in a this node a destroyed and
    /// corrupted. In this case this is a collapsed ParentOfUnknown nodes for
    /// its children of this child. In this case `Items` contains multiple
    /// records and CorruptedWL contains list of unprocessed corrupted locations.
    AliasNode *ParentOfUnknown = nullptr;

    /// \brief These lists contain locations for an unknown nodes which are
    /// children for a `ParentOfUnknown` node.
    ///
    /// If there are no corrupted locations in the node or all locations are
    /// destroyed and corrupted than this list contains multiple records.
    /// Theses records are produced by children of this node and will be
    /// merged if necessary.
    llvm::SmallVector<CorruptedMemoryItem *, 1> Items;

    /// List of unprocessed corrupted locations.
    llvm::SmallVector<DIMemory *, 4> CorruptedWL;

    /// List of nodes from alias tree that produce descendant nodes of
    /// a unknown node with a specified parent `ParentOfUnknonwn`.
    llvm::SmallPtrSet<AliasNode *, 4> NodesWL;

    /// List of promoted memory locations that produce a subtree and root of
    /// this subtree is descendant of an unknown node.
    llvm::SmallVector<DIMemoryLocation, 4> PromotedWL;

#ifdef LLVM_DEBUG
    void sizeLog() {
      llvm::dbgs() << "[DI ALIAS TREE]: node info has size " << sizeof(*this);
      llvm::dbgs() << " and consists of ";
      llvm::dbgs() << Items.size() << " corrupted lists, ";
      llvm::dbgs() << CorruptedWL.size() << " corrupted memory locations, ";
      llvm::dbgs() << NodesWL.size() << " alias tree nodes, ";
      llvm::dbgs() << PromotedWL.size() << " promoted locations\n";
    }
#endif
  };

public:
  /// \brief Creates resolver for a specified function.
  ///
  /// \param [in] F Function which is analyzed.
  /// \param [in] DIAT Debug alias tree for the function `F`. It should be
  /// build before function transformation. If it is `nullptr` than only
  /// safely promoted locations will be determined.
  /// \param [in] AT Alias tree for the function `F`. Must not be `nullptr` if
  /// DIAT is specified.
  explicit CorruptedMemoryResolver(llvm::Function &F,
    const llvm::DataLayout *DL = nullptr,
    const llvm::DominatorTree *DT = nullptr,
    DIAliasTree *DIAT = nullptr, AliasTree *AT = nullptr) :
      mFunc(&F), mDL(DL), mDT(DT), mDIAT(DIAT), mAT(AT) {
    assert((!DIAT || &DIAT->getFunction() == &F) &&
      "Alias tree must be constructed for a specified function!");
    assert((!DIAT || AT) &&
      "Alias tree must not be null if debug alias tree is specified!");
  }

  /// Returns safely promoted variables for which separate subtree of
  // debug alias tree may be safely constructed.
  const DIFragmentMap & getFragments() const noexcept {return mVarToFragments;}

  /// If a specified alias node produces a debug alias node which must be
  /// a children of some unknown node than corrupted memory locations which
  /// forms this node are returned.
  CorruptedMemoryItem * hasUnknownParent(const AliasNode &AN) {
    auto I = mChildOfUnknown.find(&AN);
    return I == mChildOfUnknown.end() ? nullptr : I->second->getForward();
  }

  /// If a specified variable produces a root of a debug alias subtree
  /// which must be a children of some unknown node than corrupted memory
  /// locations which forms this node are returned.
  ///
  /// Note, if a root of subtree is also corrupted it is contained in the result
  /// of this function.
  CorruptedMemoryItem * hasUnknownParent(const llvm::DIVariable &Var) {
    auto I = mVarChildOfUnknown.find(&Var);
    return I == mVarChildOfUnknown.end() ? nullptr : I->second->getForward();
  }

  /// If a specified memory location produces a root of a debug alias node which
  /// which must be a children of some unknown node than corrupted memory
  /// locations which forms this node are returned.
  CorruptedMemoryItem * hasUnknownParent(const DIMemoryLocation &Loc) {
    auto I = mFragChildOfUnknown.find(Loc);
    return I == mFragChildOfUnknown.end() ? nullptr : I->second->getForward();
  }


  /// Returns item which represents unknown node which connects with its parent
  /// only. The parent is a top level node.
  CorruptedMemoryItem * distinctUnknown(
      llvm::SmallVectorImpl<CorruptedMemoryItem *>::size_type Idx) {
    return mDistinctUnknown[Idx]->getForward();
  }

  /// Returns size of a list of distinct unknown nodes.
  llvm::SmallVectorImpl<CorruptedMemoryItem *>::size_type
  distinctUnknownNum() const {
    return mDistinctUnknown.size();
  }

  /// Returns true if a specified memory location is corrupted
  /// (first value is true) and whether some values are bound to
  /// the corrupted location (second value is true).
  std::pair<bool, bool> isCorrupted(const llvm::MDNode *MD) const {
    if (mCorruptedSet.count({ MD, true }))
      return std::make_pair(true, true);
    if (mCorruptedSet.count({ MD, false }))
      return std::make_pair(true, false);
    return std::make_pair(false, false);
  }

  /// Returns already constructed debug memory location for a specified memory.
  std::unique_ptr<DIMemory> popFromCache(const EstimateMemory *EM) {
    auto Itr = mCachedMemory.find(EM);
    if (Itr == mCachedMemory.end())
      return nullptr;
    auto M = std::move(Itr->second);
    mCachedMemory.erase(Itr);
    return M;
  }

  /// Returns already constructed debug memory location for a specified memory.
  std::unique_ptr<DIMemory> popFromCache(const llvm::Value *V, bool IsExec);

  /// Returns memory location which has represented a specified promoted
  /// location before promotion.
  DIMemory * beforePromotion(const DIMemoryLocation &Loc) {
    auto Itr = mSmallestFragments.find(Loc);
    return (Itr != mSmallestFragments.end()) ? Itr->second : nullptr;
  }

  /// Determines insertion hints for nodes containing promoted and corrupted
  /// memory locations.
  void resolve();

private:
  /// Finds fragments of a variables which are used in a program and does not
  /// alias each other.
  ///
  /// There are two out parameters:
  /// - This is a map from a variable to its fragments (mFragments).
  /// - This is a set of all fragments (mSmallestFragments).
  void findNoAliasFragments();

  /// \brief This determines place of an unknown node in a new debug alias tree
  /// which contains a list of corrupted memory locations from a specified node.
  ///
  /// \post
  /// - Stack of traversed children will be updated.
  /// - In success collections with hints will be updated.
  void determineCorruptedInsertionHint(DIAliasMemoryNode &N,
    const SpanningTreeRelation<AliasTree *> &AliasSTR);

  /// \brief Extracts and joins information about traversed children of
  /// a specified node from the stack.
  ///
  /// \param [in] Parent Currently processed node. This is used to determine
  /// number of records that should be extracted from the stack.
  /// \param [in] AliasSTR This is used to perform search of LCA node.
  /// \post Information (`NodeInfo`) for children of `Parent` will be extracted
  /// from `mChildStack` and new record `Info` will be pushed.
  void collapseChildStack(const DIAliasNode &Parent,
    const SpanningTreeRelation<AliasTree *> &AliasSTR);

  /// Uses promoted locations to determine insertion hint for unknown node.
  void promotedBasedHint(NodeInfo &Info);

  /// Uses alias tree to determine insertion hint for unknown node.
  void aliasTreeBasedHint(
    const SpanningTreeRelation<AliasTree *> &AliasSTR, NodeInfo &Info);

  /// Determines insertion hints for separate unknown nodes.
  void distinctUnknownHint(NodeInfo &Info);

  /// Add a copy of memory locations from a specified work list to a list
  /// of corrupted memory locations.
  void copyToCorrupted(
    const llvm::SmallVectorImpl<DIMemory *> &WL, CorruptedMemoryItem *Item);

  /// Allocates memory for a new list of corrupted memory locations.
  CorruptedMemoryItem * newCorrupted() {
    mCorrupted.push_back(std::make_unique<CorruptedMemoryItem>());
    return mCorrupted.back().get();
  }

  /// Merges corrupted lists with specified key in
  /// mChildOfUnknown map to a specified corrupted list and update map.
  void merge(CorruptedMemoryItem *To, const AliasNode *Key) {
    auto Pair = mChildOfUnknown.try_emplace(Key, To);
    if (!Pair.second)
      merge(To, Pair.first->second);
  }

  /// Merges corrupted lists with specified key in
  /// mVarChildOfUnknown map to a specified corrupted list and update map.
  void merge(CorruptedMemoryItem *To, const llvm::DIVariable *Key) {
    auto Pair = mVarChildOfUnknown.try_emplace(Key, To);
    if (!Pair.second)
      merge(To, Pair.first->second);
  }

  /// Merges corrupted list which associates with ancestor of `N` which is
  /// a child of P to list To.
  void mergeChild(CorruptedMemoryItem *To, AliasNode *P, AliasNode *N) {
    auto Parent = N;
    if (Parent != mAT->getTopLevelNode())
      for (; Parent->getParent(*mAT) != P; Parent = Parent->getParent(*mAT));
    merge(To, Parent);
  }

  /// Merges all lists in a specified range.
  template<class ItrTy> void mergeRange(ItrTy I, ItrTy E) {
    auto ToItr = I++;
    for (; I != E; ++I)
      merge(*ToItr, *I);
  }

  /// Merges specified corrupted lists.
  void merge(CorruptedMemoryItem *LHS, CorruptedMemoryItem *RHS);

  /// \brief Add information extracted from a specified node to a work lists.
  ///
  /// \parm [in] N Processed debug alias node.
  /// \param [in, out] Info Work lists that will be updated, data will be
  /// appended.
  /// - Info.CorruptedWL contains not consistent memory locations which are not
  /// promoted or still have bound values should be remembered. They will
  /// be inserted into a special unknown node into a constructed
  /// debug alias tree. Promoted locations which have bound values should not be
  /// added into a separate subtree that is associated with its variable.
  /// -Info.NodesWL comprises of alias nodes which contains values bound to
  /// locations from a specified node (not only corrupted).
  /// -Info.PromotedWL contains locations that have been promoted and are not
  /// presented into a current alias tree.
  void updateWorkLists(DIAliasMemoryNode &N, NodeInfo &Info);

  /// Determine correct sequence of replacements.
  ///
  /// (1) Build replacement graph which consists of subgraphs for independent
  /// replacements of memory.
  /// (2) Traverse SCCs of each subgraph in the post-order and perform
  /// replacement. Complex SCCs with multiple nodes prevent replacement
  /// and produce corrupted memory.
  void replaceAllMemoryUses(
    llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement,
    NodeInfo &Info);

  /// \brief Returns true if a specified memory location will be the same after
  /// its rebuilding for any bound memory.
  ///
  /// If not the same memory location will be built it means that a specified
  /// location is corrupted.
  ///
  /// \pre Memory bound to a specified location must be estimate.
  bool isSameAfterRebuildEstimate(DIMemory &M,
    llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement);

  /// \brief Returns true if a specified memory location will be the same after
  /// its rebuilding for any bound memory.
  ///
  /// If not the same memory location will be built it means that a specified
  /// location is corrupted.
  ///
  /// \pre Memory bound to a specified location must be unknown.
  bool isSameAfterRebuildUnknown(DIUnknownMemory &M,
    llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement);

  /// Returns true if a specified memory location will be the same after
  /// its rebuilding for any bound memory.
  bool isSameAfterRebuild(DIUnknownMemory &M,
      llvm::SmallVectorImpl<std::pair<DIMemory *, DIMemory *>> &Replacement) {
    if (M.isExec())
      return isSameAfterRebuildUnknown(M, Replacement);
    return isSameAfterRebuildEstimate(M, Replacement);
  }

#ifdef LLVM_DEBUG
  void pomotedCandidatesLog() {
    llvm::dbgs() << "[DI ALIAS TREE]: safely promoted candidates ";
    for (auto &Loc : mSmallestFragments)
      printDILocationSource(
        *getLanguage(*mFunc), Loc.first, llvm::dbgs()), llvm::dbgs() << " ";
    llvm::dbgs() << "\n";
  }

  void checkLog(DIMemory &M) {
    llvm::dbgs() << "[DI ALIAS TREE]: check if ";
    printDILocationSource(*getLanguage(*mFunc), M, llvm::dbgs());
    llvm::dbgs() << " is corrupted\n";
  }

  void afterRebuildLog(DIMemory &M) {
    llvm::dbgs() << "[DI ALIAS TREE]: after rebuild ";
    printDILocationSource(*getLanguage(*mFunc), M, llvm::dbgs());
    llvm::dbgs() << "\n";
  }

  void corruptedFoundLog(DIMemory &M) {
    llvm::dbgs() << "[DI ALIAS TREE]: corrupted memory found ";
    printDILocationSource(*getLanguage(*mFunc), M, llvm::dbgs());
    llvm::dbgs() << "\n";
  }
#endif

  llvm::Function *mFunc;
  const llvm::DataLayout *mDL;
  const llvm::DominatorTree *mDT;
  DIAliasTree *mDIAT;
  AliasTree *mAT;
  DIFragmentMap mVarToFragments;
  DIMemorySet mSmallestFragments;
  DIMemoryCache mCachedMemory;
  DIUnknownMemoryCache mCachedUnknownMemory;
  CorruptedSet mCorruptedSet;
  CorruptedPool mCorrupted;
  llvm::SmallVector<NodeInfo, 8> mChildStack;
  AliasNodeUnknownChildren mChildOfUnknown;
  VariableUnknownChildren mVarChildOfUnknown;
  FragmentUnknownChildren mFragChildOfUnknown;
  llvm::SmallVector<CorruptedMemoryItem *, 1> mDistinctUnknown;
};
}

namespace llvm {
template<> struct GraphTraits<tsar::CorruptedMemoryItem *> {
  using NodeRef = tsar::CorruptedMemoryItem *;
  static NodeRef getEntryNode(NodeRef I) noexcept { return I; }
  using ChildIteratorType = tsar::CorruptedMemoryItem::succ_iterator;
  static ChildIteratorType child_begin(NodeRef I) { return I->succ_begin(); }
  static ChildIteratorType child_end(NodeRef I) { return I->succ_end(); }
};

template <> struct GraphTraits<tsar::ReplacementNode *> {
  using NodeRef = tsar::ReplacementNode *;
  static NodeRef getEntryNode(NodeRef I) noexcept { return I; }

  using nodes_iterator = df_iterator<tsar::ReplacementNode *>;
  static nodes_iterator nodes_begin(tsar::ReplacementNode *G) {
    return df_begin(G);
  }
  static nodes_iterator nodes_end(tsar::ReplacementNode *G) {
    return df_end(G);
  }

  using ChildIteratorType = tsar::ReplacementNode::succ_iterator;
  static ChildIteratorType child_begin(NodeRef I) { return I->succ_begin(); }
  static ChildIteratorType child_end(NodeRef I) { return I->succ_end(); }
};

template<> struct DOTGraphTraits<tsar::ReplacementNode *> :
    public DefaultDOTGraphTraits {
  using GT = GraphTraits<tsar::ReplacementNode *>;
  using EdgeItr = typename GT::ChildIteratorType;

  explicit DOTGraphTraits(bool IsSimple = false) :
    DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const tsar::ReplacementNode *) {
    return "Replacement Tree";
  }

  std::string getNodeLabel(tsar::ReplacementNode *N, tsar::ReplacementNode *R) {
    if (N->isRoot())
      return "ROOT";
    if (N->isSelfReplacement())
      return "[[self]]";
    if (N->isCorrupted())
      return "[[corrupted]]";
    return "";
  }

  static std::string getGraphProperties(const tsar::ReplacementNode*) {
    return "rankdir=LR";
  }

  static std::string getEdgeSourceLabel(const tsar::ReplacementNode *N,
      EdgeItr) {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    if (auto *SuccFrom = N->getSuccessorFrom())
      printDILocationSource(N->getLanguage(), *SuccFrom, OS);
    return OS.str();
  }


  static bool hasEdgeDestLabels() { return true; }

  static unsigned numEdgeDestLabels(const tsar::ReplacementNode *N) {
    return N->isRoot() ? 0 : 1;
  }

  static std::string getEdgeDestLabel(const tsar::ReplacementNode *N,
      unsigned) {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    if (auto *PredTo = N->getPredecessorTo())
      printDILocationSource(N->getLanguage(), *PredTo, OS);
    return OS.str();
  }

  static bool edgeTargetsEdgeSource(const tsar::ReplacementNode *N, EdgeItr) {
    return !N->isRoot();
  }

  static EdgeItr getEdgeTarget(const void *, EdgeItr I) {
    return GT::child_begin(*I);
  }
};
}

namespace tsar {
void ReplacementNode::view(const llvm::Twine &Name) const {
  llvm::ViewGraph(const_cast<ReplacementNode *>(this), Name, false,
    llvm::DOTGraphTraits<ReplacementNode *>::getGraphName(this));
}

std::string ReplacementNode::write(const llvm::Twine &Name) const {
  return llvm::WriteGraph(
      const_cast<ReplacementNode *>(this), Name, false,
      llvm::DOTGraphTraits<ReplacementNode *>::getGraphName(this));
}
}
#endif//TSAR_CORRUPTED_MEMORY_H
