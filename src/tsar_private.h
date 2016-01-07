//===--- tsar_private.h - Private Variable Analyzer --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines passes to determine locations which can be privatized.
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
#include "tsar_df_location.h"

namespace llvm {
class Loop;
class Value;
class AliasSetTracker;
}

namespace tsar {
/// \brief This contains locations which have outward exposed definitions or
/// uses in a data-flow node.
///
/// Let us use definitions from the article "Automatic Array Privatization"
/// written by Peng Tu and David Padua (page 6):
/// "A definition of variable v in a basic block S is said to be outward
/// exposed if it is the last definition of v in S. A use of v is outward
/// exposed if S does not contain a denition of v before this use". Note that
/// in case of loops locations which have outward exposed uses can get value
/// not only outside the loop but also from previouse loop iterations.
class DefUseSet {
public:
  /// Set of locations.
  typedef llvm::SmallPtrSet<llvm::Value *, 64> LocationSet;

  /// Returns locations which have definitions in a data-flow node.
  const LocationSet & getDefs() const { return mDefs; }

  /// Returns true if a location have definition in a data-flow node.
  bool hasDef(llvm::Value *Loc) const { return mDefs.count(Loc) != 0; }

  /// Specifies that a location have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addDef(llvm::Value *Loc) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mDefs.insert(Loc);
#else
    return mDefs.insert(Loc).second;
#endif
  }

  /// Returns locations which may have definitions in a data-flow node.
  ///
  /// May define locations arise in following cases:
  /// - a data-flow node is a region and encapsulates other nodes.
  /// The two locations may or may not alias. This is the least precise result.
  /// - a location may overlap (may alias) or partially overlaps (partial alias)
  /// with another location which is must/may define locations.
  const LocationSet & getMayDefs() const { return mMayDefs; }

  /// Returns true if a location may have definition in a data-flow node.
  bool hasMayDef(llvm::Value *Loc) const {
    return mMayDefs.count(Loc) != 0;
  }

  /// Specifies that a location may have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addMayDef(llvm::Value *Loc) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mMayDefs.insert(Loc);
#else
    return mMayDefs.insert(Loc).second;
#endif
  }

  /// Returns locations which get values outside a data-flow node.
  ///
  /// The result contains also may use locations because conservativness
  /// of analysis must be preserved.
  const LocationSet & getUses() const { return mUses; }

  /// Returns true if a location gets value outside a data-flow node.
  bool hasUse(llvm::Value *Loc) const { return mUses.count(Loc) != 0; }

  /// Specifies that a location gets values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addUse(llvm::Value *Loc) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mUses.insert(Loc);
#else
    return mUses.insert(Loc).second;
#endif
  }

  /// Returns locations accesses to which are performed explicitly.
  ///
  /// For example, if p = &x and to access x, *p is used, let us assume that
  /// access to x is performed implicitly and access to *p is performed
  /// explicitly.
  const LocationSet & getExplicitAccesses() const { return mExplicitAccesses; }

  /// Returns true if there are an explicit access to a location in the node.
  bool hasExplicitAccess(llvm::Value *Loc) const {
    return mExplicitAccesses.count(Loc) != 0;
  }

  /// Specifies that there are an explicit access to a location in the node.
  ///
  /// \return False if it has been already specified.
  bool addExplicitAccess(llvm::Value *Loc) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mExplicitAccesses.insert(Loc);
#else
    return mExplicitAccesses.insert(Loc).second;
#endif
  }

  /// Returns allocas addresses of which are explicitly evaluated in the node.
  ///
  /// For example, if &x expression occures in the node then address of
  /// the x alloca is evaluated. It means that this alloca can not be privatized
  /// because the original alloca address is used.
  const LocationSet & getAddressAccesses() const { return mAddressAccesses; }

  /// Returns true if there are evaluation of a alloca address in the node.
  bool hasAddressAccess(llvm::Value *Loc) const {
    return mAddressAccesses.count(Loc) != 0;
  }

  /// Specifies that there are evaluation of a alloca address in the node.
  ///
  /// \return False if it has been already specified.
  bool addAddressAccess(llvm::Value *Loc) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mAddressAccesses.insert(Loc);
#else
    return mAddressAccesses.insert(Loc).second;
#endif
  }
private:
  LocationSet mDefs;
  LocationSet mMayDefs;
  LocationSet mUses;
  LocationSet mExplicitAccesses;
  LocationSet mAddressAccesses;
};

