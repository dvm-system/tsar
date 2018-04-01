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
#include "DIMemoryEnvironment.h"
#include "tsar_utility.h"
#include "tsar_pass.h"
#include <tagged.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/ilist.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/PointerIntPair.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Pass.h>

namespace llvm {
class DataLayout;
class DominatorTree;
class DbgValueInst;
class Instruction;
class LLVMContext;
class MDNode;
class MemoryLocation;
class DIVariable;
class DIExpression;
template<class Ty> class SmallPtrSetImpl;
}

namespace std {
template<class T, class Deleter> class unique_ptr;
}

namespace tsar {
class AliasTree;
class AliasNode;
class EstimateMemory;
class DIAliasTree;
class DIAliasNode;
class DIAliasTopNode;
class DIAliasMemoryNode;
class DIAliasUnknownNode;
class DIAliasEstimateNode;
class DIMemory;
class DIEstimateMemory;
class DIUnknownMemory;
class DIMemoryEnvironment;

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

/// Finds alias nodes which contains memory locations which is bound
/// to a specified debug memory location.
void findBoundAliasNodes(const DIEstimateMemory &DIEM, AliasTree &AT,
    llvm::SmallPtrSetImpl<AliasNode *> &Nodes);

/// Finds alias nodes which contains memory locations which is bound
/// to a specified debug memory location.
void findBoundAliasNodes(const DIUnknownMemory &DIUM, AliasTree &AT,
    llvm::SmallPtrSetImpl<AliasNode *> &Nodes);

/// Finds alias nodes which contains memory locations which is bound
/// to a specified debug memory location.
void findBoundAliasNodes(const DIMemory &DIM, AliasTree &AT,
    llvm::SmallPtrSetImpl<AliasNode *> &Nodes);

/// This represents estimate memory location using metadata information.
class DIMemory :
    public llvm::ilist_node<DIMemory, llvm::ilist_tag<Alias>> {
  using BoundValues = llvm::SmallVector<llvm::WeakVH, 2>;

public:
  /// Kind of memory.
  enum Kind : uint8_t {
    FIRST_KIND = 0,
    KIND_ESTIMATE = FIRST_KIND,
    KIND_UNKNOWN,
    LAST_KIND = KIND_UNKNOWN,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND,
  };

  /// \brief Represents a match between LLVM values and memory location.
  ///
  /// `Corrupted` means that some of values (but not all) bound to the memory
  /// location have been destroyed during some transformation passes.
  /// `Destroyed` means that all such values have been destroyed.
  /// `Empty` means that no values was bound to the memory location.
  /// `Consistent` means that there is at least one value bound to the memory
  /// location. It also means that no values have been destroyed.
  enum Binding : int8_t {
    Empty = 0,
    Consistent,
    Corrupted,
    Destroyed
  };

  /// Different bit properties of a memory location.
  enum Property : uint8_t {
    NoProperty = 0,
    Explicit = 1u << 0,
    LLVM_MARK_AS_BITMASK_ENUM(Explicit)
  };

  /// This is used to iterate over all values bound to this memory location.
  using iterator = BoundValues::const_iterator;

  /// This is used to iterate over all values bound to this memory location.
  using const_iterator = BoundValues::const_iterator;

  /// Creates a copy of a specified memory location. The copy is not attached
  /// to any alias node. No values are bound to a new location.
  inline static std::unique_ptr<DIMemory> get(
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env, DIMemory &M);

  /// Destructor.
  virtual ~DIMemory();

  /// Returns the kind of this memory location.
  Kind getKind() const noexcept { return mKind; }

  /// Returns properties of this location.
  Property getProperies() const noexcept { return mProperties; }

  /// Bitwise OR the current properties with the given properties.
  void setProperties(Property P) { mProperties |= P; }

  /// Returns true if this location is explicitly mentioned in a
  /// source code.
  bool isExplicit() const { return Explicit & getProperies(); }

  /// Returns true if this is a template which represents a set of memory

  /// Returns MDNode which represents this estimate memory location.
  llvm::MDNode * getAsMDNode() noexcept { return mMD; }

  /// Returns MDNode which represents this estimate memory location.
  const llvm::MDNode * getAsMDNode() const noexcept { return mMD; }

  /// Return true if an alias node has been already specified.
  bool hasAliasNode() const noexcept { return mNode != nullptr; }

  /// Returns a node in alias graph which contains this location.
  DIAliasMemoryNode * getAliasNode() noexcept { return mNode; }

  /// Returns a node in alias graph which contains this location.
  const DIAliasMemoryNode * getAliasNode() const noexcept { return mNode; }

  /// Returns state of a match between LLVM values and this memory location.
  Binding getBinding() const {
    if (mValues.empty())
      return Empty;
    auto I = mValues.begin(), E = mValues.end();
    auto B = (*I && !llvm::isa<llvm::UndefValue>(*I)) ? Consistent : Destroyed;
    for (++I; I != E; ++I)
      if (!*I || llvm::isa<llvm::UndefValue>(*I))
        B = (B == Destroyed) ? Destroyed : Corrupted;
    return B;
  }

  /// Returns iterator that points to the beginning of the bound values list.
  iterator begin() const { return mValues.begin(); }

  /// Returns iterator that points to the ending of the bound values list.
  iterator end() const { return mValues.end(); }

  /// Binds a specified value to the estimate memory location.
  void bindValue(llvm::Value *V) { mValues.push_back(V); }

  /// Binds values from a specified range to the memory location.
  template<class ItrTy>
  void bindValue(const ItrTy &I, const ItrTy &E) { mValues.append(I, E); }

  /// Returns `true` if there is memory handle associated with this memory.
  bool hasMemoryHandle() const {  return mEnv.getInt(); }

  /// Returns debug-level memory environment.
  DIMemoryEnvironment & getEnv() { return *mEnv.getPointer(); }

  /// Change all uses of this to point to a new memory.
  void replaceAllUsesWith(DIMemory *M);

protected:
  /// Creates interface to access information about an memory location,
  /// which is represented as a metadata.
  explicit DIMemory(DIMemoryEnvironment &Env, Kind K, llvm::MDNode *MD,
      DIAliasMemoryNode *N = nullptr) :
    mEnv(&Env, false), mKind(K), mMD(MD), mNode(N) {}

  /// Returns flags which are specified for an underlying memory location.
  uint64_t getFlags() const;

  /// Returns number of a flag operand of MDNode.
  unsigned getFlagsOp() const;

  /// Bitwise OR the current flags with the given flags.
  void setFlags(uint64_t F);

private:
  friend class DIMemoryHandleBase;
  friend class DIAliasTree;

  /// Add this location to a specified node `N` in alias tree.
  void setAliasNode(DIAliasMemoryNode &N) noexcept { mNode = &N; }

  /// Updates a flag that indicates existence of memory handles.
  void setHasMemoryHandle(bool Value) { mEnv.setInt(Value); }

  Kind mKind;
  llvm::PointerIntPair<DIMemoryEnvironment *, 1, bool> mEnv;
  Property mProperties = NoProperty;
  llvm::MDNode *mMD;
  DIAliasMemoryNode *mNode;
  llvm::SmallVector<llvm::WeakVH, 1> mValues;
};

/// Builds debug memory location for a specified memory location.
llvm::Optional<DIMemoryLocation> buildDIMemory(const llvm::MemoryLocation &Loc,
    llvm::LLVMContext &Ctx,
    const llvm::DataLayout &DL, const llvm::DominatorTree &DT);

/// Builds debug memory location for a specified memory location.
std::unique_ptr<DIMemory> buildDIMemory(const EstimateMemory &EM,
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    const llvm::DataLayout &DL, const llvm::DominatorTree &DT);

/// Builds debug memory location for a specified memory location.
std::unique_ptr<DIMemory> buildDIMemory(llvm::Value &V,
    llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
    DIMemory::Property = DIMemory::Explicit);

/// \brief This represents estimate memory location using metadata information.
///
/// This class is similar to `EstimateMemory`. However, this is high level
/// abstraction of this class and has more simple structure. It means that
/// there is no hierarchy of this locations.
///
/// The difference between DIMemoryLocation and DIEstimateMemory is that for
/// the last one a special MDNode is created.
class DIEstimateMemory : public DIMemory {
public:
  /// Set of flags which may be stored in MDNode attached to this location.
  enum Flags : uint16_t {
    NoFlags = 0,
    Template = 1u << 0,
    LLVM_MARK_AS_BITMASK_ENUM(Template)
  };

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIMemory *M) {
    return M->getKind() == KIND_ESTIMATE;
  }

