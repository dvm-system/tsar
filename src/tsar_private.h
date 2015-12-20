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
#include "tsar_df_alloca.h"

namespace tsar {
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
  bool addDef(llvm::AllocaInst *AI) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mDefs.insert(AI);
#else
    return mDefs.insert(AI).second;
#endif
  }

  /// Returns allocas which get values outside a data-flow node.
  const AllocaSet & getUses() const { return mUses; }  

  /// Returns true if the specified alloca get value outside a data-flow node.
  bool hasUse(llvm::AllocaInst *AI) const { return mUses.count(AI) != 0; }

  /// Specifies that an alloca get values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addUse(llvm::AllocaInst *AI) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mUses.insert(AI);
#else
    return mUses.insert(AI).second;
#endif
  }
private:
  AllocaSet mDefs;
  AllocaSet mUses;
};

/// This attribute is associated with DefUseSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DefUseAttr, DefUseSet)

namespace detail {
/// This represents identifier of cells in DependencySet collection,
/// which is represented as a static list.
struct DependencySet {
  /// Set of alloca instructions.
  typedef DefUseSet::AllocaSet AllocaSet;
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
/// Two kinds of attributes for each nodes in a data-flow graph are available
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
///
/// \pre The outward exposed uses and definitions must be calculated, so
/// PrivateDFFwk should be used to prepare data-flow graph before evaluation.
///
/// The LiveAttr attribute for each nodes in a data-flow graph is available
/// after this analysis.
class LiveDFFwk : private Utility::Uncopyable {
public:
  /// Set of alloca instructions.
  typedef DefUseSet::AllocaSet AllocaSet;

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
