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
#include <llvm/IR/ValueMap.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Pass.h>
#ifdef DEBUG
#include <llvm/IR/Instruction.h>
#endif//DEBUG
#include <utility.h>
#include <cell.h>
#include "tsar_df_graph.h"
#include "tsar_df_location.h"
#include <list>

using Utility::operator "" _b;

namespace llvm {
class Loop;
class Value;
class Instruction;
class StoreInst;
class AliasAnalysis;
struct AAMDNodes;
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
  /// Set of pointers to locations.
  typedef llvm::SmallPtrSet<llvm::Value *, 64> PointerSet;

  /// Set of instructions.
  typedef llvm::SmallPtrSet<llvm::Instruction *, 64> InstructionSet;

  /// Constructor.
  DefUseSet(llvm::AliasAnalysis &AA) : mExplicitAccesses(AA) {}

  /// Returns set of the must defined locations.
  const LocationSet & getDefs() const { return mDefs; }

  /// Returns true if a location have definition in a data-flow node.
  ///
  /// \attention This method does not use alias information.
  bool hasDef(const llvm::MemoryLocation &Loc) const {
    return mDefs.contain(Loc);
  }

  /// Specifies that a location has definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addDef(const llvm::MemoryLocation &Loc) {
    return mDefs.insert(Loc).second;
  }

  /// Specifies that a stored location have definition in a data-flow node.
  ///
  /// \return True if a new alias set has been created.
  bool addDef(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(llvm::isa<llvm::StoreInst>(I) &&
      "Only store instructions produce must defined locations!");
    return addDef(llvm::MemoryLocation::get(I));
  }

  /// Returns set of the may defined locations.
  const LocationSet & getMayDefs() const { return mMayDefs; }

  /// Returns true if a location may have definition in a data-flow node.
  ///
  /// May define locations arise in following cases:
  /// - a data-flow node is a region and encapsulates other nodes.
  /// It is necessary to use this conservative assumption due to complexity of
  /// CFG analysis.
  /// - a location may overlap (may alias) or partially overlaps (partial alias)
  /// with another location which is must/may define locations.
  /// \attention
  /// - This method does not use alias information.
  /// - This method returns true even if only part of the location may have
  /// definition.
  bool hasMayDef(const llvm::MemoryLocation &Loc) const {
    return mMayDefs.overlap(Loc);
  }

  /// Specifies that a location may have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addMayDef(const llvm::MemoryLocation &Loc) {
    return mMayDefs.insert(Loc).second;
  }

  /// Specifies that a modified location may have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  /// \pre The specified instruction may modify memory.
  bool addMayDef(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(I->mayWriteToMemory() && "Instrcution does not modify memory!");
    return addMayDef(llvm::MemoryLocation::get(I));
  }

  /// Returns set of the locations which get values outside a data-flow node.
  const LocationSet & getUses() const { return mUses; }

  /// Returns true if a location gets value outside a data-flow node.
  ///
  /// May use locations should be also counted because conservativness
  /// of analysis must be preserved.
  /// \attention
  /// - This method does not use alias information.
  /// - This method returns true even if only part of the location
  /// get values outside a data-flow node.
  bool hasUse(const llvm::MemoryLocation &Loc) const {
    return mUses.overlap(Loc);
  }

  /// Specifies that a location gets values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addUse(const llvm::MemoryLocation &Loc) {
    return mUses.insert(Loc).second;
  }

  /// Specifies that a location gets values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  /// \pre The specified instruction may read memory.
  bool addUse(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(I->mayReadFromMemory() && "Instrcution does not read memory!");
    return addUse(llvm::MemoryLocation::get(I));
  }

  /// Returns locations accesses to which are performed explicitly.
  ///
  /// For example, if p = &x and to access x, *p is used, let us assume that
  /// access to x is performed implicitly and access to *p is performed
  /// explicitly.
  const llvm::AliasSetTracker & getExplicitAccesses() const {
    return mExplicitAccesses;
  }

  /// Returns true if there are an explicit access to a location in the node.
  ///
  /// \attention This method returns true even if only part of the location
  /// has explicit access.
  bool hasExplicitAccess(const llvm::MemoryLocation &Loc) const;

  /// Specifies that there are an explicit access to a location in the node.
  ///
  /// \return True if a new alias set has been created.
  bool addExplicitAccess(const llvm::MemoryLocation &Loc) {
    assert(Loc.Ptr && "Pointer to memory location must not be null!");
    return mExplicitAccesses.add(
      const_cast<llvm::Value *>(Loc.Ptr), Loc.Size, Loc.AATags);
  }

  /// Specifies that there are an explicit access to a location in the node.
  ///
  /// \return True if a new alias set has been created.
  /// \pre The specified instruction may read or modify memory.
  bool addExplicitAccess(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(I->mayReadOrWriteMemory() &&
      "Instrcution does not read nor write memory!");
    return mExplicitAccesses.add(I);
  }

  /// Specifies that accesses to all locations from AST are performed
  /// explicitly.
  void addExplicitAccesses(const llvm::AliasSetTracker &AST) {
    mExplicitAccesses.add(AST);
  }

  /// Returns locations addresses of which are explicitly evaluated in the node.
  ///
  /// For example, if &x expression occures in the node then address of
  /// the x alloca is evaluated. It means that regardless of whether the
  /// location will be privatized the original location address should be
  /// available.
  const PointerSet & getAddressAccesses() const { return mAddressAccesses; }

  /// Returns true if there are evaluation of a location address in the node.
  bool hasAddressAccess(llvm::Value *Ptr) const {
    assert(Ptr && "Pointer to memory location must not be null!");
    return mAddressAccesses.count(Ptr) != 0;
  }

  /// Specifies that there are evaluation of a location address in the node.
  ///
  /// \return False if it has been already specified.
  bool addAddressAccess(llvm::Value *Ptr) {
    assert(Ptr && "Pointer to memory location must not be null!");
    return mAddressAccesses.insert(Ptr).second;
  }

  /// Returns unknown instructions which are evaluated in the node.
  ///
  /// An unknown instruction is a instruction which accessed memory with unknown
  /// description. For example, in general case call instruction is an unknown
  /// instruction.
  const InstructionSet & getUnknownInsts() const { return mUnknownInsts; }

  /// Returns true if there are an unknown instructions in the node.
  bool hasUnknownInst(llvm::Instruction *I) const {
    assert(I && "Instruction must not be null!");
    return mUnknownInsts.count(I) != 0;
  }

  /// Specifies that there are unknown instructions in the node.
  ///
  /// \return False if it has been already specified.
  bool addUnknownInst(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    return mUnknownInsts.insert(I).second;
  }

