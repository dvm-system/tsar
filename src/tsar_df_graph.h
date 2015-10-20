//===------- tsar_df_loop.h - Represent a data-flow graph ------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===--------------------------------------------------------------------===//
//
// This file defines functions and classes to represent a data-flow graph.
// The graph could be used in a data-flow framework to solve data-flow problem.
// In some cases it is convinient to use hierarchy of nodes. Some nodes are
// treated as regions which contain other nodes. LLVM-style RTTI for hierarch
// of classes that represented different nodes is avaliable.
//
// There are following main elements in this file:
// * Classes which is used to represent nodes and regions in a data-flow graph.
// * Functions, to build hierarchy of regions.
//===--------------------------------------------------------------------===//

#ifndef TSAR_DF_GRAPH_H
#define TSAR_DF_GRAPH_H

#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/DenseMap.h>
#include "llvm/Support/Casting.h"
#include <vector>
#include <utility.h>
#include <declaration.h>
#include "tsar_data_flow.h"
#include "tsar_graph.h"

namespace llvm {
class Function;
class LoopInfo;
class Loop;
class BasicBlock;
}

namespace tsar {
/// \brief Representation of a node in a data-flow framework.
///
/// The following kinds of nodes are supported: basic block, body of a
/// natural loop, body of a function entry point of the graph which will be
/// analyzed. LLVM-style RTTI for hierarch of classes that represented
/// different nodes is avaliable.
/// \par In some cases it is convinient to use hierarchy of nodes. Some nodes
/// are treated as regions which contain other nodes. Such regions we call
/// parent nodes.
class DFNode : public tsar::SmallDFNode<DFNode, 8> {
public:
  /// Kind of a node.
  /// If you add a new kind of region it should be in the range between
  /// FIRST_KIND_REGION and LAST_KIND_REGION
  enum Kind {
    FIRST_KIND = 0,
    KIND_BLOCK = FIRST_KIND,
    KIND_ENTRY,

    FIRST_KIND_REGION,
    KIND_LOOP = FIRST_KIND_REGION,
    KIND_FUNCTION,
    LAST_KIND_REGION = KIND_FUNCTION,

    LAST_KIND = LAST_KIND_REGION,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND,
  };

  /// Desctructor.
  virtual ~DFNode() {
#ifdef DEBUG
    mKind = INVALID_KIND;
    mParent = nullptr;
#endif
  }

  /// Returns the kind of the region.
  Kind getKind() const { return mKind; }

  /// Returns a parent node.
  DFNode * getParent() { return mParent; }

  /// Returns a parent node.
  const DFNode * getParent() const { return mParent; }

  /// \brief Adds a new attribute to the node.
  ///
  /// \tparam Attribute which has been declared using macros
  /// BASE_ATTR_DEF(name_, type_).
  /// \param [in] V Value of the attribute.
  /// \return If the attribute already exist it can not be added, so this
  /// function returns false. Otherwise, it returns true.
  template<class Attribute>
  bool addAttribute(typename Attribute::Value *V) {
    return mAttributes.insert(
      std::make_pair(Attribute::id(), static_cast<void *>(V))).second;
  }

  /// \brief Returns a value of the attribute or null if it does not exist.
  ///
  /// \tparam Attribute which has been declared using macros
  /// BASE_ATTR_DEF(name_, type_).
  /// \return Value of the specified attribute. If the attribute
  /// does not exist the method returns nullptr.
  template<class Attribute>
  typename Attribute::Value * getAttribute() {
    llvm::DenseMap<Utility::AttributeId, void *>::iterator I =
      mAttributes.find(Attribute::id());
    return I == mAttributes.end() ? nullptr :
      static_cast<typename Attribute::Value *>(I->second);
  }

