//===--- EstimateMemory.h ------- Memory Hierarchy --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines AliasTree and AliasNode classes to classify a collection of
// pointer references in hierarchical way. Each AliasNode refers to memory
// disjoint from its sibling nodes. Union of memory from a parent node covers
// (or coincide with) all memory from its children. To represent memory location
// EstimateMemory class is used.
//
// EstimateMemoryPass is also proposed to construct an AliasTree.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ESTIMATE_MEMORY_H
#define TSAR_ESTIMATE_MEMORY_H

#include "tsar_df_graph.h"
#include "tsar_pass.h"
#include "tsar_utility.h"
#include <Chain.h>
#include <trait.h>
#include <utility.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/simple_ilist.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Pass.h>
#include <iterator>
#include <tuple>

namespace llvm {
class EstimateMemoryPass;
class DataLayout;
}

namespace tsar {
class AliasNode;
class AliasTree;
class EstimateMemory;

/// \brief Try to strip a pointer to an 'alloca' or a 'global variable'.
///
/// This function uses llvm::GetUnderlyingObject() with unlimited number
/// of instructions to be stripped off. The difference is that this
/// also evaluates 'inttoptr' and 'load' instructions.
///
/// In some cases there is no unique 'alloca' or 'global variable'.
/// For example, 'i32* inttoptr (i32 5 to i32*)' cast an expression to
/// a pointer. In this case this expression will be returned.
llvm::Value * stripPointer(const llvm::DataLayout &DL, llvm::Value *Ptr);

/// \brief Extends a memory location to an enclosing memory location.
///
/// For example, this extends element of array to a whole array, and element
/// of a structure to a whole structure.
/// \return True if it was successfully extended.
///
/// TODO (kaniandr@gmail.com): It is known that `getelementptr` instruction
/// may contains multiple indices. At this moment this function discards all
/// such indices. For example the result of S.X.Y will be S in case of
/// single `getelementptr` or S.X in case of sequence of two instructions.
/// The last case is preferred, so try to update this method in such way.
bool stripMemoryLevel(const llvm::DataLayout &DL, llvm::MemoryLocation &Loc);

/// \brief Strips a location to its base, the nearest estimate location.
///
/// Base location will be stored in the parameter Loc, pointer or size can be
/// changed. Final type cast will be eliminate but size will be remembered.
/// Note that `getelementptr` instructions  will not be stripped because, this
/// differs this function from llvm::GetUnderlyingObject().
/// \post Loc.Ptr will never be set to nullptr.
///
/// In the following example the base for %0 will be <&x, 2>. Note that whole
/// size of x is 4 bytes.
/// \code
///   ; *((short *)&x);
///   %x = alloca i32, align 4
///   %0 = bitcast i32* %x to i16*
///   %1 = load i16, i16* %0, align 4
/// \endcode
void stripToBase(const llvm::DataLayout &D, llvm::MemoryLocation &Loc);

/// \brief Compares to bases.
///
/// TODO (kaniandr@gmail.com): When `getelementptr` instructions are compared
/// they should have the same number of operands which are compared between
/// themselves. It does not consider the case when one `getlementptr`
/// instruction is split into sequence of instructions and other instruction
/// is not split.
bool isSameBase(const llvm::DataLayout &DL,
  const llvm::Value *BasePtr1, const llvm::Value *BasePtr2);

namespace trait {
//===----------------------------------------------------------------------===//
// Alias tags which represents more accurate relations between alias pointers.
//===----------------------------------------------------------------------===//
struct NoAlias {};
struct MayAlias {};
struct PartialAlias {};
struct MustAlias {};
struct CoincideAlias {};
struct ContainedAlias {};
struct CoverAlias {};
}

/// \brief Represents more accurate relations between alias pointers.
///
/// Alongside general information which available from llvm::AliasResult enum,
/// this enumeration also specify relation between memory location sizes:
/// - trait::CoincideAlias means that locations have the same sizes.
/// - trait::ContainedAlias means that one location is strictly contained
///   into another one.
/// - trait::CoverAlias means that one location strictly covers another
/// location.
/// Note that CoincideAlias compliant with ContainedAlias and CoverAlias in
/// this case this relation between sizes are <= or >= correspondingly.
/// llvm::AliasResult values are represented as trait::NoAlias, trait::MayAlias,
/// trait::PartialAlias and trait::MustAlias.
using AliasDescriptor = bcl::TraitDescriptor<
  bcl::TraitAlternative<trait::NoAlias, trait::MayAlias,
  bcl::TraitUnion<trait::PartialAlias, trait::CoincideAlias,
  bcl::TraitAlternative<trait::ContainedAlias, trait::CoverAlias>>,
  bcl::TraitUnion<trait::MustAlias, trait::CoincideAlias,
  bcl::TraitAlternative<trait::ContainedAlias, trait::CoverAlias>>>>;

/// This merges two alias descriptors.
AliasDescriptor mergeAliasRelation(
  const AliasDescriptor &LHS, const AliasDescriptor &RHS);

/// This determines alias relation of a first memory location to a second one.
AliasDescriptor aliasRelation(llvm::AAResults &AA, const llvm::DataLayout &DL,
  const llvm::MemoryLocation &LHS, const llvm::MemoryLocation &RHS);

/// This determines alias relation of a first estimate location to a second one.
AliasDescriptor aliasRelation(llvm::AAResults &AA, const llvm::DataLayout &DL,
  const EstimateMemory &LHS, const EstimateMemory &RHS);

/// This determines alias relation between a specified estimate location'EM' and
/// locations from a specified range [BeginItr, EndItr).
template<class ItrTy>
AliasDescriptor aliasRelation(llvm::AAResults &AA, const llvm::DataLayout &DL,
  const EstimateMemory &EM, const ItrTy &BeginItr, const ItrTy &EndItr) {
  auto I = BeginItr;
  auto MergedAD = aliasRelation(AA, DL, EM, *I);
  if (MergedAD.template is<trait::MayAlias>())
    return MergedAD;
  for (++I; I != EndItr; ++I) {
    MergedAD = mergeAliasRelation(MergedAD, aliasRelation(AA, DL, EM, *I));
    if (MergedAD.template is<trait::MayAlias>())
      return MergedAD;
  }
  return MergedAD;
}

/// Determines which of specified nodes is an ancestor of other node in
/// estimate memory tree. If nodes are not connected 'nullptr' will be returned.
const EstimateMemory * ancestor(
    const EstimateMemory *LHS, const EstimateMemory *RHS);

/// Determines which of specified nodes is an ancestor of other node in
/// estimate memory tree. If nodes are not connected 'nullptr' will be returned.
inline EstimateMemory * ancestor(EstimateMemory *LHS, EstimateMemory *RHS) {
  return const_cast<EstimateMemory *>(ancestor(LHS, RHS));
}

/// Determines which of specified nodes is an descendant of other node in
/// estimate memory tree. If nodes are not connected 'nullptr' will be returned.
inline const EstimateMemory * descendant(
    const EstimateMemory *LHS, const EstimateMemory *RHS) {
  return LHS == ancestor(LHS, RHS) ? RHS : LHS;
}

/// Determines which of specified nodes is an descendant of other node in
/// estimate memory tree. If nodes are not connected 'nullptr' will be returned.
inline EstimateMemory * descendant(EstimateMemory *LHS, EstimateMemory *RHS) {
  return const_cast<EstimateMemory *>(descendant(LHS, RHS));
}

/// Represents reference to a list of ambiguous pointers which refer to the
/// same estimate memory location.
class AmbiguousRef {
public:
  ///\brief List of ambiguous pointers which may refer some location.
  ///
  /// If alias analysis is not accurate there are a lot of may aliases and
  /// this list has unpredictable size, but for non pointers it contains a
  /// single value.
  using AmbiguousList = llvm::TinyPtrVector<const llvm::Value *>;

