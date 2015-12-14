//===--- tsar_private.h - Private Variable Analyzer --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines passes to determine allocas which can be privatized.
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
#include <cell.h>
#include "tsar_df_graph.h"

namespace tsar {
/// \brief This covers IN and OUT values for a data-flow node.
///
/// \tparam Id Identifier, for example a data-flow framework which is used.
/// This is neccessary to distinguish different data-flow values.
/// \tparam InTy Type of data-flow value before the node (IN).
/// \tparam OutTy Type of data-flow value after the node (OUT).
///
/// It is possible to set InTy or OutTy to void. In this case
/// corresponding methods (get and set) are not available.
template<class Id, class InTy, class OutTy = InTy >
class DFValue {
public:
  /// Returns a data-flow value before the node.
  std::enable_if_t<!std::is_same<InTy, void>::value, const InTy &>
    getIn() const { return mIn; }

  /// Specifies a data-flow value before the node.
  void setIn(std::enable_if_t<!std::is_same<InTy, void>::value, InTy> V) {
    mIn = std::move(V);
  }

  /// Returns a data-flow value after the node.
  std::enable_if_t<!std::is_same<OutTy, void>::value, const OutTy &>
    getOut() const { return mOut; }

  /// Specifies a data-flow value after the node.
  void setOut(std::enable_if_t<!std::is_same<OutTy, void>::value, OutTy> V) {
    mOut = std::move(V);
  }

private:
  InTy mIn;
  OutTy mOut;
};

/// \brief Representation of a data-flow value formed by a set of allocas.
/// 
/// A data-flow value is a set of allocas for which a number of operations
/// is defined.
class AllocaDFValue {
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
  AllocaDFValue(Kind K) : mKind(K) {
    assert(FIRST_KIND <= K && K <= LAST_KIND &&
            "The specified kind is invalid!");
  }
public:
  /// Creats a value, which contains all variables used in the analyzed
  /// program.
  static AllocaDFValue fullValue() {
    return AllocaDFValue(AllocaDFValue::KIND_FULL);
  }

  /// Creates an empty value.
  static AllocaDFValue emptyValue() {
    return AllocaDFValue(AllocaDFValue::KIND_MASK);
  }

  /// Default constructor creates an empty value.
  AllocaDFValue() : AllocaDFValue(AllocaDFValue::KIND_MASK) {}

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
                         const AllocaDFValue &Value, ResultSet &Result) {
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
  ~AllocaDFValue() { mKind = INVALID_KIND; }

  /// Move constructor.
  AllocaDFValue(AllocaDFValue &&that) :
    mKind(that.mKind), mAllocas(std::move(that.mAllocas)) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Copy constructor.
  AllocaDFValue(const AllocaDFValue &that) :
    mKind(that.mKind), mAllocas(that.mAllocas) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Move assignment operator.
  AllocaDFValue & operator= (AllocaDFValue &&that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mAllocas = std::move(that.mAllocas);
    }
    return *this;
  }

  /// Copy assignment operator.
  AllocaDFValue & operator= (const AllocaDFValue &that) {
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
  bool intersect(const AllocaDFValue &with);

  /// Realizes merger between two values.
  bool merge(const AllocaDFValue &with);

  /// Compares two values.
  bool operator==(const AllocaDFValue &RHS) const;

  /// Compares two values.
  bool operator!=(const AllocaDFValue &RHS) const { return !(*this == RHS); }
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
                       const AllocaDFValue &Value, ResultSet &Result) {
  AllocaDFValue::difference(AIBegin, AIEnd, Value, Result);
}

/// \brief This contains allocas which have outward exposed definitions or uses
/// in a data-flow node.
///
/// Let us use definitions from the article "Automatic Array Privatization"
/// written by Peng Tu and David Padua (page 6):
/// "A definition of variable v in a basic block S is said to be outward
/// exposed if it is the last definition of v in S. A use of v is outward
/// exposed if S does not contain a denition of v before this use". Note that
/// in case of loops allocas which have outward exposed uses can get value
/// not only outside the loop but also from previouse loop iterations.
class DefUseSet {
public:
  /// Set of alloca instructions.
  typedef llvm::SmallPtrSet<llvm::AllocaInst *, 64> AllocaSet;

  /// Returns allocas which have definitions in a data-flow node.
  const AllocaSet & getDefs() const { return mDefs; }

  /// Returns true if the specified alloca have definition in a data-flow node.
  bool hasDef(llvm::AllocaInst *AI) const { return mDefs.count(AI) != 0; }