  /// \brief Removes an attribute from the node.
  ///
  /// \return The value of the removed attribute. If the attribute
  /// does not exist the method returns nullptr.
  template<class Attribute>
  typename Attribute::Value * removeAttribute() {
    llvm::DenseMap<Utility::AttributeId, void *>::iterator I =
      mAttributes.find(Attribute::id());
    if (I == mAttributes.end())
      return nullptr;
    typename Attribute::Value *V =
      static_cast<typename Attribute::Value *>(I->second);
    mAttributes.erase(I);
    return V;
  }

protected:
  /// Creates a new node of the specified type.
  explicit DFNode(Kind K) : mKind(K), mParent(nullptr) {}

private:
  friend class DFRegion;
  Kind mKind;
  DFNode *mParent;
  llvm::DenseMap<Utility::AttributeId, void *> mAttributes;
};

/// Representation of an entry node in a data-flow framework.
class DFEntry : public DFNode {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DFNode *R) {
    return R->getKind() == KIND_ENTRY;
  }

  /// \brief Ctreates representation of the entry node.
  DFEntry() : DFNode(KIND_ENTRY) {}
};

/// \brief Representation of a region in a data-flow framework.
///
/// In some cases it is convinient to use hierarchy of nodes. Some nodes
/// are treated as regions which contain other nodes.
/// LLVM-style RTTI for hierarch of classes that represented different regions
/// is avaliable.
class DFRegion : public DFNode {
public:

  /// This type used to iterate over all nodes in the region body.
  typedef std::vector<DFNode *>::const_iterator node_iterator;

  /// This type used to iterate over internal regions.
  typedef std::vector<DFRegion *>::const_iterator region_iterator;

  /// This type used to iterate over all latch nodes in the loop body.
  typedef llvm::SmallPtrSet<DFNode *, 8>::const_iterator latch_iterator;

  /// This type used to iterate over all exiting nodes in the region.
  typedef llvm::SmallPtrSet<DFNode *, 8>::const_iterator exiting_iterator;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DFNode *N) {
    return FIRST_KIND_REGION <= N->getKind() &&
      N->getKind() <= LAST_KIND_REGION;
  }

  /// \brief Deletes all nodes in the region.
  ///
  /// A memory which was allocated for the nodes is freed.
  ~DFRegion() {
    for (DFNode *N : mNodes)
      delete N;
    delete mEntry;
  }

  /// Get the number of nodes in this region.
  unsigned getNumNodes() const { return mNodes.size(); }

  /// Get a list of the nodes which make up this region body.
  const std::vector<DFNode *> & getNodes() const { return mNodes; }

  /// Returns iterator that points to the beginning of the nodes list.
  node_iterator node_begin() const { return mNodes.begin(); }

  /// Returns iterator that points to the ending of the nodes list.
  node_iterator node_end() const { return mNodes.end(); }

  /// Get the number of internal regions.
  unsigned getNumRegions() const { return mRegions.size(); }

  // Get a list of internal regions.
  const std::vector<DFRegion *> & getRegions() const { return mRegions; }

  /// Returns iterator that points to the beginning of the internal regions.
  region_iterator region_begin() const { return mRegions.begin(); }

  /// Returns iterator that points to the ending of the internal regions.
  region_iterator region_end() const { return mRegions.end(); }

  /// \brief Returns the entry-point of the data-flow graph.
  ///
  /// The result of this method is an entry point which is necessary to solve
  /// a data-flow problem. A node which is treated as entry depends on a region
  /// and it might not be essential in an original data-flow graph.
  /// For example, in case of loop, the entry node is not a header of the loop,
  /// this node is a predecessor of the header.
  /// \attention This node should not contained in  list of nodes which is
  /// a result of the getNodes() method. 
  DFNode * getEntryNode() const {
    assert(mEntry && "There is no entry node in the graph!");
    return mEntry;
  }

  /// \brief Specifies an exiting node of the data-flow graph.
  ///
  /// Multiple nodes can be specified.
  void setExitingNode(DFNode *N) {
    assert(N && "Node must not be null!");
    mExitingNodes.insert(N);
  }

  /// Get a list of the exiting nodes of this region.
  const llvm::SmallPtrSet<DFNode *, 8> & getExitingNodes() const {
    return mExitingNodes;
  }

  /// Returns iterator that points to the beginning of the exiting nodes list.
  exiting_iterator exiting_begin() const { return mExitingNodes.begin(); }

  /// Returns iterator that points to the ending of the exiting nodes list.
  exiting_iterator exiting_end() const { return mExitingNodes.end(); }

  ///\brief Returns true if the node is an exiting node of this region.
  ///
  /// Exiting node is a node which is inside of the region and 
  /// have successors outside of the region.
  bool isExiting(const DFNode *N) const {
    return mExitingNodes.count(const_cast<DFNode *>(N));
  }

  /// \brief Specifies an latch node of the data-flow graph.
  ///
  /// Multiple nodes can be specified.
  void setLatchNode(DFNode *N) {
    assert(N && "Node must not be null!");
    mLatchNodes.insert(N);
  }

  /// Get a list of the latch nodes of this loop.
  const llvm::SmallPtrSet<DFNode *, 8> & getLatchNodes() const {
    return mLatchNodes;
  }

  /// Returns iterator that points to the beginning of the latch nodes list.
  latch_iterator latch_begin() const { return mLatchNodes.begin(); }

  /// Returns iterator that points to the ending of the latch nodes list.
  latch_iterator latch_end() const { return mLatchNodes.end(); }

  ///\brief  Returns true if the node is an latch node of this loop.
  ///
  /// A latch node is a node that contains a branch back to the header.
  bool isLatch(const DFNode *N) const {
    return mLatchNodes.count(const_cast<DFNode *>(N));
  }

  /// \brief Inserts a new node at the end of the list of nodes.
  ///
  /// \attention The inserted node falls under the control of the region and
  /// will be destroyed at the same time when the region will be destroyed.
  /// \pre
  /// - A new node can not take a null value.
  /// - The node should be differ from other nodes of the graph.
  void addNode(DFNode *N) {
    assert(N && "Node must not be null!");
    assert(FIRST_KIND <= N->getKind() && N->getKind() <= LAST_KIND &&
      "Unknown kind of a node!");
    assert(N != mEntry && "Only one entry node must be in the region!");
#ifdef DEBUG
    for (DFNode *Node : mNodes)
      assert(N != Node &&
        "The node must not be contained in the region!");
#endif
    N->mParent = this;
    if (llvm::isa<DFEntry>(N)) {
      mEntry = N;
      return;
    }
    mNodes.push_back(N);
    if (DFRegion *R = llvm::dyn_cast<DFRegion>(N))
      mRegions.push_back(R);
  }