  /// Pool of ambiguous lists, which is necessary because all estimate locations
  /// in a hierarchy chain should have the same lists of ambiguous pointers.
  using AmbigiousPool = std::vector<AmbiguousList>;

  /// Adds new list to a specified pool and returns reference to this list.
  static AmbiguousRef make(AmbigiousPool &P) {
    P.push_back(AmbiguousList());
    return AmbiguousRef(P, P.size() - 1);
  }

  /// Creates a reference to an ambiguous list which is stored in a pool `Pool`
  /// and has a specified index `Idx`.
  AmbiguousRef(AmbigiousPool &P, size_t Idx) : mPool(&P), mListIdx(Idx) {
    assert(mPool->size() > mListIdx && mListIdx >= 0 &&
      "Index is out of range!");
  }

  /// Returns a reference to the list.
  AmbiguousList & operator*() const { return (*mPool)[mListIdx]; }

  /// Returns a pointer to the list.
  AmbiguousList * operator->() const { return &operator*(); }

  /// Compares two lists.
  bool operator==(const AmbiguousRef &With) const noexcept {
    return mPool == With.mPool && mListIdx == With.mListIdx;
  }

  /// Compares two lists.
  bool operator!=(const AmbiguousRef &With) const noexcept {
    return !operator==(With);
  }
private:
  AmbigiousPool *mPool;
  size_t mListIdx;
};


/// \brief This tag uses to implement a sequence of memory locations which are
/// ordered by inclusion.
///
/// All locations in a chain starts at the same points and have the same lists
/// of ambiguous pointers. The first location in a chain has the smallest size,
/// each subsequent location has larger size.
struct Hierarchy {};

/// This tag uses to implement a sequence of memory locations which may alias.
struct Alias {};

/// This tag uses to implement a sequence of sibling nodes.
struct Sibling {};

/// This tag uses to implement a sequence of nodes which is treated as a pool.
struct Pool {};
}