  /// Specifies that an alloca have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addDef(llvm::AllocaInst *AI) { return mDefs.insert(AI); }

  /// Returns allocas which get values outside a data-flow node.
  const AllocaSet & getUses() const { return mUses; }  

  /// Returns true if the specified alloca get value outside a data-flow node.
  bool hasUse(llvm::AllocaInst *AI) const { return mUses.count(AI) != 0; }

  /// Specifies that an alloca get values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addUse(llvm::AllocaInst *AI) { return mUses.insert(AI); }
private:
  AllocaSet mDefs;
  AllocaSet mUses;
};

/// This attribute is associated with DefUseSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DefUseAttr, DefUseSet)

/// \brief This contains privatizability information for allocas
/// in a natural loop.


namespace detail {
/// This represents identifier of cells in DependencySet collection,
/// which is represented as a static list.
struct DependencySet {
  /// Set of alloca instructions.
  typedef llvm::SmallPtrSet<llvm::AllocaInst *, 64> AllocaSet;
  struct Private { typedef AllocaSet ValueType; };
  struct LastPrivate { typedef AllocaSet ValueType; };
  struct SecondToLastPrivate { typedef AllocaSet ValueType; };
  struct DynamicPrivate { typedef AllocaSet ValueType; };
  struct Shared { typedef AllocaSet ValueType; };
  struct Dependency { typedef AllocaSet ValueType; };
};
}

const detail::DependencySet::Private Private;
const detail::DependencySet::LastPrivate LastPrivate;
const detail::DependencySet::SecondToLastPrivate SecondToLastPrivate;
const detail::DependencySet::DynamicPrivate DynamicPrivate;
const detail::DependencySet::Shared Shared;
const detail::DependencySet::Dependency Dependency;

/// \brief This represents data dependency in loops.
///
/// The following information is avaliable:
/// - a set of private allocas;
/// - a set of last private allocas;
/// - a set of second to last private allocas;
/// - a set of dynamic private allocas;
/// - a set of shared allocas;
/// - a set of allocas that caused dependency.
///
/// Calculation of a last private variables differs depending on internal
/// representation of a loop. There are two type of representations.
/// -# The first type has a following pattern:
/// \code
/// iter: if (...) goto exit;
///           ...
///         goto iter;
/// exit:
/// \endcode
/// For example, representation of a for-loop refers to this type.
/// The candidates for last private variables associated with the for-loop
/// will be stored as second to last privates allocas, because 
/// the last definition of these allocas is executed on the second to the last
/// loop iteration (on the last iteration the loop condition
/// check is executed only).
/// -# The second type has a following pattern:
/// \code
/// iter:
///           ...
///       if (...) goto exit; else goto iter;
/// exit:
/// \endcode
/// For example, representation of a do-while-loop refers to this type.
/// In this case the candidates for last private variables
/// will be stored as last privates allocas.
///
/// In some cases it is impossible to determine in static an iteration
/// where the last definition of an alloca have been executed. Such allocas
/// will be stored as dynamic private allocas collection.
///
/// Let us give the following example to explain how to access the information:
/// \code
/// DependencySet DS;
/// for (AllocaInst *AI : DS[Private]) {...}
/// \endcode
/// Note, (*DS)[Private] is a set of type AllocaSet, so it is possible to call
/// all methods that is avaliable for AllocaSet.
/// You can also use LastPrivate, SecondToLastPrivate, DynamicPrivate instead of
/// Private to access the necessary kind of allocas.
class DependencySet: public CELL_COLL_6(
    detail::DependencySet::Private,
    detail::DependencySet::LastPrivate,
    detail::DependencySet::SecondToLastPrivate,
    detail::DependencySet::DynamicPrivate,
    detail::DependencySet::Shared,
    detail::DependencySet::Dependency) {
public:
  /// Set of alloca instructions.
  typedef detail::DependencySet::AllocaSet AllocaSet;

  /// \brief Checks that an alloca has a specified kind of privatizability.
  ///
  /// Usage: DependencySet *DS; DS->is(Private, AI);
  template<class Kind> bool is(Kind K, llvm::AllocaInst *AI) const {
    return (*this)[K].count(AI) != 0;
  }
};

/// This attribute is associated with DependencySet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DependencyAttr, DependencySet)

/// \brief Data-flow framework which is used to find candidates
/// in privatizable allocas for each natural loops.
///
/// The data-flow problem is solved in forward direction.
/// The result of this analysis should be complemented to separate private
/// from last private allocas. The reason for this is the scope of analysis.
/// The analysis is performed for loop bodies only.
///
/// Two kinds of attributes for each nodes in a data-flow graph is available
/// after this analysis. The first kind, is DefUseAttr and the second one is
/// PrivateDFAttr. The third kind of attribute (DependencyAttr) becomes available
/// for nodes which corresponds to natural loops. The complemented results
/// should be stored in the value of the DependencyAttr attribute.
class PrivateDFFwk : private Utility::Uncopyable {
public:
  /// Set of alloca instructions.
  typedef DependencySet::AllocaSet AllocaSet;