/// This attribute is associated with DefUseSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DefUseAttr, DefUseSet)

namespace detail {
/// This represents identifier of cells in DependencySet collection,
/// which is represented as a static list.
struct DependencySet {
  /// Set of locations.
  typedef DefUseSet::LocationSet LocationSet;
  struct Private { typedef LocationSet ValueType; };
  struct LastPrivate { typedef LocationSet ValueType; };
  struct SecondToLastPrivate { typedef LocationSet ValueType; };
  struct DynamicPrivate { typedef LocationSet ValueType; };
  struct FirstPrivate { typedef LocationSet ValueType; };
  struct Shared { typedef LocationSet ValueType; };
  struct Dependency { typedef LocationSet ValueType; };
};
}

const detail::DependencySet::Private Private;
const detail::DependencySet::LastPrivate LastPrivate;
const detail::DependencySet::SecondToLastPrivate SecondToLastPrivate;
const detail::DependencySet::DynamicPrivate DynamicPrivate;
const detail::DependencySet::FirstPrivate FirstPrivate;
const detail::DependencySet::Shared Shared;
const detail::DependencySet::Dependency Dependency;

/// \brief This represents data dependency in loops.
///
/// The following information is avaliable:
/// - a set of private locations;
/// - a set of last private locations;
/// - a set of second to last private locations;
/// - a set of dynamic private locations;
/// - a set of first private locations;
/// - a set of shared locations;
/// - a set of locations that caused dependency.
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
/// will be stored as second to last privates locations, because 
/// the last definition of these locations is executed on the second to the last
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
/// will be stored as last privates locations.
///
/// In some cases it is impossible to determine in static an iteration
/// where the last definition of an location have been executed. Such locations
/// will be stored as dynamic private locations collection.
///
/// Let us give the following example to explain how to access the information:
/// \code
/// DependencySet DS;
/// for (Value *Loc : DS[Private]) {...}
/// \endcode
/// Note, (*DS)[Private] is a set of type LocationSet, so it is possible to call
/// all methods that is avaliable for LocationSet.
/// You can also use LastPrivate, SecondToLastPrivate, DynamicPrivate instead of
/// Private to access the necessary kind of locations.
class DependencySet: public CELL_COLL_7(
    detail::DependencySet::Private,
    detail::DependencySet::LastPrivate,
    detail::DependencySet::SecondToLastPrivate,
    detail::DependencySet::DynamicPrivate,
    detail::DependencySet::FirstPrivate,
    detail::DependencySet::Shared,
    detail::DependencySet::Dependency) {
public:
  /// Set of locations.
  typedef detail::DependencySet::LocationSet LocationSet;

  /// \brief Checks that a location has a specified kind of privatizability.
  ///
  /// Usage: DependencySet *DS; DS->is(Private, Loc);
  template<class Kind> bool is(Kind K, llvm::Value *Loc) const {
    return (*this)[K].count(Loc) != 0;
  }
};

/// This attribute is associated with DependencySet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DependencyAttr, DependencySet)

/// \brief Data-flow framework which is used to find candidates
/// in privatizable locations for each natural loops.
///
/// The data-flow problem is solved in forward direction.
/// The result of this analysis should be complemented to separate private
/// from last private locations. The reason for this is the scope of analysis.
/// The analysis is performed for loop bodies only.
///
/// Two kinds of attributes for each nodes in a data-flow graph are available
/// after this analysis. The first kind, is DefUseAttr and the second one is
/// PrivateDFAttr. The third kind of attribute (DependencyAttr) becomes available
/// for nodes which corresponds to natural loops. The complemented results
/// should be stored in the value of the DependencyAttr attribute.
class PrivateDFFwk : private Utility::Uncopyable {
public:
  /// Set of locations.
  typedef DependencySet::LocationSet LocationSet;

  /// Information about privatizability of variables for the analysed region.
  typedef llvm::DenseMap<llvm::Loop *, DependencySet *> PrivateInfo;

  /// Creates data-flow framework.
  explicit PrivateDFFwk(llvm::AliasSetTracker *AT, PrivateInfo &PI) :
    mAliasTracker(AT), mPrivates(PI) {
    assert(mAliasTracker && "AliasSetTracker must not be null!"); 
  }