namespace bcl {
/// This is a specialization of intrusive chain aims to access sequence of
/// estimate memory locations with the same bases.
template<> class Chain<tsar::EstimateMemory, tsar::Hierarchy> {
public:
  /// Implicit cast to a data type Ty.
  operator tsar::EstimateMemory & () noexcept {
    return static_cast<tsar::EstimateMemory &>(*this);
  }

  /// Implicit cast to a data type Ty.
  operator const tsar::EstimateMemory & () const noexcept {
    return static_cast<const tsar::EstimateMemory &>(*this);
  }

private:
  /// Sets a specified node as a next node for this one.
  void spliceNext(Chain *N);

  /// Sets a specified node as a next node for this one.
  void mergeNext(Chain *N);

  /// Sets a specified node as a previous node for this one.
  void splicePrev(Chain *N);

  /// Sets a specified node as a previous node for this one.
  void mergePrev(Chain *N);

  /// Returns a next node.
  tsar::EstimateMemory * getNext() {
    return const_cast<tsar::EstimateMemory *>(
      static_cast<const Chain<tsar::EstimateMemory, tsar::Hierarchy> *>(this)->getNext());
  };

  /// Returns a next node.
  const tsar::EstimateMemory * getNext() const;

  /// Returns a previous node.
  tsar::EstimateMemory * getPrev() {
    return const_cast<tsar::EstimateMemory *>(
      static_cast<const Chain<tsar::EstimateMemory,
        tsar::Hierarchy> *>(this)->getPrev());
  }

  /// Returns a previous node.
  const tsar::EstimateMemory * getPrev() const;

private:
  template<class EstimateMemory, class Hierarchy> friend struct ChainTraits;
  template<class EstimateMemory, class Hierarchy> friend struct ChainIterator;
};
}