private:
  LocationSet mDefs;
  LocationSet mMayDefs;
  LocationSet mUses;
  llvm::AliasSetTracker mExplicitAccesses;
  PointerSet mAddressAccesses;
  InstructionSet mUnknownInsts;
};

/// This attribute is associated with DefUseSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DefUseAttr, DefUseSet)

namespace detail {
/// Declaration of a trait recognized by analyzer.
#define TSAR_TRAIT_DEF(name_, id_, collection_) \
struct name_ { static constexpr auto Id = id_; typedef collection_ ValueType; };

/// This represents identifier of cells in DependencySet collection,
/// which is represented as a static list.
struct DependencySet {
  /// List of locations with an appropriate trait.
  typedef std::list<const llvm::MemoryLocation *> LocationList;
  struct Analyze { typedef BaseLocationSet ValueType; };
  TSAR_TRAIT_DEF(NoAccess, 1111111_b, LocationList)
  TSAR_TRAIT_DEF(AddressAccess, 1011111_b, LocationList)
  TSAR_TRAIT_DEF(Shared, 0111110_b, LocationList)
  TSAR_TRAIT_DEF(Private, 0101111_b, LocationList)
  TSAR_TRAIT_DEF(FirstPrivate, 0101110_b, LocationList)
  TSAR_TRAIT_DEF(SecondToLastPrivate, 0101011_b, LocationList)
  TSAR_TRAIT_DEF(LastPrivate, 0100111_b, LocationList)
  TSAR_TRAIT_DEF(DynamicPrivate, 0100011_b, LocationList)
  TSAR_TRAIT_DEF(Dependency, 0100000_b, LocationList)
};
}

constexpr detail::DependencySet::Analyze Analyze;
constexpr detail::DependencySet::NoAccess NoAccess;
constexpr detail::DependencySet::AddressAccess AddressAccess;
constexpr detail::DependencySet::Private Private;
constexpr detail::DependencySet::LastPrivate LastPrivate;
constexpr detail::DependencySet::SecondToLastPrivate SecondToLastPrivate;
constexpr detail::DependencySet::DynamicPrivate DynamicPrivate;
constexpr detail::DependencySet::FirstPrivate FirstPrivate;
constexpr detail::DependencySet::Shared Shared;
constexpr detail::DependencySet::Dependency Dependency;