  /// Creates a new memory location which is not attached to any alias node.
  static std::unique_ptr<DIEstimateMemory> get(
      llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
      llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F = NoFlags);

  /// Returns existent location. Note, it will not be attached to an alias node.
  static std::unique_ptr<DIEstimateMemory> getIfExists(
      llvm::LLVMContext &Ctx, DIMemoryEnvironment &Env,
      llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F = NoFlags);

  /// Creates a copy of a specified memory location. The copy is not attached
  /// to any alias node. No values are bound to a new location.
  static std::unique_ptr<DIEstimateMemory> get(llvm::LLVMContext &Ctx,
      DIMemoryEnvironment &Env, DIEstimateMemory &EM) {
    return get(Ctx, Env, EM.getVariable(), EM.getExpression(), EM.getFlags());
  }

  /// Returns underlying variable.
  llvm::DIVariable * getVariable();

  /// Returns underlying variable.
  const llvm::DIVariable * getVariable() const;

  /// Returns expression that defines a fragment of an underlying variable.
  llvm::DIExpression * getExpression();

  /// Returns expression that defines a fragment of an underlying variable.
  const llvm::DIExpression * getExpression() const;

  /// Returns flags which are specified for an underlying variable.
  Flags getFlags() const {
    return static_cast<Flags>(DIMemory::getFlags());
  }