namespace tsar {
/// \brief This proposes a representation of a memory location to present
/// results of data dependence analysis.
///
/// Different locations are joined into a sequence of memory locations which may
/// alias. All locations from alias sequence collapsed into the single alias
/// node into a program alias tree.
/// Each location is a part of estimate memory location tree. A parent in this
/// tree covers all its children. All locations in a tree with the same list of
/// ambiguous pointers which may refer these locations represents a hierarchy
/// sequence. Note that at runtime it can be investigated that this pointers
/// refer different memory but due to inaccurate alias analysis it might not be
/// determined by a static analysis.
class EstimateMemory :
  public llvm::ilist_node<EstimateMemory, llvm::ilist_tag<Alias>>,
  public llvm::ilist_node<EstimateMemory, llvm::ilist_tag<Sibling>>,
  public bcl::Chain<EstimateMemory, Hierarchy> {

  using ChildList =
    llvm::simple_ilist<EstimateMemory, llvm::ilist_tag<Sibling>>;
public:
  /// This type used to iterate over all ambiguous pointers.
  using ambiguous_iterator = AmbiguousRef::AmbiguousList::const_iterator;

  /// This type is used to iterate over all children of this location.
  using child_iterator = ChildList::iterator;

  /// This type is used to iterate over all children of this location.
  using const_child_iterator = ChildList::const_iterator;

  /// This type is used to calculates number of children of this location.
  using child_size = ChildList::size_type;

  /// Returns size of location in address units or
  /// llvm::MemoryLocation:: UnknownSize if the size is not known.
  uint64_t getSize() const noexcept { return mSize; }

  /// Returns true if size is known.
  bool isSized() const noexcept {
    return mSize != llvm::MemoryLocation::UnknownSize;
  }

  /// Returns the metadata nodes which describes the aliasing of the location,
  /// or null if there is no information or conflicting information.
  llvm::AAMDNodes getAAInfo() const {
    // If we have missing or conflicting AAInfo, return null.
    if (mAATags == llvm::DenseMapInfo<llvm::AAMDNodes>::getEmptyKey() ||
      mAATags == llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey())
      return llvm::AAMDNodes();
    return mAATags;
  }

  /// \brief Returns true if this location defines ambiguous memory,
  /// for example, due to multiple pointer assignment.
  ///
  /// Ambiguousness means that it is not known whether two pointers which
  /// represent this location are overlapped.
  /// If alias analysis was accurate then there are two different estimate
  /// memory locations in the following case:
  /// \code
  ///   int *P, X, Y;
  ///   P = &X; // *P represents the first location X
  ///   P = &Y; // *P represents the second location Y
  /// \endcode
  /// If X and Y may overlap then it will be single ambiguous location *P.
  bool isAmbiguous() const noexcept { return mAmbiguous->size() > 1; }

  /// Returns first of possible pointers which may point to this location.
  const llvm::Value * front() const {
    assert(!mAmbiguous->empty() &&
      "List of ambiguous pointers must not be empty");
    return *begin();
  }

  /// Returns iterator that points to the beginning of the ambiguous pointers.
  ambiguous_iterator begin() const { return mAmbiguous->begin(); }

  /// Returns iterator that points to the ending of the ambiguous pointers.
  ambiguous_iterator end() const { return mAmbiguous->end(); }

  /// \brief Update the metadata nodes which describes the aliasing of the
  /// location.
  ///
  /// If there is conflict between metadata nodes the aliasing information
  /// will be dropped.
  void updateAAInfo(const llvm::AAMDNodes &AAInfo) {
    if (mAATags == llvm::DenseMapInfo<llvm::AAMDNodes>::getEmptyKey())
      // Aliasing information has not been set yet.
      mAATags = AAInfo;
    else if (mAATags != AAInfo)
      // Metadata nodes are in conflict state.
      mAATags = llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey();
  }

  /// Return true if an alias node has been already specified.
  bool hasAliasNode() const noexcept { return mNode != nullptr; }

  /// \brief Returns a node in alias graph which contains this location.
  ///
  /// This is not a thread-safe method.
  /// This uses union-find algorithm to search real alias node for the location.
  AliasNode * getAliasNode(const AliasTree &G) {
    return const_cast<AliasNode *>(
      static_cast<const EstimateMemory *>(this)->getAliasNode(G));
  }

  /// \brief Returns a node in alias graph which contains this location.
  ///
  /// This is not a thread-safe method.
  /// This uses union-find algorithm to search real alias node for the location.
  const AliasNode * getAliasNode(const AliasTree &G) const;

  /// Returns true if this location and a specified one have the same bases.
  bool isSameBase(const EstimateMemory &EM) const noexcept {
    return mAmbiguous == EM.mAmbiguous;
  }

  /// Returns parent.
  EstimateMemory * getParent() noexcept { return mParent; }

  /// Returns parent.
  const EstimateMemory * getParent() const noexcept { return mParent; }

  /// Returns top level memory location in the estimate memory tree.
  EstimateMemory * getTopLevelParent() noexcept {
    return const_cast<EstimateMemory *>(
      static_cast<const EstimateMemory *>(this)->getTopLevelParent());
  }

  /// Returns top level memory location in the estimate memory tree.
  const EstimateMemory * getTopLevelParent() const noexcept {
    auto Current = this;
    while (auto Parent = Current->getParent())
      Current = Parent;
    return Current;
  }

  /// Returns iterator that points to the beginning of the children list.
  child_iterator child_begin() { return mChildren.begin(); }

  /// Returns iterator that points to the ending of the children list.
  child_iterator child_end() { return mChildren.end(); }

  /// Returns iterator that points to the beginning of the children list.
  const_child_iterator child_begin() const { return mChildren.begin(); }

  /// Returns iterator that points to the ending of the children list.
  const_child_iterator child_end() const { return mChildren.end(); }

  /// Returns number of children in linear time.
  child_size getChildNumber() const { return mChildren.size(); }

  /// Returns true if this memory is a leaf in the estimate memory tree
  /// (in constant time).
  bool isLeaf() const { return mChildren.empty(); }

private:
  /// Creates an estimate location for a specified memory.
  ///
  /// \pre The second parameter specifies a list of ambiguous pointer which
  /// refer this location. It should be the same for all locations in hierarchy
  /// sequence but locations in unambiguous and alias sequences should have
  /// different lists.
  EstimateMemory(llvm::MemoryLocation &&Loc, AmbiguousRef &&AL) :
    mSize(std::move(Loc.Size)), mAATags(std::move(Loc.AATags)),
    mAmbiguous(AL) {
    assert(Loc.Ptr && "Pointer to a memory must not be null!");
    mAmbiguous->push_back(std::move(Loc.Ptr));
  }

  /// Creates an estimate location for a specified memory.
  ///
  /// \pre The second parameter specifies a list of ambiguous pointer which
  /// refer this location. It should be the same for all locations in hierarchy
  /// sequence but locations in unambiguous and alias sequences should have
  /// different lists.
  EstimateMemory(const llvm::MemoryLocation &Loc, AmbiguousRef &&AL) :
    mSize(Loc.Size), mAATags(Loc.AATags), mAmbiguous(AL) {
    assert(Loc.Ptr && "Pointer to a memory must not be null!");
    mAmbiguous->push_back(Loc.Ptr);
  }

  /// Creates an out of tree copy of a specified location.
  EstimateMemory(const EstimateMemory &EM) :
    mSize(EM.mSize), mAATags(EM.mAATags), mAmbiguous(EM.mAmbiguous) { }

  /// Creates an out of tree copy of a specified location with a new size
  /// and metadata node.
  EstimateMemory(const EstimateMemory &EM, uint64_t Size,
      const llvm::AAMDNodes &AATags) : EstimateMemory(EM) {
    mSize = Size;
    mAATags = AATags;
  }

  /// Returns list of ambiguous pointers which refer this location.
  const AmbiguousRef & getAmbiguousList() noexcept { return mAmbiguous; }

  /// Add this location to a specified node `N` in alias tree.
  void setAliasNode(AliasNode &N, const AliasTree &G);

  friend class AliasTree;
  friend class bcl::Chain<EstimateMemory, Hierarchy>;

  uint64_t mSize;
  llvm::AAMDNodes mAATags;
  AmbiguousRef mAmbiguous;
  mutable AliasNode *mNode = nullptr;
  EstimateMemory *mParent = nullptr;
  ChildList mChildren;
};

/// This represents node in an alias tree which refers an alias sequence of
/// estimate memory locations.
class AliasNode :
  public llvm::ilist_node<AliasNode, llvm::ilist_tag<Pool>,
  llvm::ilist_sentinel_tracking<true>>,
  public llvm::ilist_node<AliasNode, llvm::ilist_tag<Sibling>> {

  using ChildList = llvm::simple_ilist<AliasNode, llvm::ilist_tag<Sibling>>;
  using AliasList = llvm::simple_ilist<EstimateMemory, llvm::ilist_tag<Alias>>;

public:
  /// This type is used to iterate over all alias memory locations in this node.
  using iterator = AliasList::iterator;

  /// This type is used to iterate over all alias memory locations in this node.
  using const_iterator = AliasList::const_iterator;

  /// This type is used to iterate over all children of this node.
  using child_iterator = ChildList::iterator;

  /// This type is used to iterate over all children of this node.
  using const_child_iterator = ChildList::const_iterator;

  AliasNode(const AliasNode &) = delete;
  AliasNode(AliasNode &&) = delete;
  AliasNode & operator=(const AliasNode &) = delete;
  AliasNode & operator=(AliasNode &&) = delete;

  /// Returns iterator that points to the beginning of the alias list.
  iterator begin() { return mAliases.begin(); }

  /// Returns iterator that points to the beginning of the alias list.
  const_iterator begin() const { mAliases.begin(); }

  /// Returns iterator that points to the ending of the alias list.
  iterator end() { return mAliases.end(); }

  /// Returns iterator that points to the ending of the alias list.
  const_iterator end() const { return mAliases.end(); }

  /// Returns true if the node does not contain memory locations.
  bool empty() const noexcept { return mAliases.empty(); }

  /// Returns parent of the node.
  AliasNode * getParent(const AliasTree &G) {
    return const_cast<AliasNode *>(
      static_cast<const AliasNode *>(this)->getParent(G));
  }

  /// Returns parent of the node.
  const AliasNode * getParent(const AliasTree &G) const;

  /// Returns iterator that points to the beginning of the children list.
  child_iterator child_begin() { return mChildren.begin(); }

  /// Returns iterator that points to the ending of the children list.
  child_iterator child_end() { return mChildren.end(); }

  /// Returns iterator that points to the beginning of the children list.
  const_child_iterator child_begin() const { return mChildren.begin(); }

  /// Returns iterator that points to the ending of the children list.
  const_child_iterator child_end() const { return mChildren.end(); }

  /// Merges two nodes with a common parent or an immediate child into a parent.
  void mergeNodeIn(AliasNode &AN, const AliasTree &G) {
    assert(!AN.mForward && "Alias node is already forwarding!");
    assert(!mForward && "This set is a forwarding node!");
    assert(AN.getParent(G) == getParent(G) || AN.getParent(G) == this &&
      "Only nodes with a common parent or an immediate child in a parent can be merged!");
    assert(&AN != this && "Alias node can not be merged with itself!");
    AN.mForward = this;
    retain();
    mChildren.splice(mChildren.end(), AN.mChildren);
    mAliases.splice(mAliases.end(), AN.mAliases);
    AN.getParent(G)->mChildren.erase(child_iterator(AN));
  }

  /// \brief Returns true if this node should be ignored as a part of the graph
  /// due to this node has been merged with some other node.
  ///
  /// Delayed removal of memory allocated for the node will be performed.
  bool isForwarding() const noexcept { return mForward; }

  /// \brief Return the real alias node this represents.
  ///
  /// If this has been merged with another node and is forwarding,
  /// return the ultimate destination node. Intermediate nodes will be released.
  /// This is not a thread-safe method.
  /// This uses union-find algorithm to search real alias node.
  AliasNode *getForwardedTarget(const AliasTree &G) const {
    if (!isForwarding()) return const_cast<AliasNode *>(this);
    AliasNode *Dest = mForward->getForwardedTarget(G);
    if (Dest != mForward) {
      Dest->retain();
      mForward->release(G);
      mForward = Dest;
    }
    return Dest;
  }
private:
  friend AliasTree;
  friend EstimateMemory;

  /// Creates an empty node.
  AliasNode() {};

  /// Specifies a parent for this node.
  void setParent(AliasNode &Parent, const AliasTree &G) {
    if (mParent) {
      mParent->mChildren.erase(child_iterator(this));
      mParent->release(G);
    }
    mParent = &Parent;
    mParent->mChildren.push_back(*this);
    mParent->retain();
  }

  /// Inserts new estimate memory location at the end of memory sequence.
  void push_back(EstimateMemory &EM) { mAliases.push_back(EM); }

  /// Increases number of references to this node.
  void retain() const noexcept { ++mRefCount; }


  /// Decreases a number of references to this node and remove it from a
  /// specified graph if there is no references any more.
  void release(const AliasTree &G) const;

  mutable AliasNode *mParent = nullptr;
  ChildList mChildren;
  AliasList mAliases;
  mutable AliasNode *mForward = nullptr;
  mutable unsigned mRefCount = 0;
};

class AliasTree {
  /// \brief This chain represents hierarchy of base locations.
  ///
  /// Note, tsar::EstimateMemory class inherits bcl::Chain template,
  /// so it is possible to treat each location as a node in some list.
  using BaseChain = EstimateMemory *;

