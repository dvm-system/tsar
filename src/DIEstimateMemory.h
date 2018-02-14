//===- DIEstimateMemory.h - Memory Hierarchy (Debug) ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes similar to AliasTree and AliasNode but uses
// metadata information instead of LLVM IR entities. So memory is represented
// at a source level.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_ESTIMATE_MEMORY_H
#define TSAR_DI_ESTIMATE_MEMORY_H

#include "DIMemoryLocation.h"
#include "tsar_utility.h"
#include "tsar_pass.h"
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/ilist.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>

namespace llvm {
class DbgValueInst;
class Instruction;
class MDNode;
class DIVariable;
class DIExpression;
}

namespace tsar {
class DIAliasTree;
class DIALiasNode;
class DIAliasTopNode;
class DIAliasUnknownNode;
class DIAliasEstimateNode;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// \brief Checks that two fragments of a variable may overlap.
///
/// Two fragments of zero size may not overlap. Note that there is no reason
/// to invoke this functions for fragments of different variables. Complex
/// expressions which contains elements other then dwarf::DW_OP_LLVM_fragment
/// does not analyzed accurately. In this case overlapping is conservatively
/// assumed.
bool mayAliasFragments(
  const llvm::DIExpression &LHS, const llvm::DIExpression &RHS);

/// \brief This represents estimate memory location using metadata information.
///
/// This class is similar to `EstimateMemory`. However, this is high level
/// abstraction of this class and has more simple structure. It means that
/// there is no hierarchy of this locations.
///
/// The difference between DIMemoryLocation and DIEstimateMemory is that for
/// the last one a special MDNode is created.
class DIEstimateMemory :
  public llvm::ilist_node<DIEstimateMemory, llvm::ilist_tag<Alias>> {
public:
  enum Flags : uint16_t {
    NoFlags = 0,
    Explicit = 1u << 0,
    Template = 1u << 1,
    LLVM_MARK_AS_BITMASK_ENUM(Template)
  };

  /// Creates a new memory location which is not attached to any alias node.
  static std::unique_ptr<DIEstimateMemory> get(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F = NoFlags);

  /// Returns existent location. Note, it will not be attached to an alias node.
  static std::unique_ptr<DIEstimateMemory> getIfExists(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F = NoFlags);

  /// Returns MDNode which represents this estimate memory location.
  llvm::MDNode * getAsMDNode() noexcept { return mMD; }

  /// Returns MDNode which represents this estimate memory location.
  const llvm::MDNode * getAsMDNode() const noexcept { return mMD; }

  /// Returns underlying variable.
  llvm::DIVariable * getVariable();

  /// Returns underlying variable.
  const llvm::DIVariable * getVariable() const;

  /// Returns expression that defines a fragment of an underlying variable.
  llvm::DIExpression * getExpression();

  /// Returns expression that defines a fragment of an underlying variable.
  const llvm::DIExpression * getExpression() const;

  /// Returns flags which are specified for an underlying variable.
  Flags getFlags() const;

  /// Bitwise OR the current flags with the given flags.
  void setFlags(Flags F);

  /// Returns true if this location is explicitly mentioned in a
  /// source code.
  bool isExplicit() const { return Explicit & getFlags(); }

  /// Returns true if this is a template which represents a set of memory
  /// locations (see DIMemoryLocation for details).
  bool isTemplate() const { return Template & getFlags(); }

  /// Returns true if size is known.
  bool isSized() const {
    return DIMemoryLocation(
      const_cast<llvm::DIVariable *>(getVariable()),
      const_cast<llvm::DIExpression *>(getExpression())).isSized();
  }

  /// Returns size of location, in address units, or
  /// llvm::MemoryLocation:: UnknownSize if the size is not known.
  uint64_t getSize() const {
    return DIMemoryLocation(
      const_cast<llvm::DIVariable *>(getVariable()),
      const_cast<llvm::DIExpression *>(getExpression())).getSize();
  }

  /// Return true if an alias node has been already specified.
  bool hasAliasNode() const noexcept { return mNode != nullptr; }

  /// Returns a node in alias graph which contains this location.
  DIAliasEstimateNode * getAliasNode() noexcept { return mNode; }

  /// Returns a node in alias graph which contains this location.
  const DIAliasEstimateNode * getAliasNode() const noexcept { return mNode; }

private:
  friend class DIAliasTree;

  /// Creates interface to access information about an estimate memory location,
  /// which is represented as a metadata.
  explicit DIEstimateMemory(
      llvm::MDNode *MD, DIAliasEstimateNode *N = nullptr) : mMD(MD), mNode (N) {
    assert(MD && "Metadata must not be null!");
  }

  /// Add this location to a specified node `N` in alias tree.
  void setAliasNode(DIAliasEstimateNode &N) noexcept { mNode = &N; }

  /// Returns number of flag operand of MDNode.
  unsigned getFlagsOp() const;

  llvm::MDNode *mMD;
  DIAliasEstimateNode *mNode;
};
}