  /// Bitwise OR the current flags with the given flags.
  void setFlags(Flags F) { DIMemory::setFlags(F); }

  /// Returns true if this is a template representation of memory location
  /// (see DIMemoryLocation for details).
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

private:
  /// Creates interface to access information about an estimate memory location,
  /// which is represented as a metadata.
  explicit DIEstimateMemory(DIMemoryEnvironment &Env, llvm::MDNode *MD,
      DIAliasMemoryNode *N = nullptr) : DIMemory(Env, KIND_ESTIMATE, MD, N) {}
};

///\brief This represents unknown memory location using metadata information.
///
/// For example this represents memory accessed in function call.
class DIUnknownMemory : public DIMemory {
public:
  /// Set of flags which may be stored in MDNode attached to this location.
  enum Flags : uint16_t {
    NoFlags = 0,
    Call = 1u << 0,
    LLVM_MARK_AS_BITMASK_ENUM(Call)
  };

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIMemory *M) {
    return M->getKind() == KIND_UNKNOWN;
  }

  /// Creates a copy of a specified memory location. The copy is not attached
  /// to any alias node. No values are bound to a new location.
  static std::unique_ptr<DIUnknownMemory> get(llvm::LLVMContext &Ctx,
    DIMemoryEnvironment &Env, DIUnknownMemory &UM);

  /// Creates unknown memory location from a specified MDNode.
  static std::unique_ptr<DIUnknownMemory> get(llvm::LLVMContext &Ctx,
    DIMemoryEnvironment &Env, llvm::MDNode *MD, llvm::DILocation *Loc,
    Flags F = NoFlags);

  /// Returns underlying metadata.
  llvm::MDNode * getMetadata();

  /// Returns underlying metadata.
  const llvm::MDNode * getMetadata() const;

  /// Returns location in a source code which defines this memory if location
  /// is specified.
  llvm::DebugLoc getDebugLoc() const;

  /// \brief Returns true if this is a distinct memory.
  ///
  /// This means that this location is always unique. The same location
  /// can not be obtained after rebuild.
  bool isDistinct() const { return getAsMDNode() == getMetadata(); }

  /// Returns flags which are specified for an underlying variable.
  Flags getFlags() const {
    return static_cast<Flags>(DIMemory::getFlags());
  }

  /// Bitwise OR the current flags with the given flags.
  void setFlags(Flags F) { DIMemory::setFlags(F); }

  /// Returns true if this is a representation of memory accessed via a
  /// function call.
  bool isCall() const { return Call & getFlags(); }

private:
  /// Creates interface to access information about an estimate memory location,
  /// which is represented as a metadata.
  explicit DIUnknownMemory(DIMemoryEnvironment &Env, llvm::MDNode *MD,
    DIAliasMemoryNode *N = nullptr) : DIMemory(Env, KIND_UNKNOWN, MD, N) {}
};