  /// List of bases contains normally 1 element, it is useful to access
  /// different bases with the same stripped pointer, which are stored in a map.
  using BaseList = llvm::TinyPtrVector<BaseChain>;

  /// Map from stripped pointers which address base locations to a list of
  /// memory location chains.
  using StrippedMap = llvm::DenseMap<const llvm::Value *, BaseList>;

  /// Pool to store pointers to all alias nodes, including forwarding.
  using AliasNodePool = llvm::ilist<AliasNode,
    llvm::ilist_tag<Pool>, llvm::ilist_sentinel_tracking<true>>;

  /// This is used to iterate over all nodes in tree excluding forwarding.
  template<class ItrTy> class iterator_imp {
    template<class RHSItrTy> friend class iterator_imp;
  public:
    typedef typename ItrTy::value_type value_type;
    typedef typename ItrTy::pointer pointer;
    typedef typename ItrTy::reference reference;
    typedef typename ItrTy::difference_type difference_type;
    typedef typename ItrTy::iterator_category iterator_category;

    explicit iterator_imp(const ItrTy &I) : mCurrItr(I) {
      for (; !mCurrItr.isEnd() && mCurrItr->isForwarding(); ++mCurrItr);
    }

    /// This allows constructing of const iterator from nonconst one, note
    /// that RHSItrTy must allow such conversion to ItrTy.
    template<class RHSItrTy>
    iterator_imp(const iterator_imp<RHSItrTy> &RHS) : mCurrItr(RHS.mCurrItr) {};

    /// This allows assignment of const iterator from nonconst one, note
    /// that RHSItrTy must allow such conversion to ItrTy.
    template<class RHSItrTy>
    iterator_imp & operator=(const iterator_imp<RHSItrTy> &RHS) {
      mCurrItr = RHS.mCurrItr;
    }

    bool operator==(const iterator_imp &RHS) const noexcept {
      return mCurrItr == RHS.mCurrItr;
    }
    bool operator!=(const iterator_imp &RHS) const noexcept {
      return !operator==(RHS);
    }

    reference operator*() const noexcept { return *mCurrItr; }
    pointer operator->() const noexcept { return &operator*(); }

    iterator_imp & operator++() { // Preincrement
      for (++mCurrItr; !mCurrItr.isEnd() && mCurrItr->isForwarding();
        ++mCurrItr);
      return *this;
    }
    iterator_imp operator++(int) { // Postincrement
      iterator Tmp = *this; ++*this; return Tmp;
    }
  private:
    ItrTy mCurrItr;
  };

public:
  /// This is used to iterate over all nodes in tree excluding forwarding.
  using iterator = iterator_imp<AliasNodePool::iterator>;