protected:
  /// Creates a new node of the specified type.
  explicit DFRegion(Kind K) : DFNode(K), mEntry(nullptr) {}

private:
  std::vector<DFNode *> mNodes;
  std::vector<DFRegion *> mRegions;
  llvm::SmallPtrSet<DFNode *, 8> mExitingNodes;
  llvm::SmallPtrSet<DFNode *, 8> mLatchNodes;
  DFNode *mEntry;
};

/// \brief Representation of a loop in a data-flow framework.
///
/// Instance of this class is used to represent abstraction of a loop
/// in data-flow framework. This class should be used only to solve
/// data-flow problem. The loop can be collapsed to one abstract node
/// to simplify the data-flow graph that contains this loop. If this loop
/// has inner loops they also can be collapsed. So the abstraction of this loop
/// can internally contain nodes of following types: basic block,
/// collapsed inner loop and entry node.
class DFLoop : public DFRegion {
public:

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DFNode *N) {
    return N->getKind() == KIND_LOOP;
  }

  /// \brief Creates representation of the loop.
  ///
  /// \pre The loop argument can not take a null value.
  explicit DFLoop(llvm::Loop *L) : DFRegion(KIND_LOOP), mLoop(L) {
    assert(L && "Loop must not be null!");
  }

  /// Get the loop.
  llvm::Loop * getLoop() const { return mLoop; }

private:
  llvm::Loop *mLoop;
};


/// \brief Representation of a basic block in a data-flow framework.
///
/// Instance of this class is used to represent abstraction of a basic block
/// in data-flow framework. This class should be used only to solve
/// data-flow problem.
class DFBlock : public DFNode {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DFNode *N) {
    return N->getKind() == KIND_BLOCK;
  }

  /// \brief Ctreates representation of the block.
  ///
  /// \pre The block argument can not take a null value.
  explicit DFBlock(llvm::BasicBlock *B) : DFNode(KIND_BLOCK), mBlock(B) {
    assert(B && "Block must not be null!");
  }

  /// Get the block.
  llvm::BasicBlock * getBlock() const { return mBlock; }

private:
  llvm::BasicBlock *mBlock;
};