  /// Returns a tracker for sets of aliases.
  llvm::AliasSetTracker * getTracker() const { return mAliasTracker; }

  /// Collapses a data-flow graph which represents a region to a one node
  /// in a data-flow graph of an outer region.
  void collapse(DFRegion *R);

private:
  llvm::AliasSetTracker *mAliasTracker;
  PrivateInfo &mPrivates;
};

/// This covers IN and OUT value for a privatizability analysis.
typedef DFValue<PrivateDFFwk, LocationDFValue> PrivateDFValue;

/// This attribute is associated with PrivateDFValue and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(PrivateDFAttr, PrivateDFValue)

/// Traits for a data-flow framework which is used to find candidates
/// in privatizable locations for each natural loops.
template<> struct DataFlowTraits<PrivateDFFwk *> {
  typedef Forward<DFRegion * > GraphType;
  typedef LocationDFValue ValueType;
  static ValueType topElement(PrivateDFFwk *, GraphType) {
    return LocationDFValue::fullValue();
  }
  static ValueType boundaryCondition(PrivateDFFwk *, GraphType) {
    return LocationDFValue::emptyValue();
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
/// in privatizable locations for each natural loops.
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

/// \brief Data-flow framework which is used to find live locations
/// for basic blocks, loops, functions, etc.
///
/// \pre The outward exposed uses and definitions must be calculated, so
/// PrivateDFFwk should be used to prepare data-flow graph before evaluation.
///
/// The LiveAttr attribute for each nodes in a data-flow graph is available
/// after this analysis.
class LiveDFFwk : private Utility::Uncopyable {
public:
  /// Set of locations.
  typedef DefUseSet::LocationSet LocationSet;

  /// Creates data-flow framework.
  explicit LiveDFFwk(llvm::AliasSetTracker *AT) : mAliasTracker(AT) {
    assert(mAliasTracker && "AliasSetTracker must not be null!");
  }

private:
  llvm::AliasSetTracker *mAliasTracker;
};

/// This covers IN and OUT value for a live locations analysis.
typedef DFValue<LiveDFFwk, LiveDFFwk::LocationSet> LiveSet;

/// This attribute is associated with LiveSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(LiveAttr, LiveSet)

/// Traits for a data-flow framework which is used to find live locations.
template<> struct DataFlowTraits<LiveDFFwk *> {
  typedef Backward<DFRegion * > GraphType;
  typedef LiveDFFwk::LocationSet ValueType;
  static ValueType topElement(LiveDFFwk *, GraphType) { return ValueType(); }
  static ValueType boundaryCondition(LiveDFFwk *DFF, GraphType G) {
    LiveSet *LS = G.Graph->getAttribute<LiveAttr>();
    assert(LS && "Data-flow value must not be be null!");
    ValueType V(topElement(DFF, G));
    // If a location is alive before a loop it is alive befor each iteration.
    // This occurs due to conservatism of analysis.
    // If a location is alive befor iteration with number I then it is alive
    // after iteration with number I-1. So it should be used as a boundary
    // value.
    meetOperator(LS->getIn(), V, DFF, G);
    // If a location is alive after a loop it also should be used as a boundary
    // value.
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

/// Traits for a data-flow framework which is used to find live locations.
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
/// This pass determines locations which can be privatized.
class PrivateRecognitionPass :
    public FunctionPass, private Utility::Uncopyable {
  /// Set of locations.
  typedef tsar::PrivateDFFwk::LocationSet LocationSet;
  /// Information about privatizability of variables for the analysed region.
  typedef tsar::PrivateDFFwk::PrivateInfo PrivateInfo;
public:
  /// Pass identification, replacement for typeid
  static char ID; 

  /// Default constructor.
  PrivateRecognitionPass() : FunctionPass(ID) {
    initializePrivateRecognitionPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns privatizable locations sorted according to kinds
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
  /// body of each natural loop is analyzed. Secondly, when live locations
  /// for each basic block are discovered, results of loop body analysis must be
  /// finalized. The result of this analysis should be complemented to separate
  /// private from last private locations. The case where location access
  /// is performed by pointer is also considered.
  /// \param [in, out] R Region in a data-flow graph, it can not be null.
  void resolveCandidats(tsar::DFRegion *R);

private:
  PrivateInfo mPrivates;
  AliasSetTracker *mAliasTracker;
};
}
#endif//TSAR_PRIVATE_H