  /// This is used to iterate over all nodes in tree excluding forwarding.
  using const_iterator = iterator_imp<AliasNodePool::const_iterator>;

  /// Size of an alias tree.
  using size_type = AliasNodePool::size_type;

  /// Creates empty alias tree.
  AliasTree(llvm::AAResults &AA, const llvm::DataLayout &DL) :
    mAA(&AA), mDL(&DL), mTopLevelNode(new AliasNode) {
    mNodes.push_back(mTopLevelNode);
  }

  /// Destroys alias tree.
  ~AliasTree() {
    for (auto &Pair : mBases) {
      for (auto *EM : Pair.second)
        delete EM;
    }
  }

  /// Returns root of the alias tree.
  AliasNode * getTopLevelNode() noexcept { return mTopLevelNode; }

  /// Returns root of the alias tree.
  const AliasNode * getTopLevelNode() const noexcept { return mTopLevelNode; }

  /// Returns iterator that points to the beginning of the node list,
  /// forwarding nodes will not be traversed.
  iterator begin() { return iterator(mNodes.begin()); }

  /// Returns iterator that points to the ending of the node list,
  /// forwarding nodes will not be traversed.
  iterator end() { return iterator(mNodes.end()); }

  /// Returns iterator that points to the beginning of the node list,
  /// forwarding nodes will not be traversed.
  const_iterator begin() const { return const_iterator(mNodes.begin()); }

  /// Returns iterator that points to the ending of the node list,
  /// forwarding nodes will not be traversed.
  const_iterator end() const { return const_iterator(mNodes.end()); }

  /// Returns true if this alias tree is empty.
  bool empty() const { return mNodes.empty(); }

  /// Returns number of nodes, including forwarding.
  size_type size() const { return mNodes.size(); }

  /// Inserts new estimate memory location.
  void add(const llvm::Value *Ptr,
    uint64_t Size, const llvm::AAMDNodes &AAInfo) {
    return add(llvm::MemoryLocation(Ptr, Size, AAInfo));
  }

  /// Inserts new estimate memory location.
  void add(const llvm::MemoryLocation &Loc);

  /// Removes node from the graph, note that this class manages memory
  /// allocation to store nodes.
  void removeNode(AliasNode *N);

  /// Returns the smallest estimate memory location which covers a specified
  /// memory location or nullptr.
  const EstimateMemory * find(const llvm::MemoryLocation &Loc) const;

  /// Returns the smallest estimate memory location which covers a specified
  /// memory location or nullptr.
  EstimateMemory * find(const llvm::MemoryLocation &Loc) {
    return const_cast<EstimateMemory *>(
      static_cast<const AliasTree *>(this)->find(Loc));
  }

  /// \brief This pop up ghostview window and displays the alias tree.
  ///
  /// This depends on there being a 'dot' and 'gv' program in a system path.
  void view() const;