/// \brief Representation of a function in a data-flow framework.
///
/// Instance of this class is used to represent abstraction of a function
/// in data-flow framework. A function is treated as region which may contain
/// basic blocks, collapsed inner loops and entry node.
/// This class should be used only to solve a data-flow problem.
class DFFunction : public DFRegion {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DFNode *N) {
    return N->getKind() == KIND_FUNCTION;
  }

  /// \brief Ctreates representation of the block.
  ///
  /// \pre The block argument can not take a null value.
  explicit DFFunction(llvm::Function *F) : DFRegion(KIND_FUNCTION), mFunc(F) {
    assert(F && "Function must not be null!");
  }

  /// Get the function.
  llvm::Function * getFunction() const { return mFunc; }
private:
  llvm::Function *mFunc;
};

/// This class should be specialized by different loops which is why
/// the default version is empty. The specialization is used to iterate over
/// blocks and internal loops which are part of a loop.
/// The following elements should be provided: typedef region_iterator,
/// - typedef block_iterator,
///   static block_iterator block_begin(LoopReptn &L),
///   static block_iterator block_end (LoopReptn &L) -
///     Allow iteration over all blocks in the specified loop.
/// - typedef loop_iterator,
///   static loop_iterator loop_begin(LoopReptn &L),
///   static loop_iterator loop_end (LoopReptn &L) -
///     Allow iteration over all internal loops in the specified loop.
template<class LoopReptn> class LoopTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename LoopReptn::UnknownLoopError loop_iterator;
};

template<> class LoopTraits<llvm::Loop *> {
public:
  typedef llvm::Loop::block_iterator block_iterator;
  typedef llvm::Loop::iterator loop_iterator;
  static block_iterator block_begin(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->block_begin();
  }
  static block_iterator block_end(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->block_end();
  }
  static llvm::BasicBlock * getHeader(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->getHeader();
  }
  static loop_iterator loop_begin(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->begin();
  }
  static loop_iterator loop_end(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->end();
  }
};

template<> class LoopTraits<std::pair<llvm::Function *, llvm::LoopInfo *>> {
  typedef std::pair<llvm::Function *, llvm::LoopInfo *> LoopReptn;
public:
  class block_iterator : public llvm::Function::iterator {
    // Let us use this iterator to access a list of blocks in a function
    // as a list of pointers. Originaly a list of blocks in a function is
    // implemented as a list of objects, not a list of pointers.
    typedef llvm::Function::iterator base;
  public:
    typedef pointer reference;
    block_iterator(reference R) : base(R) {}
    block_iterator() : base() {}
    reference operator*() const { return base::operator pointer(); }
  };
  typedef llvm::LoopInfo::iterator loop_iterator;
  static block_iterator block_begin(LoopReptn L) {
    return static_cast<block_iterator>(L.first->begin());
  }
  static block_iterator block_end(LoopReptn L) {
    return static_cast<block_iterator>(L.first->end());
  }
  static llvm::BasicBlock * getHeader(LoopReptn L) {
    return &L.first->getEntryBlock();
  }
  static loop_iterator loop_begin(LoopReptn L) {
    return L.second->begin();
  }
  static loop_iterator loop_end(LoopReptn L) {
    return L.second->end();
  }
};

