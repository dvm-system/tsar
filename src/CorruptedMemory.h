//===- DIEstimateMemory.h - Memory Hierarchy (Debug) ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to determine insertion hints for debug alias nodes
// containing promoted and corrupted memory locations. This is an internal file.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_CORRUPTED_MEMORY_H
#define TSAR_CORRUPTED_MEMORY_H

#include "tsar_data_flow.h"
#include "DIEstimateMemory.h"
#include "DIMemoryHandle.h"
#include "EstimateMemory.h"
#include "SpanningTreeRelation.h"
#include "tsar_dbg_output.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/PointerIntPair.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/Support/Debug.h>
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
    for (size_type Idx = size(); Idx > 0; --Idx)
      if (mMemory[Idx - 1]->getAsMDNode() == *I)
        mMemory.erase(&mMemory[Idx - 1]);
  }
private:
  CorruptedList mMemory;
  CorruptedMemoryItem *mForward = nullptr;
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
    llvm::DIVariable *, llvm:: TinyPtrVector<llvm::DIExpression *>,
    llvm::DenseMapInfo<llvm::DIVariable *>,
    tsar::TaggedDenseMapPair<
      bcl::tagged<llvm::DIVariable *, llvm::DIVariable>,
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
  using DIMemoryCash =
    llvm::DenseMap<const EstimateMemory *, std::unique_ptr<DIMemory>>;

  /// Already constructed memory locations for a new debug alias tree.
  using DIUnknownMemoryCash =
    llvm::DenseMap<const llvm::Value *, std::unique_ptr<DIMemory>>;

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

  /// Cashed information about traversed nodes.
  struct NodeInfo {
    NodeInfo() = default;

    /// \brief Parent of unknown node that must contain corrupted locations.
    ///
    /// The special case occurs if all location in a this node a destroyed and
    /// corrupted. In this case this is a collapsed ParentOfUnknown nodes for
    /// its children of this child. In this case `Items` contains multiple records and CorruptedWL
    /// contains list of unprocessed corrupted locations.
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

  /// If a specified variable produces a root of a debug alias subtree which
  /// which must be a children of some unknown node than corrupted memory
  /// locations which forms this node are returned.
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
  std::pair<bool, bool> isCorrupted(const DIMemory &M) const {
    if (mCorruptedSet.count({ M.getAsMDNode(), true }))
      return std::make_pair(true, true);
    if (mCorruptedSet.count({ M.getAsMDNode(), false }))
      return std::make_pair(true, false);
    return std::make_pair(false, false);
  }

  /// Returns already constructed debug memory location for a specified memory.
  std::unique_ptr<DIMemory> popFromCash(const EstimateMemory *EM) {
    auto Itr = mCashedMemory.find(EM);
    if (Itr == mCashedMemory.end())
      return nullptr;
    auto M = std::move(Itr->second);
    mCashedMemory.erase(Itr);
    return M;
  }

  /// Returns already constructed debug memory location for a specified memory.
  std::unique_ptr<DIMemory> popFromCash(const llvm::Value *V) {
    auto Itr = mCashedUnknownMemory.find(V);
    if (Itr == mCashedUnknownMemory.end())
      return nullptr;
    auto M = std::move(Itr->second);
    mCashedUnknownMemory.erase(Itr);
    return M;
  }

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

  /// \brief Add a copy of memory locations from a specified work list to a list
  /// of corrupted memory locations.
  ///
  /// If Item is `nullptr` new item will be created.
  /// \return Pointer to an updated item.
  CorruptedMemoryItem * copyToCorrupted(
    const llvm::SmallVectorImpl<DIMemory *> &WL, CorruptedMemoryItem *Item);

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

  /// \brief Returns true if a specified memory location will be the same after
  /// its rebuilding for any bound memory.
  ///
  /// If not the same memory location will be built it means that a specified
  /// location is corrupted.
  bool isSameAfterRebuild(DIEstimateMemory &M);

  /// \brief Returns true if a specified memory location will be the same after
  /// its rebuilding for any bound memory.
  ///
  /// If not the same memory location will be built it means that a specified
  /// location is corrupted.
  bool isSameAfterRebuild(DIUnknownMemory &M);

#ifndef NDEBUG
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
  DIMemoryCash mCashedMemory;
  DIUnknownMemoryCash mCashedUnknownMemory;
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
}
#endif//TSAR_CORRUPTED_MEMORY_H