std::unique_ptr<DIMemory> DIMemory::get(llvm::LLVMContext &Ctx,
    DIMemoryEnvironment &Env, DIMemory &M) {
  if (auto *EM = llvm::dyn_cast<DIEstimateMemory>(&M))
    return DIEstimateMemory::get(Ctx, Env, *EM);
  return DIUnknownMemory::get(Ctx, Env, llvm::cast<DIUnknownMemory>(M));
}

/// This represents debug info node in an alias tree which refers
/// an alias sequence of estimate memory locations.
class DIAliasNode :
  public llvm::ilist_node<DIAliasNode, llvm::ilist_tag<Pool>,
  llvm::ilist_sentinel_tracking<true>>,
  public llvm::ilist_node<DIAliasNode, llvm::ilist_tag<Sibling>> {

  using ChildList = llvm::simple_ilist<DIAliasNode, llvm::ilist_tag<Sibling>>;

public:
  /// Kind of node
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

  virtual ~DIAliasNode() = default;

  DIAliasNode(const DIAliasNode &) = delete;
  DIAliasNode(DIAliasNode &&) = delete;
  DIAliasNode & operator=(const DIAliasNode &) = delete;
  DIAliasNode & operator=(DIAliasNode &&) = delete;

  /// Returns the kind of this node.
  Kind getKind() const noexcept { return mKind; }

  /// Returns parent of the node.
  DIAliasNode * getParent() noexcept { return mParent; }

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

  /// Returns number of children of the node in linear time.
  std::size_t child_size() const { return mChildren.size(); }

  /// Returns true in constant time if this node is a leaf.
  bool child_empty() const { return mChildren.empty(); }

protected:
  friend class DIAliasMemoryNode;

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

class DIAliasMemoryNode : public DIAliasNode {
  using AliasList = llvm::simple_ilist<DIMemory, llvm::ilist_tag<Alias>>;

public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIAliasNode *N) {
    return N->getKind() == KIND_ESTIMATE || N->getKind() == KIND_UNKNOWN;
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

  /// Returns true if the node does not contain memory locations
  /// in constant time
  bool empty() const noexcept(noexcept(std::declval<AliasList>().empty())) {
    return mAliases.empty();
  }

  /// Returns number of memory locations in the node in linear time.
  size_t size() const noexcept(noexcept(std::declval<AliasList>().size())) {
    return mAliases.size();
  }

protected:
  friend DIAliasTree;

   /// Default constructor.
  explicit DIAliasMemoryNode(Kind K) : DIAliasNode(K) {
    assert(K == KIND_ESTIMATE || K == KIND_UNKNOWN &&
      "Alias memory node must be estimate or unknown only!");
  }

  /// Inserts new memory location at the end of memory sequence.
  ///
  /// \pre Alias estimate node may contain estimate memory locations only.
  void push_back(DIMemory &M) {
    assert((!llvm::isa<DIAliasEstimateNode>(this) ||
      llvm::isa<DIEstimateMemory>(M)) &&
      "Alias estimate node may contain estimate memory location only!");
    mAliases.push_back(M);
  }

  /// Removes node from alias tree, never deletes.
  void remove() {
    for (auto &Child : mChildren)
      Child.mParent = mParent;
    mParent->mChildren.erase(child_iterator(this));
    mParent->mChildren.splice(mParent->mChildren.end(), mChildren);
    mParent = nullptr;
  }

  /// Removes memory location from a node, never deletes.
  static void remove(DIMemory &M) {
    if (auto *N = M.getAliasNode())
      N->mAliases.erase(iterator(M));
  }

private:
  AliasList mAliases;
};

class DIAliasEstimateNode : public DIAliasMemoryNode {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIAliasNode *N) {
    return N->getKind() == KIND_ESTIMATE;
  }

private:
  friend DIAliasTree;

  /// Default constructor.
  explicit DIAliasEstimateNode() : DIAliasMemoryNode(KIND_ESTIMATE) {}
};

/// This represents information of accesses to unknown memory.
class DIAliasUnknownNode : public DIAliasMemoryNode {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DIAliasNode *N) {
    return N->getKind() == KIND_UNKNOWN;
  }

private:
  friend DIAliasTree;

  /// Default constructor.
  explicit DIAliasUnknownNode() : DIAliasMemoryNode(KIND_UNKNOWN) {}
};