  /// This works like view() but this does not display ambiguous pointers. This
  /// show name of base locations and their sizes only.
  void viewOnly() const;

private:
  /// Performs depth-first search of a new node insertion point
  /// and insert a new empty node, if it is necessary.
  ///
  /// \param [in] NewEm Estimate memory location which is used to determine
  /// node insertion point.
  /// \param [in] Start Start node for search.
  /// \return Alias node that should refer to a specified memory `NewEM`.
  /// This method may merge some nodes or return already existing one.
  AliasNode * addEmptyNode(const EstimateMemory &NewEM, AliasNode &Current);

  /// Checks whether pointers to specified locations may refer the same address.
  llvm::AliasResult isSamePointer(
    const EstimateMemory &EM, const llvm::MemoryLocation &Loc) const;

  /// \brief Checks whether specified estimate locations may alias each other.
  ///
  /// This method is potentially slow because in the worst cast it uses
  /// AAResults::alias() method to compare all possible pairs of ambiguous
  /// pointers.
  bool slowMayAlias(const EstimateMemory &LHS, const EstimateMemory &RHS) const;

  /// Inserts a new base location into the stripped map or update existing ones.
  ///
  /// If a base location for the specified location contains some base location
  /// in this set, an appropriate hierarchy sequence of locations will be
  /// updated. For example, the following expressions *(short*)P and *P where P
  /// has type int have base locations started at *P with different sizes.
  /// When *(short*)P will be evaluated the result will be *P with size
  /// size_of(short). When *P will be evaluated the result will be *P with
  /// size size_of(int). These two results will be chained and sorted from
  /// smaller to greater size.
  /// If size is unknown it will be set to llvm::MemoryLocation::UnknownSize.
  /// \return A tuple which contains following values will be returned:
  /// - Estimate memory location which is most accurately represent a specified
  ///   location `Loc`. Pointer for base location will never be set to nullptr.
  /// - `true` if a new estimate memory location has been created.
  /// - `true` if a specified location `Loc` add ambiguousness to existed
  ///   hierarchy sequence. The first and second value may be set to true both.
  std::tuple<EstimateMemory *, bool, bool>
    insert(const llvm::MemoryLocation &Loc);

  llvm::AAResults *mAA;
  const llvm::DataLayout *mDL;
  AliasNodePool mNodes;
  AliasNode *mTopLevelNode;
  tsar::AmbiguousRef::AmbigiousPool mAmbiguousPool;
  StrippedMap mBases;
};

inline void EstimateMemory::setAliasNode(AliasNode &N, const AliasTree &G) {
  if (mNode)
    mNode->release(G);
  mNode = &N;
  mNode->push_back(*this);
  mNode->retain();
}

inline void AliasNode::release(const AliasTree &G) const {
  if (--mRefCount == 0)
    const_cast<AliasTree &>(G).removeNode(const_cast<AliasNode *>(this));
}
}

inline const tsar::EstimateMemory *
bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy>::getNext() const {
  auto &Chain = static_cast<const tsar::EstimateMemory &>(*this);
  auto Next = Chain.getParent();
  return Next && Chain.isSameBase(*Next) ? Next : nullptr;
}

inline const tsar::EstimateMemory *
bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy>::getPrev() const {
  auto &Chain = static_cast<const tsar::EstimateMemory &>(*this);
  auto Prev = Chain.child_begin();
  return Prev != Chain.child_end() && Chain.isSameBase(*Prev) ?
    &*Prev : nullptr;
}

inline void bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy>::spliceNext(
    bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy> *N) {
  assert(N != this && "A node must not follow itself!");
  auto Chain = static_cast<tsar::EstimateMemory *>(this);
  auto Next = static_cast<tsar::EstimateMemory *>(N);
  if (Chain->getParent() == Next)
    return;
  if (Next) {
    Next->mParent = Chain->mParent;
    Next->mChildren.push_back(*Chain);
  }
  if (Chain->mParent) {
    Chain->mParent->mChildren.remove(*Chain);
    if (Next)
      Chain->mParent->mChildren.push_back(*Next);
  }
  Chain->mParent = Next;
}

inline void bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy>::mergeNext(
    bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy> *N) {
  assert(N != this && "A node must not follow itself!");
  auto Chain = static_cast<tsar::EstimateMemory *>(this);
  auto Next = static_cast<tsar::EstimateMemory *>(N);
  if (Chain->getParent() == Next)
    return;
  if (Next)
    Next->mChildren.push_back(*Chain);
  if (Chain->mParent)
    Chain->mParent->mChildren.remove(*Chain);
  Chain->mParent = Next;
}

inline void bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy>::splicePrev(
    bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy> *N) {
  assert(N != this && "A node must not precede itself!");
  auto Chain = static_cast<tsar::EstimateMemory *>(this);
  auto Prev = static_cast<tsar::EstimateMemory *>(N);
  if (Prev && Prev->getParent() == Chain)
    return;
  for (auto &Child : Chain->mChildren)
    Child.mParent = Prev;
  if (Prev) {
    Prev->mChildren.splice(Prev->mChildren.end(), Chain->mChildren);
    Prev->mParent = Chain;
    Chain->mChildren.push_back(*Prev);
  }
}