/// \brief Builds hierarchy of regions for the specified loop nest.
///
/// This function treats a loop nest as hierarchy of regions. Each region is
/// an abstraction of an inner loop. Only natural loops will be treated as a
/// region other loops will be ignored.
/// \tparam LoopReptn Representation of the outermost loop in the nest.
/// The LoopTraits class should be specialized by type of each loop in the nest.
/// For example, the outermost loop can be a loop llvm::Loop * or
/// a whole funcction std::pair<llvm::Function *, llvm::LoopInfo *>.
/// \param [in] L An outermost loop in the nest, it can not be null.
/// \param [in, out] A region which is associated with the specified loop.
template<class LoopReptn> void buildLoopRegion(LoopReptn L, DFRegion *R) {
  assert(R && "Region must not be null!");
  typedef LoopTraits<LoopReptn> LT;
  DenseMap<BasicBlock *, DFNode *> Blocks;
  for (LT::loop_iterator I = LT::loop_begin(L), E = LT::loop_end(L);
       I != E; ++I) {
    DFLoop *DFL = new DFLoop(*I);
    buildLoopRegion(*I, DFL);
    R->addNode(DFL);
    for (BasicBlock *BB : (*I)->getBlocks())
      Blocks.insert(std::make_pair(BB, DFL));
  }
  for (LT::block_iterator I = LT::block_begin(L), E = LT::block_end(L);
       I != E; ++I) {
    if (Blocks.count(*I))
      continue;
    DFBlock * N = new DFBlock(*I);
    R->addNode(N);
    Blocks.insert(std::make_pair(*I, N));
  }
  assert(LT::getHeader(L) && Blocks.count(LT::getHeader(L)) &&
    "Data-flow node for the loop header is not found!");
  DFEntry *EntryNode = new DFEntry;
  R->addNode(EntryNode);
  DFNode *HeaderNode = Blocks.find(LT::getHeader(L))->second;
  EntryNode->addSuccessor(HeaderNode);
  HeaderNode->addPredecessor(EntryNode);
  for (auto BBToN : Blocks) {
    if (succ_begin(BBToN.first) == succ_end(BBToN.first))
      R->setExitingNode(BBToN.second);
    else
      for (succ_iterator SI = succ_begin(BBToN.first),
        SE = succ_end(BBToN.first); SI != SE; ++SI) {
        auto SToNode = Blocks.find(*SI);
        // First, exiting nodes will be specified.
        // Second, latch nodes will be specified. A latch node is a node
        // that contains a branch back to the header.
        // Third, successors will be specified:
        // 1. Back and exit edges will be ignored.
        // 2. Branches inside inner loops will be ignored.
        // There is branch from a data-flow node to itself
        // (SToNode->second == BBToN.second) only if this node is an abstraction
        // of an inner loop. So this branch is inside this inner loop
        // and should be ignored.
        if (SToNode == Blocks.end())
          R->setExitingNode(BBToN.second);
        else if (*SI == LT::getHeader(L))
          R->setLatchNode(BBToN.second);
        else if (SToNode->second != BBToN.second)
          BBToN.second->addSuccessor(SToNode->second);
      }
    // Predecessors outsied the loop will be ignored.
    if (BBToN.first != LT::getHeader(L)) {
      for (pred_iterator PI = pred_begin(BBToN.first),
        PE = pred_end(BBToN.first); PI != PE; ++PI) {
        assert(Blocks.count(*PI) &&
          "Data-flow node for the specified basic block is not found!");
        DFNode *PN = Blocks.find(*PI)->second;
        // Branches inside inner loop will be ignored (for details, see above).
        if (PN != BBToN.second)
          BBToN.second->addPredecessor(PN);
      }
    }
  }
}
}

namespace llvm {
template<> struct GraphTraits<tsar::DFRegion *> {
  typedef tsar::DFNode NodeType;
  static NodeType *getEntryNode(tsar::DFRegion *G) { return G->getEntryNode(); }
  typedef NodeType::succ_iterator ChildIteratorType;
  static ChildIteratorType child_begin(NodeType *N) { return N->pred_begin(); }
  static ChildIteratorType child_end(NodeType *N) { return N->pred_end(); }
  typedef tsar::DFRegion::node_iterator nodes_iterator;
  static nodes_iterator nodes_begin(tsar::DFRegion *G) { return G->node_begin(); }
  static nodes_iterator nodes_end(tsar::DFRegion *G) { return G->node_end(); }
  unsigned size(tsar::DFRegion *G) { return G->getNumNodes(); }
};

template<> struct GraphTraits<Inverse<tsar::DFRegion *> > :
  public GraphTraits<tsar::DFRegion *> {
  static ChildIteratorType child_begin(NodeType *N) { return N->succ_begin(); }
  static ChildIteratorType child_end(NodeType *N) { return N->succ_end(); }
};

template<> struct GraphTraits<tsar::DFLoop *> :
  public GraphTraits<tsar::DFRegion *> {};

template<> struct GraphTraits<Inverse<tsar::DFLoop *> > :
  public GraphTraits<Inverse<tsar::DFRegion *> > {};
}
#endif//TSAR_DF_GRAPH_H