  /// Information about privatizability of variables for the analysed region.
  typedef llvm::DenseMap<llvm::Loop *, DependencySet *> PrivateInfo;

  /// Creates data-flow framework and specifies a set of allocas 
  /// that should be analyzed.
  explicit PrivateDFFwk(const AllocaSet &AnlsAllocas, PrivateInfo &PI) :
    mAnlsAllocas(AnlsAllocas), mPrivates(PI) {}

  /// Returns true if the specified alloca should be analyzed.
  bool isAnalyse(llvm::AllocaInst *AI) { return mAnlsAllocas.count(AI) != 0; }

  /// Collapses a data-flow graph which represents a region to a one node
  /// in a data-flow graph of an outer region.
  void collapse(DFRegion *R);

private:
  const AllocaSet &mAnlsAllocas;
  PrivateInfo &mPrivates;
};

/// This covers IN and OUT value for a privatizability analysis.
typedef DFValue<PrivateDFFwk, AllocaDFValue> PrivateDFValue;

/// This attribute is associated with PrivateDFValue and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(PrivateDFAttr, PrivateDFValue)

/// Traits for a data-flow framework which is used to find candidates
/// in privatizable allocas for each natural loops.
template<> struct DataFlowTraits<PrivateDFFwk *> {
  typedef Forward<DFRegion * > GraphType;
  typedef AllocaDFValue ValueType;
  static ValueType topElement(PrivateDFFwk *, GraphType) {
    return AllocaDFValue::fullValue();
  }
  static ValueType boundaryCondition(PrivateDFFwk *, GraphType) {
    return AllocaDFValue::emptyValue();
  }
  static void setValue(ValueType V, DFNode *N, PrivateDFFwk *) {
    assert(N && "Node must not be null!");
    PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
    assert(PV && "Data-flow value must not be null!");
    PV->setOut(std::move(V));
  }
  static const ValueType & getValue(DFNode *N, PrivateDFFwk *) {
    assert(N && "Node must not be null!");
    PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
    assert(PV && "Data-flow value must not be be null!");
    return PV->getOut();
  }
  static void initialize(DFNode *, PrivateDFFwk *, GraphType);
  static void meetOperator(
      const ValueType &LHS, ValueType &RHS, PrivateDFFwk *, GraphType) {
    RHS.intersect(LHS);
  }
  static bool transferFunction(ValueType, DFNode *, PrivateDFFwk *, GraphType);
};

/// Tratis for a data-flow framework which is used to find candidates
/// in privatizable allocas for each natural loops.
template<> struct RegionDFTraits<PrivateDFFwk *> :
    DataFlowTraits<PrivateDFFwk *> {
  static void expand(PrivateDFFwk *, GraphType) {}
  static void collapse(PrivateDFFwk *Fwk, GraphType G) {
    Fwk->collapse(G.Graph);
  }
  typedef DFRegion::region_iterator region_iterator;
  static region_iterator region_begin(GraphType G) {
    return G.Graph->region_begin();
  }
  static region_iterator region_end(GraphType G) {
    return G.Graph->region_end();
  }
};

/// \brief Data-flow framework which is used to find live allocas
/// for basic blocks, loops, functions, etc.
class LiveDFFwk : private Utility::Uncopyable {
public:
  /// Set of alloca instructions.
  typedef llvm::SmallPtrSet<llvm::AllocaInst *, 64> AllocaSet;

  /// Creates data-flow framework and specifies a set of allocas 
  /// that should be analyzed.
  explicit LiveDFFwk(const AllocaSet &AnlsAllocas) :
    mAnlsAllocas(AnlsAllocas) {}