inline void bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy>::mergePrev(
    bcl::Chain<tsar::EstimateMemory, tsar::Hierarchy> *N) {
  assert(N != this && "A node must not precede itself!");
  auto Chain = static_cast<tsar::EstimateMemory *>(this);
  auto Prev = static_cast<tsar::EstimateMemory *>(N);
  if (Prev && Prev->getParent() == Chain)
    return;
  for (auto &Child : Chain->mChildren)
    Child.mParent = nullptr;
  if (Prev) {
    Prev->mParent = Chain;
    Chain->mChildren.clear();
    Chain->mChildren.push_back(*Prev);
  }
}
namespace llvm {
//===----------------------------------------------------------------------===//
// GraphTraits specializations for estimate memory tree (EstimateMemory)
//===----------------------------------------------------------------------===//

template<> struct GraphTraits<tsar::EstimateMemory *> {
  using NodeRef = tsar::EstimateMemory *;
  static NodeRef getEntryNode(tsar::EstimateMemory *N) noexcept {
    return N;
  }
  using ChildIteratorType =
    pointer_iterator<tsar::EstimateMemory::child_iterator>;
  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->child_begin());
  }
  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->child_end());
  }
};

template<> struct GraphTraits<const tsar::EstimateMemory *> {
  using NodeRef = const tsar::EstimateMemory *;
  static NodeRef getEntryNode(const tsar::EstimateMemory *N) noexcept {
    return N;
  }
  using ChildIteratorType =
    pointer_iterator<tsar::EstimateMemory::const_child_iterator>;
  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->child_begin());
  }
  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->child_end());
  }
};
//===----------------------------------------------------------------------===//
// GraphTraits specializations for alias tree (AliasTree)
//===----------------------------------------------------------------------===//

template<> struct GraphTraits<tsar::AliasNode *> {
  using NodeRef = tsar::AliasNode *;
  static NodeRef getEntryNode(tsar::AliasNode *AN) noexcept {
    return AN;
  }
  using ChildIteratorType =
    pointer_iterator<tsar::AliasNode::child_iterator>;
  static ChildIteratorType child_begin(NodeRef AN) {
    return ChildIteratorType(AN->child_begin());
  }
  static ChildIteratorType child_end(NodeRef AN) {
    return ChildIteratorType(AN->child_end());
  }
};

template<> struct GraphTraits<const tsar::AliasNode *> {
  using NodeRef = const tsar::AliasNode *;
  static NodeRef getEntryNode(const tsar::AliasNode *AN) noexcept {
    return AN;
  }
  using ChildIteratorType =
    pointer_iterator<tsar::AliasNode::const_child_iterator>;
  static ChildIteratorType child_begin(NodeRef AN) {
    return ChildIteratorType(AN->child_begin());
  }
  static ChildIteratorType child_end(NodeRef AN) {
    return ChildIteratorType(AN->child_end());
  }
};

template<> struct GraphTraits<tsar::AliasTree *> :
    public GraphTraits<tsar::AliasNode *> {
  static NodeRef getEntryNode(tsar::AliasTree *AT) noexcept {
    return AT->getTopLevelNode();
  }
  using nodes_iterator = pointer_iterator<tsar::AliasTree::iterator>;
  static nodes_iterator nodes_begin(tsar::AliasTree *AT) {
    return nodes_iterator(AT->begin());
  }
  static nodes_iterator nodes_end(tsar::AliasTree *AT) {
    return nodes_iterator(AT->end());
  }
  static unsigned size(tsar::AliasTree *AT) { return AT->size(); }
};

template<> struct GraphTraits<const tsar::AliasTree *> :
    public GraphTraits<const tsar::AliasNode *> {
  static NodeRef getEntryNode(const tsar::AliasTree *AT) noexcept {
    return AT->getTopLevelNode();
  }
  using nodes_iterator = pointer_iterator<tsar::AliasTree::const_iterator>;
  static nodes_iterator nodes_begin(const tsar::AliasTree *AT) {
    return nodes_iterator(AT->begin());
  }
  static nodes_iterator nodes_end(const tsar::AliasTree *AT) {
    return nodes_iterator(AT->end());
  }
  static unsigned size(const tsar::AliasTree *AT) { return AT->size(); }
};


/// This per-function analysis pass build hierarchy of a whole memory which
/// is used in an analyzed function.
class EstimateMemoryPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  EstimateMemoryPass() : FunctionPass(ID) {
    initializeEstimateMemoryPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns alias tree for the last analyzed function.
  tsar::AliasTree & getAliasTree() {
    assert(mAliasTree && "Alias tree has not been constructed yet!");
    return *mAliasTree;
  }

  /// Returns alias tree for the last analyzed function.
  const tsar::AliasTree & getAliasTree() const {
    assert(mAliasTree && "Alias tree has not been constructed yet!");
    return *mAliasTree;
  }

  /// Build hierarchy of accessed memory for a specified function.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases memory.
  void releaseMemory() override {
    if (mAliasTree) {
      delete mAliasTree;
      mAliasTree = nullptr;
    }
  }

private:
  tsar::AliasTree *mAliasTree = nullptr;
};
}
#endif//TSAR_ESTIMATE_MEMORY_H