namespace tsar {
/// This represents debug info node in an alias tree which refers
/// an alias sequence of estimate memory locations.
class DIAliasNode :
  public llvm::ilist_node<DIAliasNode, llvm::ilist_tag<Pool>,
  llvm::ilist_sentinel_tracking<true>>,
  public llvm::ilist_node<DIAliasNode, llvm::ilist_tag<Sibling>> {

  using ChildList = llvm::simple_ilist<DIAliasNode, llvm::ilist_tag<Sibling>>;

public:
  /// Kind of node.
  enum Kind : uint8_t {
    FIRST_KIND = 0,
    KIND_TOP = FIRST_KIND,
    KIND_ESTIMATE,
    KIND_UNKNOWN,
    LAST_KIND = KIND_UNKNOWN,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND,
  };

  /// This type is used to iterate over all children of this node.
  using child_iterator = ChildList::iterator;

  /// This type is used to iterate over all children of this node.
  using const_child_iterator = ChildList::const_iterator;

  ~DIAliasNode() = default;

  DIAliasNode(const DIAliasNode &) = delete;
  DIAliasNode(DIAliasNode &&) = delete;
  DIAliasNode & operator=(const DIAliasNode &) = delete;
  DIAliasNode & operator=(DIAliasNode &&) = delete;

  /// Returns the kind of this node.
  Kind getKind() const noexcept { return mKind; }

  /// Returns parent of the node.
  const DIAliasNode * getParent() const noexcept { return mParent; }

  /// Returns iterator that points to the beginning of the children list.
  child_iterator child_begin() { return mChildren.begin(); }

  /// Returns iterator that points to the ending of the children list.
  child_iterator child_end() { return mChildren.end(); }

  /// Returns iterator that points to the beginning of the children list.
  const_child_iterator child_begin() const { return mChildren.begin(); }

  /// Returns iterator that points to the ending of the children list.
  const_child_iterator child_end() const { return mChildren.end(); }

protected:
  /// Creates an empty node of a specified kind `K`.
  explicit DIAliasNode(Kind K) : mKind(K) {};

  /// Specifies a parent for this node.
  void setParent(DIAliasNode &Parent) {
    if (mParent)
      mParent->mChildren.erase(child_iterator(this));
    mParent = &Parent;
    mParent->mChildren.push_back(*this);
  }

  Kind mKind;
  DIAliasNode *mParent = nullptr;
  ChildList mChildren;
};

/// This represents a root of an alias tree.
class DIAliasTopNode : public DIAliasNode {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIAliasNode *N) {
    return N->getKind() == KIND_TOP;
  }

private:
  friend class DIAliasTree;

  /// Default constructor.
  DIAliasTopNode() : DIAliasNode(KIND_TOP) {}
};

class DIAliasEstimateNode : public DIAliasNode {
  using AliasList =
    llvm::simple_ilist<DIEstimateMemory, llvm::ilist_tag<Alias>>;

public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIAliasNode *N) {
    return N->getKind() == KIND_ESTIMATE;
  }

  /// This type is used to iterate over all alias memory locations in this node.
  using iterator = AliasList::iterator;

  /// This type is used to iterate over all alias memory locations in this node.
  using const_iterator = AliasList::const_iterator;

  /// Returns iterator that points to the beginning of the alias list.
  iterator begin() { return mAliases.begin(); }

  /// Returns iterator that points to the beginning of the alias list.
  const_iterator begin() const { return mAliases.begin(); }

  /// Returns iterator that points to the ending of the alias list.
  iterator end() { return mAliases.end(); }

  /// Returns iterator that points to the ending of the alias list.
  const_iterator end() const { return mAliases.end(); }

  /// Returns true if the node does not contain memory locations.
  bool empty() const noexcept(noexcept(std::declval<AliasList>().empty())) {
    return mAliases.empty();
  }

private:
  friend DIAliasTree;

  /// Default constructor.
  DIAliasEstimateNode() : DIAliasNode(KIND_ESTIMATE) {}

  /// Inserts new estimate memory location at the end of memory sequence.
  void push_back(DIEstimateMemory &EM) { mAliases.push_back(EM); }

  AliasList mAliases;
};