/// \brief This represents data dependency in loops.
///
/// The following information is available:
/// - a set of analyzed locations;
/// - a set of locations addresses of which are evaluated;
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
/// all methods that is available for LocationSet.
/// You can also use LastPrivate, SecondToLastPrivate, DynamicPrivate instead of
/// Private to access the necessary kind of locations.
class DependencySet: public CELL_COLL_9(
    detail::DependencySet::Analyze,
    detail::DependencySet::AddressAccess,
    detail::DependencySet::Private,
    detail::DependencySet::LastPrivate,
    detail::DependencySet::SecondToLastPrivate,
    detail::DependencySet::DynamicPrivate,
    detail::DependencySet::FirstPrivate,
    detail::DependencySet::Shared,
    detail::DependencySet::Dependency) {
public:
  /// Set of locations.
  typedef detail::DependencySet::LocationList LocationList;

  /// \brief Checks that a location has a specified kind of privatizability.
  ///
  /// Usage: DependencySet *DS; DS->is(Private, Loc);
  template<class Kind> bool is(Kind K, const llvm::MemoryLocation *Loc) const {
    return (*this)[K].count(Loc) != 0;
  }
};

/// This attribute is associated with DependencySet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(DependencyAttr, DependencySet)

/// \brief Data-flow framework which is used to find must defined locations
/// for each natural loops.
///
/// The data-flow problem is solved in forward direction.
/// The analysis is performed for loop bodies only.
///
/// Two kinds of attributes for each nodes in a data-flow graph are available
/// after this analysis. The first kind, is DefUseAttr and the second one is
/// PrivateDFAttr.
class PrivateDFFwk : private Utility::Uncopyable {
public:
  /// Creates data-flow framework.
  explicit PrivateDFFwk(llvm::AliasSetTracker *AST) :
    mAliasTracker(AST){
    assert(mAliasTracker && "AliasSetTracker must not be null!"); 
  }

  /// Returns a tracker for sets of aliases.
  llvm::AliasSetTracker * getTracker() const { return mAliasTracker; }

  /// Collapses a data-flow graph which represents a region to a one node
  /// in a data-flow graph of an outer region.
  void collapse(DFRegion *R);

private:
  llvm::AliasSetTracker *mAliasTracker;
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
  /// Creates data-flow framework.
  explicit LiveDFFwk(llvm::AliasSetTracker *AST) : mAliasTracker(AST) {
    assert(mAliasTracker && "AliasSetTracker must not be null!");
  }

private:
  llvm::AliasSetTracker *mAliasTracker;
};

/// This covers IN and OUT value for a live locations analysis.
typedef DFValue<LiveDFFwk, LocationSet> LiveSet;

/// This attribute is associated with LiveSet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(LiveAttr, LiveSet)

/// Traits for a data-flow framework which is used to find live locations.
template<> struct DataFlowTraits<LiveDFFwk *> {
  typedef Backward<DFRegion * > GraphType;
  typedef LocationSet ValueType;
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
  /// Information about privatizability of variables for the analysed region.
  typedef llvm::DenseMap<llvm::Loop *, tsar::DependencySet *> PrivateInfo;
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

  /// Releases allocated memory.
  void releaseMemory() override {
    for (auto &Pair : mPrivates)
      delete Pair.second;
    mPrivates.clear();
  }

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  /// \brief Implements recognition of privatizable locations.
  ///
  /// Privatizability analysis is performed in two steps. Firstly,
  /// body of each natural loop is analyzed. Secondly, when live locations
  /// for each basic block are discovered, results of loop body analysis must be
  /// finalized. The result of this analysis should be complemented to separate
  /// private from last private locations. The case where location access
  /// is performed by pointer is also considered. Shared locations also
  /// analyzed.
  /// \param [in, out] R Region in a data-flow graph, it can not be null.
  /// \pre Def-use and live analysis have been performed for the region.
  void resolveCandidats(tsar::DFRegion *R);


  /// Releases memory allocated for attributes in a data-flow graph.
  void releaseMemory(tsar::DFRegion *R);

private:
  PrivateInfo mPrivates;
  AliasSetTracker *mAliasTracker;
};
}
#endif//TSAR_PRIVATE_H