  /// Returns true if the specified alloca should be analyzed.
  bool isAnalyse(llvm::AllocaInst *AI) { return mAnlsAllocas.count(AI) != 0; }

private:
  const AllocaSet &mAnlsAllocas;
};

/// This covers IN and OUT value for a live allocas analysis.
typedef DFValue<LiveDFFwk, LiveDFFwk::AllocaSet> LiveSet;

/// This attribute is associated with LiveSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(LiveAttr, LiveSet)

/// Traits for a data-flow framework which is used to find live allocas.
template<> struct DataFlowTraits<LiveDFFwk *> {
  typedef Backward<DFRegion * > GraphType;
  typedef LiveDFFwk::AllocaSet ValueType;
  static ValueType topElement(LiveDFFwk *, GraphType) { return ValueType(); }
  static ValueType boundaryCondition(LiveDFFwk *DFF, GraphType G) {
    LiveSet *LS = G.Graph->getAttribute<LiveAttr>();
    assert(LS && "Data-flow value must not be be null!");
    ValueType V(topElement(DFF, G));
    // If an alloca is alive before a loop it is alive befor each iteration.
    // This occurs due to conservatism of analysis.
    // If alloca is alive befor iteration with number I then it is alive after
    // iteration with number I-1. So it should be used as a boundary value.
    meetOperator(LS->getIn(), V, DFF, G);
    // If alloca is alive after a loop it also should be used as a boundary value.
    meetOperator(LS->getOut(), V, DFF, G);
    return V;
  }
  static void setValue(ValueType V, DFNode *N, LiveDFFwk *) {
    assert(N && "Node must not be null!");
    LiveSet *LS = N->getAttribute<LiveAttr>();
    assert(LS && "Data-flow value must not be null!");
    LS->setIn(std::move(V));
  }
  static const ValueType & getValue(DFNode *N, LiveDFFwk *) {
    assert(N && "Node must not be null!");
    LiveSet *LS = N->getAttribute<LiveAttr>();
    assert(LS && "Data-flow value must not be be null!");
    return LS->getIn();
  }
  static void initialize(DFNode *, LiveDFFwk *, GraphType);
  static void meetOperator(
      const ValueType &LHS, ValueType &RHS, LiveDFFwk *, GraphType) {
    RHS.insert(LHS.begin(), LHS.end());
  }
  static bool transferFunction(ValueType, DFNode *, LiveDFFwk *, GraphType);
};

/// Traits for a data-flow framework which is used to find live allocas.
template<> struct RegionDFTraits<LiveDFFwk *> :
  DataFlowTraits<LiveDFFwk *> {
  static void expand(LiveDFFwk *, GraphType G) {
    DFNode *LN = G.Graph->getLatchNode();
    if (!LN)
      return;
    DFNode *EN = G.Graph->getExitNode();
    LN->addSuccessor(EN);
    EN->addPredecessor(LN);
  }
  static void collapse(LiveDFFwk *, GraphType G) {
    DFNode *LN = G.Graph->getLatchNode();
    if (!LN)
      return;
    DFNode *EN = G.Graph->getExitNode();
    LN->removeSuccessor(EN);
    EN->removePredecessor(LN);
  }
  typedef DFRegion::region_iterator region_iterator;
  static region_iterator region_begin(GraphType G) {
    return G.Graph->region_begin();
  }
  static region_iterator region_end(GraphType G) {
    return G.Graph->region_end();
  }
};
}

namespace llvm {
class Loop;

/// This pass determines allocas which can be privatized.
class PrivateRecognitionPass :
    public FunctionPass, private Utility::Uncopyable {
  /// Set of alloca instructions.
  typedef tsar::PrivateDFFwk::AllocaSet AllocaSet;
  /// Information about privatizability of variables for the analysed region.
  typedef tsar::PrivateDFFwk::PrivateInfo PrivateInfo;
public:
  /// Pass identification, replacement for typeid
  static char ID; 

  /// Default constructor.
  PrivateRecognitionPass() : FunctionPass(ID) {
    initializePrivateRecognitionPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns privatizable allocas sorted according to kinds
  /// of their privatizability.
  const tsar::DependencySet & getPrivatesFor(Loop *L) const {
    assert(L && "Loop must not be null!");
    auto DS = mPrivates.find(L);
    assert((DS != mPrivates.end() || DS->second) &&
      "DependencySet must be specified!");
    return *DS->second;
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
  /// \brief Complements the result of loop bodies analysis.
  ///
  /// Privatizability analysis is performed in two steps. Firstly,
  /// body of each natural loop is analyzed. Secondly, when live allocas
  /// for each basic block are discovered, results of loop body analysis must be
  /// finalized. The result of this analysis should be complemented to separate
  /// private from last private allocas.
  /// \param [in, out] R Region in a data-flow graph, it can not be null.
  void resolveCandidats(tsar::DFRegion *R);

private:
  PrivateInfo mPrivates;
  AllocaSet mAnlsAllocas;
};
}
#endif//TSAR_PRIVATE_H
