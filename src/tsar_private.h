//===--- tsar_private.h - Private Variable Analyzer --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines passes to analyze variables which can be privatized.
// We use data-flow framework to implement this kind of analysis. This file
// contains elements which is necessary to determine this framework.
// The following articles can be helpful to understand it:
//  * "Automatic Array Privatization" Peng Tu and David Padua 
//  * "Array Privatization for Parallel Execution of Loops" Zhiyuan Li.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRIVATE_H
#define TSAR_PRIVATE_H

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_df_loop.h"

namespace tsar {
/// \brief Representation of a data-flow value.
/// 
/// A data-flow value is a set of variables for which a number of operations
/// is defined.
class PrivateDFValue {
  typedef llvm::SmallPtrSet<llvm::AllocaInst *, 64> AllocaSet;
  // There are two kind of values. The KIND_FULL kind means that the set of
  // variables is full and contains all variables used in the analyzed program.
  // The KIND_MASK kind means that the set contains variables located in the 
  // alloca collection (mAllocas). This is internal information which is
  // neccessary to safely and effectively implement a number of operations
  // which is permissible for a arbitrary set of variables.
  enum Kind {
    FIRST_KIND,
    KIND_FULL = FIRST_KIND,
    KIND_MASK,
    LAST_KIND = KIND_MASK,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };
  PrivateDFValue(Kind K) : mKind(K) {
    assert(FIRST_KIND <= K && K <= LAST_KIND &&
            "The specified kind is invalid!");
  }
public:
  /// Creats a value, which contains all variables used in the analyzed 
  /// program.
  static PrivateDFValue fullValue() {
    return PrivateDFValue(PrivateDFValue::KIND_FULL);
  }

  /// Creates an empty value.
  static PrivateDFValue emptyValue() {
    return PrivateDFValue(PrivateDFValue::KIND_MASK);
  }

  /// \brief Calculates the difference between a set of allocas and a set
  /// which is represented as a data-flow value.
  ///
  /// \param [in] AIBegin Iterator that points to the beginning of the allocas
  /// set.
  /// \param [in] AIEnd Iterator that points to the ending of the allocas set.
  /// \param [in] Value Data-flow value.
  /// \param [out] Result It contains the result of this operation.
  /// The following operation should be provided:
  /// - void ResultSet::insert(llvm::AllocaInst *).
  template<class alloca_iterator, class ResultSet>
  static void difference(const alloca_iterator &AIBegin,
                         const alloca_iterator &AIEnd,
                         const PrivateDFValue &Value, ResultSet &Result) {
    //If all allocas are contained in Value or range of iterators is empty,
    //than Result should be empty.
    if (Value.mKind == KIND_FULL || AIBegin == AIEnd)
      return;
    if (Value.mAllocas.empty())
      Result.insert(AIBegin, AIEnd);
    for (alloca_iterator I = AIBegin; I != AIEnd; ++I)
      if (Value.exist(*I))
        Result.insert(*I);
  }

  /// Destructor.
  ~PrivateDFValue() { mKind = INVALID_KIND; }

  /// Move constructor.
  PrivateDFValue(PrivateDFValue &&that) :
    mKind(that.mKind), mAllocas(std::move(that.mAllocas)) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Copy constructor.
  PrivateDFValue(const PrivateDFValue &that) :
    mKind(that.mKind), mAllocas(that.mAllocas) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Move assignment operator.
  PrivateDFValue & operator= (PrivateDFValue &&that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mAllocas = std::move(that.mAllocas);
    }
    return *this;
  }