/// This represents information of accesses to unknown memory.
class DIAliasUnknownNode : public DIAliasNode {
  using UnknownList = llvm::SmallPtrSet<llvm::MDNode *, 8>;

public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIAliasNode *N) {
    return N->getKind() == KIND_UNKNOWN;
  }

  /// This type is used to iterate over all alias memory locations in this node.
  using iterator = UnknownList::iterator;

  /// This type is used to iterate over all alias memory locations in this node.
  using const_iterator = UnknownList::const_iterator;

  /// Returns iterator that points to the beginning of the alias list.
  iterator begin() { return mUnknownInsts.begin(); }

  /// Returns iterator that points to the beginning of the alias list.
  const_iterator begin() const { return mUnknownInsts.begin(); }

  /// Returns iterator that points to the ending of the alias list.
  iterator end() { return mUnknownInsts.end(); }

  /// Returns iterator that points to the ending of the alias list.
  const_iterator end() const { return mUnknownInsts.end(); }

  /// Returns true if the node does not contain memory locations.
  bool empty() const noexcept(noexcept(std::declval<UnknownList>().empty())) {
    return mUnknownInsts.empty();
  }

private:
  friend DIAliasTree;

  /// Default constructor.
  DIAliasUnknownNode() : DIAliasNode(KIND_UNKNOWN) {}

  /// Inserts unknown memory access at the end of unknown memory sequence.
  void push_back(llvm::MDNode *I) { mUnknownInsts.insert(I); }

  UnknownList mUnknownInsts;
};

class DIAliasTree {
  /// Set of estimate memory locations.
  using DIMemorySet = llvm::DenseSet<DIEstimateMemory *>;

  /// Pool to store pointers to all alias nodes.
  using AliasNodePool = llvm::ilist<DIAliasNode,
    llvm::ilist_tag<Pool>, llvm::ilist_sentinel_tracking<true>>;

public:
  /// This is used to iterate over all nodes in tree.
  using iterator = AliasNodePool::iterator;

  /// This is used to iterate over all nodes in tree.
  using const_iterator = AliasNodePool::const_iterator;

  /// Size of an alias tree.
  using size_type = AliasNodePool::size_type;

  /// Builds alias tree which contains a top node only.
  explicit DIAliasTree() : mTopLevelNode(new DIAliasTopNode) {
    mNodes.push_back(mTopLevelNode);
  }

  /// Destroys alias tree.
  ~DIAliasTree() {
    for (auto *EM : mFragments)
      delete EM;
  }
  /// Returns root of the alias tree.
  DIAliasNode * getTopLevelNode() noexcept { return mTopLevelNode; }

  /// Returns root of the alias tree.
  const DIAliasNode * getTopLevelNode() const noexcept { return mTopLevelNode; }

  /// Returns iterator that points to the beginning of the node list.
  iterator begin() { return iterator(mNodes.begin()); }

  /// Returns iterator that points to the ending of the node list.
  iterator end() { return iterator(mNodes.end()); }

  /// Returns iterator that points to the beginning of the node list.
  const_iterator begin() const { return const_iterator(mNodes.begin()); }

  /// Returns iterator that points to the ending of the node list.
  const_iterator end() const { return const_iterator(mNodes.end()); }

  /// Returns true if this alias tree is empty.
  bool empty() const { return mNodes.empty(); }

  /// Returns number of nodes, including forwarding.
  size_type size() const { return mNodes.size(); }

  /// Creates new node and attaches a specified location to it. The location
  /// must not be previously attached to this alias tree.
  DIEstimateMemory & addNewNode(
      std::unique_ptr<DIEstimateMemory> &&EM, DIAliasNode &Parent) {
    auto *N = new DIAliasEstimateNode;
    mNodes.push_back(N);
    N->setParent(Parent);
    return addToNode(std::move(EM), *N);
  }

  /// Attaches a specified location to a specified alias node. The location
  /// must not be previously attached to this alias tree.
  DIEstimateMemory & addToNode(
      std::unique_ptr<DIEstimateMemory> &&EM, DIAliasEstimateNode &N) {
    auto Pair = mFragments.insert(EM.release());
    assert(Pair.second && "Memory location is arleady attached to a node!");
    (*Pair.first)->setAliasNode(N);
    N.push_back(**Pair.first);
    return **Pair.first;
  }

  /// \brief This pop up ghostview window and displays the alias tree.
  ///
  /// This depends on there being a 'dot' and 'gv' program in a system path.
  void view() const;

private:
  AliasNodePool mNodes;
  DIAliasNode *mTopLevelNode = nullptr;
  DIMemorySet mFragments;
};
}