class DIAliasTree {
  /// Set of estimate memory locations.
  using DIMemorySet = llvm::DenseSet<DIMemory *>;

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

  /// This is used to iterate over all estimate memory locations in tree.
  using memory_iterator = llvm::pointee_iterator<DIMemorySet::iterator>;

  /// This is used to iterate over all estimate memory locations in tree.
  using memory_const_iterator =
    llvm::pointee_iterator<DIMemorySet::const_iterator>;

  /// Builds alias tree which contains a top node only.
  explicit DIAliasTree(llvm::Function &F);

  /// Destroys alias tree.
  ~DIAliasTree() {
    for (auto *EM : mFragments)
      delete EM;
  }

  /// Returns function which is associated with the alias tree.
  llvm::Function & getFunction() noexcept { return *mFunc; }

  /// Returns function which is associated with the alias tree.
  const llvm::Function & getFunction() const noexcept { return *mFunc; }

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

  /// Returns number of nodes.
  size_type size() const { return mNodes.size(); }

  /// Returns iterator that points to the beginning of the estimate memory list.
  memory_iterator memory_begin() { return mFragments.begin(); }

  /// Returns iterator that points to the ending of the estimate memory list.
  memory_iterator memory_end() { return mFragments.end(); }

  /// Returns iterator that points to the beginning of the estimate memory list.
  memory_const_iterator memory_begin() const { return mFragments.begin(); }

  /// Returns iterator that points to the ending of the estimate memory list.
  memory_const_iterator memory_end() const { return mFragments.end(); }

  /// Returns true if there are no estimate memory locations in the tree.
  bool memory_empty() const { return mFragments.empty(); }

  /// Returns number of estimate memory locations in the tree.
  size_type memory_size() const { return mFragments.size(); }

  /// Creates new node and attaches a specified location to it. The location
  /// must not be previously attached to this alias tree.
  DIEstimateMemory & addNewNode(
    std::unique_ptr<DIEstimateMemory> &&EM, DIAliasNode &Parent);

  /// Creates new node and attaches a specified location to it. The location
  /// must not be previously attached to this alias tree.
  DIMemory & addNewUnknownNode(
    std::unique_ptr<DIMemory> &&M, DIAliasNode &Parent);

  /// Attaches a specified location to a specified alias node. The location
  /// must not be previously attached to this alias tree.
  ///
  /// \pre Alias estimate node may contain estimate memory locations only.
  DIMemory & addToNode(std::unique_ptr<DIMemory> &&M, DIAliasMemoryNode &N);

  /// \brief Removes specified node from the alias tree and deletes it.
  ///
  /// \post All memory locations from a specified node will be deleted. Parent
  /// for each child of a node `N` will be set to parent of `N`.
  void erase(DIAliasMemoryNode &N);

  /// \brief Removes specified memory from the alias tree and deletes it. If
  /// this is a last memory location in a node the whole node will be deleted.
  ///
  /// \return A pair of flags. The first flag is `true` if a specified memory
  /// has been removed successfully. The second flag is `true` is a removed
  /// memory was the last location in the node and the node has been also
  /// removed.
  std::pair<bool, bool> erase(DIMemory &M);

  /// \brief This pop up ghostview window and displays the alias tree.
  ///
  /// This depends on there being a 'dot' and 'gv' program in a system path.
  void view() const;

private:
  AliasNodePool mNodes;
  DIAliasNode *mTopLevelNode = nullptr;
  DIMemorySet mFragments;
  llvm::Function *mFunc;
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

  /// Returns true if alias tree has been successfully constructed.
  bool isConstructed() const noexcept { return mDIAliasTree != nullptr; }

  /// Returns alias tree for the last analyzed function.
  tsar::DIAliasTree & getAliasTree() noexcept {
    assert(mDIAliasTree && "Alias tree has not been constructed yet!");
    return *mDIAliasTree;
  }

  /// Returns alias tree for the last analyzed function.
  const tsar::DIAliasTree & getAliasTree() const noexcept {
    assert(mDIAliasTree && "Alias tree has not been constructed yet!");
    return *mDIAliasTree;
  }

  /// Build hierarchy of accessed memory for a specified function.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  tsar::DIAliasTree *mDIAliasTree = nullptr;
};
}
#endif//TSAR_DI_ESTIMATE_MEMORY_H