  /// Copy assignment operator.
  PrivateDFValue & operator= (const PrivateDFValue &that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mAllocas = that.mAllocas;
    }
    return *this;
  }

  /// Returns true if the value contains the specified alloca.
  bool exist(llvm::AllocaInst *AI) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mAllocas.count(AI);
  }

  /// Returns true if the value does not contain any alloca.
  bool empty() const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_MASK && mAllocas.empty();
  }

  /// Removes all allocas from the value.
  void clear() {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    mKind = KIND_MASK;
    mAllocas.clear();
  }

  /// Inserts a new alloca into the value, returns false if it already exists.
  bool insert(llvm::AllocaInst *AI) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mAllocas.insert(AI);
  }

  /// Inserts all allocas from the range into the value, returns false
  /// ifnothing has been added.
  template<class alloca_iterator >
  bool insert(const alloca_iterator &AIBegin, const alloca_iterator &AIEnd) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      return false;
    bool isChanged = false;
    for (alloca_iterator I = AIBegin; I != AIEnd; ++I)
      isChanged = mAllocas.insert(*I) || isChanged;
    return isChanged;
  }

  /// Realizes intersection between two values.
  bool intersect(const PrivateDFValue &with);

  /// Realizes merger between two values.
  bool merge(const PrivateDFValue &with);

  /// Compares two values.
  bool operator==(const PrivateDFValue &RHS) const;

  /// Compares two values.
  bool operator!=(const PrivateDFValue &RHS) const { return !(*this == RHS); }
private:
  Kind mKind;
  AllocaSet mAllocas;
};

/// \brief This calculates the difference between a set of allocas and a set
/// which is represented as a data-flow value.
///
/// \param [in] AIBegin Iterator that points to the beginning of the allocas
/// set.
/// \param [in] AIEnd Iterator that points to the ending of the allocas set.
/// \param [in] Value Data-flow value.
/// \param [out] Result It contains the result of this operation.
/// The following operation should be provided:
/// - void ResultSet::insert(llvm::AllocaInst *).
template<class alloca_iterator, class ResultSet>
static void difference(const alloca_iterator &AIBegin,
                       const alloca_iterator &AIEnd,
                       const PrivateDFValue &Value, ResultSet &Result) {
  PrivateDFValue::difference(AIBegin, AIEnd, Value, Result);
}

/// Instances of this class are used to represent nodes in a data-flow graph.
///
/// This class is an abstract class and a base class for representations
/// of nodes in data-flow graph. There are three possible types of nodes
/// (for details see tsar_df_loop.h)
class PrivateDFNode : public SmallDFNode<PrivateDFNode, 8> {
public:
  /// Set of alloca instructions.
  typedef llvm::SmallPtrSet<llvm::AllocaInst *, 64> AllocaSet;

  /// Creates data-flow node and specifies a set of allocas 
  /// that should be analyzed.
  explicit PrivateDFNode(const AllocaSet &AnlsAllocas) :
    mAnlsAllocas(AnlsAllocas),
    mIn(PrivateDFValue::emptyValue()), mOut(PrivateDFValue::emptyValue()) {}

  /// Virtual destructor.
  virtual ~PrivateDFNode() {}

  /// Returns a data-flow value associated with this node.
  const PrivateDFValue & getValue() const { return mOut; }

  /// Specifies a data-flow value associated with this node.
  void setValue(PrivateDFValue V) { mOut = std::move(V); }

  /// Returns a data-flow value before the node.
  const PrivateDFValue & getIn() const { return mIn; }

  /// Returns a data-flow value after the node.
  const PrivateDFValue & getOut() const { return getValue(); }

  /// Returns allocas which have definitions in the node.
  const AllocaSet & getDefs() const { return mDefs; }

  /// \brief Returns allocas which get values outside the node.
  ///
  /// The node does not contain definitions of these values before their use.
  /// In case of loops such allocas can get value not only outside the loop
  /// but also from previouse loop iterations.
  const AllocaSet & getUses() const { return mUses; }

  /// \brief Evaluate the transfer function according to a data-flow analysis
  /// algorithm.
  ///
  /// \return This returns true if produced data-flow value differs from
  /// the data-flow value produced on previouse iteration of the data-flow 
  /// analysis algorithm. For the first iteration the new value compares with
  /// the initial one.
  virtual bool transferFunction(PrivateDFValue In) = 0;

protected:
  const AllocaSet &mAnlsAllocas;
  PrivateDFValue mIn;
  PrivateDFValue mOut;
  AllocaSet mDefs;
  AllocaSet mUses;
};
}