namespace llvm {
//===----------------------------------------------------------------------===//
// GraphTraits specializations for alias tree (DIAliasTree)
//===----------------------------------------------------------------------===//

template<> struct GraphTraits<tsar::DIAliasNode *> {
  using NodeRef = tsar::DIAliasNode *;
  static NodeRef getEntryNode(tsar::DIAliasNode *AN) noexcept {
    return AN;
  }
  using ChildIteratorType =
    pointer_iterator<tsar::DIAliasNode::child_iterator>;
  static ChildIteratorType child_begin(NodeRef AN) {
    return ChildIteratorType(AN->child_begin());
  }
  static ChildIteratorType child_end(NodeRef AN) {
    return ChildIteratorType(AN->child_end());
  }
};

template<> struct GraphTraits<const tsar::DIAliasNode *> {
  using NodeRef = const tsar::DIAliasNode *;
  static NodeRef getEntryNode(const tsar::DIAliasNode *AN) noexcept {
    return AN;
  }
  using ChildIteratorType =
    pointer_iterator<tsar::DIAliasNode::const_child_iterator>;
  static ChildIteratorType child_begin(NodeRef AN) {
    return ChildIteratorType(AN->child_begin());
  }
  static ChildIteratorType child_end(NodeRef AN) {
    return ChildIteratorType(AN->child_end());
  }
};

template<> struct GraphTraits<tsar::DIAliasTree *> :
    public GraphTraits<tsar::DIAliasNode *> {
  static NodeRef getEntryNode(tsar::DIAliasTree *AT) noexcept {
    return AT->getTopLevelNode();
  }
  using nodes_iterator = pointer_iterator<tsar::DIAliasTree::iterator>;
  static nodes_iterator nodes_begin(tsar::DIAliasTree *AT) {
    return nodes_iterator(AT->begin());
  }
  static nodes_iterator nodes_end(tsar::DIAliasTree *AT) {
    return nodes_iterator(AT->end());
  }
  static std::size_t size(tsar::DIAliasTree *AT) { return AT->size(); }
};

template<> struct GraphTraits<const tsar::DIAliasTree *> :
    public GraphTraits<const tsar::DIAliasNode *> {
  static NodeRef getEntryNode(const tsar::DIAliasTree *AT) noexcept {
    return AT->getTopLevelNode();
  }
  using nodes_iterator = pointer_iterator<tsar::DIAliasTree::const_iterator>;
  static nodes_iterator nodes_begin(const tsar::DIAliasTree *AT) {
    return nodes_iterator(AT->begin());
  }
  static nodes_iterator nodes_end(const tsar::DIAliasTree *AT) {
    return nodes_iterator(AT->end());
  }
  static std::size_t size(const tsar::DIAliasTree *AT) { return AT->size(); }
};

/// This per-function analysis pass build hierarchy of a whole memory which
/// is used in an analyzed function.
class DIEstimateMemoryPass : public FunctionPass, private bcl::Uncopyable {
/// Set of memory locations.
using DIMemorySet = DenseSet<tsar::DIMemoryLocation>;

/// Map from variable to its fragments.
using DIFragmentMap = DenseMap<
  DIVariable *, TinyPtrVector<DIExpression *>,
  DenseMapInfo<DIVariable *>,
  tsar::TaggedDenseMapPair<
    bcl::tagged<DIVariable *, DIVariable>,
    bcl::tagged<TinyPtrVector<DIExpression *>, DIExpression>>>;

public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DIEstimateMemoryPass() : FunctionPass(ID) {
    initializeDIEstimateMemoryPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns alias tree for the last analyzed function.
  tsar::DIAliasTree & getAliasTree() {
    assert(mAliasTree && "Alias tree has not been constructed yet!");
    return *mAliasTree;
  }

  /// Returns alias tree for the last analyzed function.
  const tsar::DIAliasTree & getAliasTree() const {
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
    mContext = nullptr;
  }

private:
  /// Finds fragments of a variables which are used in a program and does not
  /// alias each other.
  ///
  /// There are two out parameters. Firstly, this is a map from a variable to its
  /// fragments. Secondly, this is a list of all fragments.
  void findNoAliasFragments(Function &F,
      DIFragmentMap &VarToFragment, DIMemorySet &SmallestFragments);

  tsar::DIAliasTree *mAliasTree = nullptr;
  LLVMContext *mContext = nullptr;
};
}
#endif//TSAR_DI_ESTIMATE_MEMORY_H