namespace llvm {
class Loop;

class PrivateRecognitionPass :
  public FunctionPass, private Utility::Uncopyable {
  typedef tsar::PrivateDFNode::AllocaSet AllocaSet;
public: 
  /// Iterator to access private and last private allocas.
  typedef AllocaSet::iterator alloca_iterator;

  /// Pass identification, replacement for typeid
  static char ID; 

  /// Default constructor.
  PrivateRecognitionPass() : FunctionPass(ID) {
    initializePrivateRecognitionPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns range to access private allocas for the specified loop.
  std::pair<alloca_iterator, alloca_iterator> getPrivatesFor(Loop *L) const {
    assert(L && "Loop must not be null!");
    auto LToPriv = mPrivates.find(L);
    if (LToPriv != mPrivates.end()) {
      return std::make_pair(LToPriv->second->begin(), LToPriv->second->end());
    }
    return std::make_pair(mEmptyAllocaSet.begin(), mEmptyAllocaSet.end());
  }

  /// Returns range to access last private allocas for the specified loop.
  std::pair<alloca_iterator, alloca_iterator> getLastPrivatesFor(Loop *L) const {
    assert(L && "Loop must not be null!");
    auto LToPriv = mLastPrivates.find(L);
    if (LToPriv != mLastPrivates.end()) {
      return std::make_pair(LToPriv->second->begin(), LToPriv->second->end());
    }
    return std::make_pair(mEmptyAllocaSet.begin(), mEmptyAllocaSet.end());
  }

  /// Returns range to access second to last private allocas for the specified loop.
  std::pair<alloca_iterator, alloca_iterator> getSecondToLastPrivatesFor(Loop *L) const {
    assert(L && "Loop must not be null!");
    auto LToPriv = mSecondToLastPrivates.find(L);
    if (LToPriv != mSecondToLastPrivates.end()) {
      return std::make_pair(LToPriv->second->begin(), LToPriv->second->end());
    }
    return std::make_pair(mEmptyAllocaSet.begin(), mEmptyAllocaSet.end());
  }

  /// Returns range to access dynamic private allocas for the specified loop.
  std::pair<alloca_iterator, alloca_iterator> getDynamicPrivatesFor(Loop *L) const {
    assert(L && "Loop must not be null!");
    auto LToPriv = mDynamicPrivates.find(L);
    if (LToPriv != mDynamicPrivates.end()) {
      return std::make_pair(LToPriv->second->begin(), LToPriv->second->end());
    }
    return std::make_pair(mEmptyAllocaSet.begin(), mEmptyAllocaSet.end());
  }

  /// Returns true if the specified alloca is private for the loop.
  bool isPrivateFor(AllocaInst *AI, Loop *L) const {
    assert(L && "Loop must not be null!");
    assert(AI && "Value must not be null!");
    auto LToPriv = mPrivates.find(L);
    if (LToPriv != mPrivates.end())
      return LToPriv->second->count(AI);
    return false;
  }

  /// Returns true if the specified alloca is last private for the loop.
  bool isLastPrivateFor(AllocaInst *AI, Loop *L) const {
    assert(L && "Loop must not be null!");
    assert(AI && "Value must not be null!");
    auto LToPriv = mLastPrivates.find(L);
    if (LToPriv != mLastPrivates.end())
      return LToPriv->second->count(AI);
    return false;
  }

  /// Returns true if the specified alloca is second to last private for the loop.
  bool isSecondToLastPrivateFor(AllocaInst *AI, Loop *L) const {
    assert(L && "Loop must not be null!");
    assert(AI && "Value must not be null!");
    auto LToPriv = mSecondToLastPrivates.find(L);
    if (LToPriv != mSecondToLastPrivates.end())
      return LToPriv->second->count(AI);
    return false;
  }

  /// Returns true if the specified alloca is dynamic private for the loop.
  bool isDynamicPrivateFor(AllocaInst *AI, Loop *L) const {
    assert(L && "Loop must not be null!");
    assert(AI && "Value must not be null!");
    auto LToPriv = mDynamicPrivates.find(L);
    if (LToPriv != mDynamicPrivates.end())
      return LToPriv->second->count(AI);
    return false;
  }

  /// Recognises private (last private) variables for loops
  /// in the specified function.
  bool runOnFunction(Function &F) override;

  /// Release allocated memory.
  void releaseMemory() override {
  // TODO(kaniandr@gmail.com): implement this method and release memory
  // which has been allocated for a data-flow graph of loop nests.
  }

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  llvm::DenseMap<llvm::Loop *, AllocaSet *> mPrivates;
  llvm::DenseMap<llvm::Loop *, AllocaSet *> mLastPrivates;
  llvm::DenseMap<llvm::Loop *, AllocaSet *> mSecondToLastPrivates;
  llvm::DenseMap<llvm::Loop *, AllocaSet *> mDynamicPrivates;
  AllocaSet mEmptyAllocaSet;
};
}

namespace tsar {
/// Representation of an entry node in a data-flow framework.
class PrivateEntryNode :
public PrivateDFNode,
public EntryDFBase <PrivateDFNode> {
public:
  /// Creates data-flow node and specifies a set of allocas 
  /// that should be analyzed.
  PrivateEntryNode(const AllocaSet &AnlsAllocas) :
    PrivateDFNode(AnlsAllocas) {}

  /// \brief Evaluate the transfer function according to a data-flow analysis
  /// algorithm.
  ///
  /// A data-flow value for the entry node is never changed, so this function
  /// alwasy returns false.
  bool transferFunction(PrivateDFValue) override { return false; }
};

/// Representation of a basic block in a data-flow framework.
class PrivateBBNode :
public PrivateDFNode,
public BlockDFBase<llvm::BasicBlock, PrivateDFNode> {
  typedef BlockDFBase<llvm::BasicBlock, PrivateDFNode> BlockBase;
public:
  /// Creates data-flow node and specifies a set of allocas 
  /// that should be analyzed.
  PrivateBBNode(llvm::BasicBlock *BB, const AllocaSet &AnlsAllocas);

  /// Evaluate the transfer function according to a data-flow analysis
  /// algorithm.
  bool transferFunction(PrivateDFValue In) override;
};

/// Representation of a loop in a data-flow framework.
class PrivateLNode :
public PrivateDFNode,
public LoopDFBase<llvm::BasicBlock, llvm::Loop, PrivateDFNode> {
  typedef LoopDFBase<llvm::BasicBlock, llvm::Loop, PrivateDFNode> LoopBase;
public:
  static PrivateDFValue topElement() { return PrivateDFValue::fullValue(); }
  static PrivateDFValue boundaryCondition() { return PrivateDFValue::emptyValue(); }
  static void meetOperator(const PrivateDFValue & LHS, PrivateDFValue &RHS) { RHS.intersect(LHS); }

  /// Creates data-flow node and specifies a set of allocas
  /// that should be analyzed.
  PrivateLNode(llvm::Loop *L, const AllocaSet &AnlsAllocas,
               llvm::DenseMap<llvm::Loop *, AllocaSet *> &Privates,
               llvm::DenseMap<llvm::Loop *, AllocaSet *> &LastPrivates,
               llvm::DenseMap<llvm::Loop *, AllocaSet *> &SecondToLastPrivates,
               llvm::DenseMap<llvm::Loop *, AllocaSet *> &DynamicPrivates) :
    PrivateDFNode(AnlsAllocas), LoopBase::LoopDFBase(L),
    mPrivates(Privates), mLastPrivates(LastPrivates),
    mSecondToLastPrivates(SecondToLastPrivates),
    mDynamicPrivates(DynamicPrivates) {}

  /// Destructor.
  ~PrivateLNode() {
    for (PrivateDFNode *N : getNodes())
      delete N;
  }

  /// \brief Add an entry node of the data-flow graph.
  ///
  /// \pre See preconditions for this function in the base class. 
  PrivateDFNode * addEntryNode() {
    PrivateDFNode *N = new PrivateEntryNode(mAnlsAllocas);
    LoopBase::addEntryNode(N);
    return N;
  }

  /// Creates an abstraction of the specified inner loop.
  std::pair<PrivateLNode *, PrivateDFNode *> addNode(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    PrivateLNode *N =
      new PrivateLNode(L, mAnlsAllocas, mPrivates, mLastPrivates,
                       mSecondToLastPrivates, mDynamicPrivates);
    LoopBase::addNode(N);
    return std::pair<PrivateLNode *, PrivateDFNode *>(N, N);
  }

  /// Creates an abstraction of the specified basic block.
  PrivateDFNode *addNode(llvm::BasicBlock *BB) {
    assert(BB && "BasicBlock must not be null!");
    PrivateBBNode *N = new PrivateBBNode(BB, mAnlsAllocas);
    LoopBase::addNode(N);
    return N;
  }

  /// Evaluate the transfer function according to a data-flow analysis
  /// algorithm.
  bool transferFunction(PrivateDFValue) override { return false; }

  void collapse();

private:
  llvm::DenseMap<llvm::Loop *, AllocaSet *> &mPrivates;
  llvm::DenseMap<llvm::Loop *, AllocaSet *> &mLastPrivates;
  llvm::DenseMap<llvm::Loop *, AllocaSet *> &mSecondToLastPrivates;
  llvm::DenseMap<llvm::Loop *, AllocaSet *> &mDynamicPrivates;
};
}

namespace llvm{
template<> struct GraphTraits<tsar::PrivateLNode *> {
  typedef tsar::PrivateDFNode NodeType;
  static NodeType *getEntryNode(tsar::PrivateLNode *G) { return G->getEntryNode(); }
  typedef NodeType::succ_iterator ChildIteratorType;
  static ChildIteratorType child_begin(NodeType *N) { return N->pred_begin(); }
  static ChildIteratorType child_end(NodeType *N) { return N->pred_end(); }
  typedef tsar::PrivateLNode::nodes_iterator nodes_iterator;
  static nodes_iterator nodes_begin(tsar::PrivateLNode *G) { return G->nodes_begin(); }
  static nodes_iterator nodes_end(tsar::PrivateLNode *G) { return G->nodes_end(); }
  unsigned size(tsar::PrivateLNode *G) { return G->getNumNodes(); }
};

template<> struct GraphTraits<Inverse<tsar::PrivateLNode *> > :
  public GraphTraits <tsar::PrivateLNode *> {
  static ChildIteratorType child_begin(NodeType *N) { return N->succ_begin(); }
  static ChildIteratorType child_end(NodeType *N) { return N->succ_end(); }
};
}

namespace tsar {
/// \brief Data-flow framework.
///
/// This is a specialization of the tsar::DataFlowTraits template,
/// see it for details.
template<> struct DataFlowTraits<PrivateLNode *> :
llvm::GraphTraits<PrivateLNode *> {  
  typedef PrivateDFValue ValueType;
  static ValueType topElement(PrivateLNode *G) {
    return PrivateLNode::topElement();
  }
  static ValueType boundaryCondition(PrivateLNode *G) {
    return PrivateLNode::boundaryCondition();
  }
  static void setValue(ValueType V, NodeType *N) { N->setValue(std::move(V)); }
  static const ValueType & getValue(NodeType *N) { return N->getValue(); }
  static void meetOperator(const ValueType &LHS, ValueType &RHS) {
    PrivateLNode::meetOperator(LHS, RHS);
  }
  static bool transferFunction(ValueType V, NodeType *N) {
    return N->transferFunction(std::move(V));
  }
};

/// \brief Data-flow framework for loop nest.
///
/// This is a specialization of the tsar::LoopDFTraits template,
/// see it for details.
template<> struct LoopDFTraits<tsar::PrivateLNode *> :
DataFlowTraits<PrivateLNode *> {
  static llvm::Loop *getLoop(PrivateLNode *G) { return G->getLoop(); }
  static NodeType * addEntryNode( PrivateLNode *G) { return G->addEntryNode(); }
  static std::pair<PrivateLNode *, NodeType *> 
    addNode(llvm::Loop *L, PrivateLNode *G) { return G->addNode(L); }
  static NodeType *addNode(llvm::BasicBlock *BB, PrivateLNode *G) {
    return G->addNode(BB);
  }
  static void addSuccessor(NodeType *Succ, NodeType *N) {
    N->addSuccessor(Succ);
  }
  static void addPredecessor(NodeType *Pred, NodeType *N) {
    N->addPredecessor(Pred);
  }
  static void collapseLoop(PrivateLNode *G) { G->collapse(); }
  static void setExitingNode(NodeType *N, PrivateLNode *G) { G->setExitingNode(N); }
  static void setLatchNode(NodeType *N, PrivateLNode *G) { G->setLatchNode(N); }
};
}
#endif//TSAR_PRIVATE_H
