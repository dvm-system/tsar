//===--- DefinedMemory.h --- Defined Memory Analysis ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file defines passes to determine must/may defined locations for each
// data-flow region. We use data-flow framework to implement this kind of
// analysis. This filecontains elements which is necessary to determine this
// framework.
// The following articles can be helpful to understand it:
//  * "Automatic Array Privatization" Peng Tu and David Padua
//  * "Array Privatization for Parallel Execution of Loops" Zhiyuan Li.
// Note that each location in obtained sets has been stripped to the nearest
// estimate location (see tsar::stripToBase() function).
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DEFINED_MEMORY_H
#define TSAR_DEFINED_MEMORY_H

#include "tsar/ADT/DataFlow.h"
#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/DFMemoryLocation.h"
#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/AliasSetTracker.h>
#ifdef LLVM_DEBUG
# include <llvm/IR/Instruction.h>
#endif//DEBUG
#include <llvm/Pass.h>

namespace llvm {
class DominatorTree;
class Value;
class Instruction;
class StoreInst;
class TargetLibraryInfo;
class ScalarEvolution;
}

namespace tsar {
class AliasTree;
class DFRegionInfo;
class DelinearizeInfo;
struct GlobalOptions;

/// \brief This contains locations which have outward exposed definitions or
/// uses in a data-flow node.
///
/// Let us use definitions from the article "Automatic Array Privatization"
/// written by Peng Tu and David Padua (page 6):
/// "A definition of variable v in a basic block S is said to be outward
/// exposed if it is the last definition of v in S. A use of v is outward
/// exposed if S does not contain a definition of v before this use". Note that
/// in case of loops locations which have outward exposed uses can get value
/// not only outside the loop but also from previous loop iterations.
class DefUseSet {
public:
  /// Set of pointers to locations.
  typedef llvm::SmallPtrSet<llvm::Value *, 32> PointerSet;

  /// Set of instructions.
  typedef llvm::SmallPtrSet<llvm::Instruction *, 32> InstructionSet;

  // Set of pointers accessed in instructions to be attaced.
  typedef llvm::SmallDenseMap<llvm::Value *,
                              llvm::SmallVector<llvm::Instruction *, 8>, 32>
      TransitiveMap;

  /// Set of memory locations.
  typedef MemorySet<MemoryLocationRange> LocationSet;

  /// Returns set of the must defined locations.
  const LocationSet & getDefs() const { return mDefs; }

  /// Returns true if a location have definition in a data-flow node.
  ///
  /// \attention This method does not use alias information.
  bool hasDef(const MemoryLocationRange &Loc) const {
    return mDefs.contain(Loc);
  }

  /// Specifies that a location has definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addDef(const MemoryLocationRange &Loc) {
    return mDefs.insert(Loc).second;
  }

  /// Specifies that a stored location have definition in a data-flow node.
  ///
  /// \return True if a new alias set has been created.
  bool addDef(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(llvm::isa<llvm::StoreInst>(I) &&
      "Only store instructions produce must defined locations!");
    return addDef(MemoryLocationRange::get(I));
  }

  /// Specifies that all locations in a set have definition in a data-flow node.
  ///
  /// \return False if all locations have been already specified.
  bool addDefs(const LocationSet &Defs) {
    return mDefs.insert(Defs.begin(), Defs.end());
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
  bool hasMayDef(const MemoryLocationRange &Loc) const {
    return mMayDefs.overlap(Loc);
  }

  /// Specifies that a location may have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addMayDef(const MemoryLocationRange &Loc) {
    return mMayDefs.insert(Loc).second;
  }

  /// Specifies that a modified location may have definition in a data-flow node.
  ///
  /// \return False if it has been already specified.
  /// \pre The specified instruction may modify memory.
  bool addMayDef(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(I->mayWriteToMemory() && "Instruction does not modify memory!");
    return addMayDef(MemoryLocationRange::get(I));
  }

  /// Specifies that all locations in a set may have definition in a data-flow
  /// node.
  ///
  /// \return False if all locations have been already specified.
  bool addMayDefs(const LocationSet &MayDefs) {
    return mMayDefs.insert(MayDefs.begin(), MayDefs.end());
  }

  /// Returns set of the locations which get values outside a data-flow node.
  const LocationSet & getUses() const { return mUses; }

  /// Returns true if a location gets value outside a data-flow node.
  ///
  /// May use locations should be also counted because conservativeness
  /// of analysis must be preserved.
  /// \attention
  /// - This method does not use alias information.
  /// - This method returns true even if only part of the location
  /// get values outside a data-flow node.
  bool hasUse(const MemoryLocationRange &Loc) const {
    return mUses.overlap(Loc);
  }

  /// Specifies that a location gets values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  bool addUse(const MemoryLocationRange &Loc) {
    return mUses.insert(Loc).second;
  }

  /// Specifies that a location gets values outside a data-flow node.
  ///
  /// \return False if it has been already specified.
  /// \pre The specified instruction may read memory.
  bool addUse(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(I->mayReadFromMemory() && "Instruction does not read memory!");
    return addUse(MemoryLocationRange::get(I));
  }

  /// Specifies that all locations in a set get values outside a data-flow node.
  ///
  /// \return False if all locations have been already specified.
  bool addUses(const LocationSet &Uses) {
    return mUses.insert(Uses.begin(), Uses.end());
  }

  /// Returns locations accesses to which are performed explicitly.
  ///
  /// For example, if p = &x and to access x, *p is used, let us assume that
  /// access to x is performed implicitly and access to *p is performed
  /// explicitly.
  const LocationSet & getExplicitAccesses() const {
    return mExplicitAccesses;
  }

  /// Returns true if there are an explicit access to a location in the node.
  ///
  /// \attention This method returns true even if only part of the location
  /// has explicit access.
  bool hasExplicitAccess(const MemoryLocationRange &Loc) const {
    assert(Loc.Ptr && "Pointer to memory location must not be null!");
    return mExplicitAccesses.overlap(Loc);
  }

  /// Specifies that there are an explicit access to a location in the node.
  void addExplicitAccess(const MemoryLocationRange &Loc) {
    assert(Loc.Ptr && "Pointer to memory location must not be null!");
    mExplicitAccesses.insert(Loc);
  }

  /// Specifies that there are an explicit access to all locations from a
  /// specified set.
  void addExplicitAccesses(const LocationSet &Accesses) {
    mExplicitAccesses.insert(Accesses.begin(), Accesses.end());
  }

  /// Returns locations addresses of which are explicitly evaluated in the node.
  ///
  /// For example, if &x expression occurs in the node then address of
  /// the x 'alloca' is evaluated. It means that regardless of whether the
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
  /// Unknown instructions may access memory which is differ from objects
  /// pointed to by their pointer-typed arguments.
  const InstructionSet & getUnknownInsts() const { return mUnknownInsts; }

  /// \brief Returns true if there are an unknown instructions in the node.
  ///
  /// Unknown instructions may access memory which is differ from objects
  /// pointed to by their pointer-typed arguments.
  bool hasUnknownInst(llvm::Instruction *I) const {
    assert(I && "Instruction must not be null!");
    return mUnknownInsts.count(I) != 0;
  }

  /// \brief Specifies that there are unknown instructions in the node.
  ///
  /// Unknown instructions may access memory which is differ from objects
  /// pointed to by their pointer-typed arguments.
  /// \return False if it has been already specified.
  bool addUnknownInst(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    return mUnknownInsts.insert(I).second;
  }

  /// Returns explicitly called instructions which accesses unknown memory.
  const InstructionSet & getExplicitUnknowns() const {
    return mExplicitUnknowns;
  }

  /// Returns true if there are an explicit access to an unknown location.
  bool hasExplicitUnknown(llvm::Instruction *I) const {
    assert(I && "Instruction must not be null!");
    return mExplicitUnknowns.count(I) != 0;
  }

  /// \brief Specifies that there are an explicit access to an unknown location
  /// in the node.
  ///
  /// \pre The specified instruction may read or modify memory which is differ
  /// from objects pointed to by their pointer-typed arguments.
  void addExplicitUnknown(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    assert(I->mayReadOrWriteMemory() &&
      "Instruction does not read nor write memory!");
    mExplicitUnknowns.insert(I);
  }

  /// Specifies that there are an explicit access to all unknown locations from
  /// a specified set.
  void addExplicitUnknowns(const InstructionSet &Accesses) {
    mExplicitUnknowns.insert(Accesses.begin(), Accesses.end());
  }

  /// Returns list of instructions which may access address of unknown memory.
  ///
  /// For example, even if a function call does not access any memory location
  /// it may accesses some addresses of a memory location.
  const InstructionSet & getAddressUnknowns() const { return mAddressUnknowns; }

  /// Returns true if there are evaluation of an unknown address in the node.
  bool hasAddressUnknowns(llvm::Instruction *I) const {
    assert(I && "Instruction must not be null!");
    return mAddressUnknowns.count(I) != 0;
  }

  /// Specifies that there are evaluation of an unknown address in the node.
  ///
  /// \return False if it has been already specified.
  bool addAddressUnknowns(llvm::Instruction *I) {
    assert(I && "Instruction must not be null!");
    return mAddressUnknowns.insert(I).second;
  }

  /// Returns locations addresses of which are transitively evaluated in the
  /// node.
  const TransitiveMap &getAddressTransitives() const {
    return mAddressTransitives;
  }

  /// Return `true` if there are transitive evaluation of an address in the
  /// node.
  bool hasAddressTransitives(llvm::Value *Ptr) const {
    assert(Ptr && "Pointer must not be null!");
    return mAddressTransitives.count(Ptr) != 0;
  }

  /// Specifie that there are transitive evaluation of an address in the node.
  void addAddressTransitives(llvm::Value *Ptr, llvm::Instruction *I) {
    assert(Ptr && "Pointer must not be null!");
    return mAddressTransitives.try_emplace(Ptr).first->second.push_back(I);
  }

  /// Specifie that there are transitive evaluation of an address in the node.
  ///
  /// \return False if it has been already specified.
  bool addAddressTransitives(llvm::Value *Ptr,
                             llvm::ArrayRef<llvm::Instruction *> Insts) {
    assert(Ptr && "Pointer must not be null!");
    return mAddressTransitives.try_emplace(Ptr, Insts.begin(), Insts.end())
        .second;
  }

private:
  LocationSet mDefs;
  LocationSet mMayDefs;
  LocationSet mUses;
  LocationSet mExplicitAccesses;
  PointerSet mAddressAccesses;
  InstructionSet mUnknownInsts;
  InstructionSet mExplicitUnknowns;
  InstructionSet mAddressUnknowns;
  TransitiveMap mAddressTransitives;
};

/// This presents information whether a location has definition after a node
/// in a data-flow graph.
struct DefinitionInfo {
  LocationDFValue MustReach;
  LocationDFValue MayReach;
};

/// \brief Data-flow framework which is used to find must defined locations
/// for each natural loops.
///
/// The data-flow problem is solved in forward direction.
/// The analysis is performed for loop bodies only.
///
/// Two kinds of information for each nodes in a data-flow graph are available
/// after this analysis. The first kind, is DefUseSet and the second one is
/// ReachSet.
/// \attention Note that analysis which is performed for base locations is not
/// the same as analysis which is performed for variables form a source code.
/// For example, the base location for (short&)X is a memory location with
/// a size equal to size_of(short) regardless the size of X which might have
/// type int. Be careful when results of this analysis is propagated for
/// variables from a source code.
/// for (...) { (short&X) = ... ;} ... = X;
/// The short part of X will be recognized as last private, but the whole
/// variable X must be also set to first private to preserve the value
/// obtained before the loop.
class ReachDFFwk : private bcl::Uncopyable {
public:
  /// This covers IN and OUT value for a must/may reach definition analysis.
  typedef DFValue<ReachDFFwk, DefinitionInfo> ReachSet;

  /// This represents results of reach definition analysis results.
  typedef llvm::DenseMap<DFNode *,
    std::tuple<std::unique_ptr<DefUseSet>, std::unique_ptr<ReachSet>>,
    llvm::DenseMapInfo<DFNode *>,
    tsar::TaggedDenseMapTuple<
      bcl::tagged<DFNode *, DFNode>,
      bcl::tagged<std::unique_ptr<DefUseSet>, DefUseSet>,
      bcl::tagged<std::unique_ptr<ReachSet>, ReachSet>>> DefinedMemoryInfo;

  /// This represents results of interprocedural analysis.
  typedef llvm::DenseMap<llvm::Function *, std::unique_ptr<DefUseSet>,
    llvm::DenseMapInfo<llvm::Function *>,
    tsar::TaggedDenseMapPair<
      bcl::tagged<llvm::Function *, llvm::Function>,
      bcl::tagged<std::unique_ptr<DefUseSet>, DefUseSet>>> InterprocDefUseInfo;

  /// Creates data-flow framework.
  ReachDFFwk(AliasTree &AT, llvm::TargetLibraryInfo &TLI,
      const DFRegionInfo &DFI, const llvm::DominatorTree &DT,
      const DelinearizeInfo &DI, llvm::ScalarEvolution &SE,
      const llvm::DataLayout &DL, const GlobalOptions &GO,
      DefinedMemoryInfo &DefInfo) :
    mAliasTree(&AT), mTLI(&TLI), mRegionInfo(&DFI),
    mDT(&DT), mDI(&DI), mSE(&SE), mDL(&DL), mGO(&GO), mDefInfo(&DefInfo) {}

  /// Creates data-flow framework.
  ReachDFFwk(AliasTree &AT, llvm::TargetLibraryInfo &TLI,
      const DFRegionInfo &DFI, const llvm::DominatorTree &DT,
      const DelinearizeInfo &DI, llvm::ScalarEvolution &SE,
      const llvm::DataLayout &DL, const GlobalOptions &GO,
      DefinedMemoryInfo &DefInfo, InterprocDefUseInfo &InterprocDUInfo) :
    mAliasTree(&AT), mTLI(&TLI), mRegionInfo(&DFI), mDT(&DT),
    mDI(&DI), mSE(&SE), mDL(&DL), mGO(&GO), mDefInfo(&DefInfo),
    mInterprocDUInfo(&InterprocDUInfo) {}

  /// Creates data-flow framework.
  ReachDFFwk(AliasTree &AT, llvm::TargetLibraryInfo &TLI,
      const llvm::DominatorTree &DT, const DelinearizeInfo &DI,
      llvm::ScalarEvolution &SE, const llvm::DataLayout &DL,
      const GlobalOptions &GO, DefinedMemoryInfo &DefInfo,
      InterprocDefUseInfo &InterprocDUInfo) :
    mAliasTree(&AT), mTLI(&TLI), mDT(&DT), mDI(&DI), mSE(&SE), mDL(&DL),
    mGO(&GO), mDefInfo(&DefInfo), mInterprocDUInfo(&InterprocDUInfo) {}

  /// Return results of interprocedural analysis or nullptr.
  InterprocDefUseInfo * getInterprocDefUseInfo() noexcept {
    return mInterprocDUInfo;
  }

  /// Return results of interprocedural analysis or nullptr.
  InterprocDefUseInfo * getInterprocDefUseInfo() const noexcept {
    return mInterprocDUInfo;
  }

  /// Returns representation of reach definition analysis results.
  DefinedMemoryInfo & getDefInfo() noexcept { return *mDefInfo; }

  /// Returns representation of reach definition analysis results.
  const DefinedMemoryInfo & getDefInfo() const noexcept { return *mDefInfo; }

  /// Returns an alias tree.
  AliasTree & getAliasTree() const noexcept { return *mAliasTree; }

  /// Returns target library information.
  llvm::TargetLibraryInfo & getTLI() const noexcept { return *mTLI; }

  /// Return dominator tree.
  const llvm::DominatorTree & getDomTree() const noexcept { return *mDT; }

  /// Return hierarchy of regions.
  const DFRegionInfo &getRegionInfo() const noexcept { return *mRegionInfo; }

  /// Returns delinearize info.
  const DelinearizeInfo * getDelinearizeInfo() const noexcept { return mDI; }

  /// Returns scalar evolution.
  llvm::ScalarEvolution * getScalarEvolution() const noexcept { return mSE; }

  /// Returns data layout.
  const llvm::DataLayout & getDataLayout() const noexcept { return *mDL; }

  /// Returns global options.
  const GlobalOptions & getGlobalOptions() const noexcept { return *mGO; }

  /// Collapses a data-flow graph which represents a region to a one node
  /// in a data-flow graph of an outer region.
  void collapse(DFRegion *R);
private:
  AliasTree *mAliasTree;
  llvm::TargetLibraryInfo *mTLI;
  const llvm::DominatorTree *mDT;
  const DFRegionInfo *mRegionInfo;
  DefinedMemoryInfo *mDefInfo;
  InterprocDefUseInfo *mInterprocDUInfo = nullptr;
  const DelinearizeInfo *mDI;
  llvm::ScalarEvolution *mSE;
  const llvm::DataLayout *mDL;
  const GlobalOptions *mGO;
};

/// This represents results of interprocedural reach definition analysis.
typedef ReachDFFwk::InterprocDefUseInfo InterprocDefUseInfo;

/// This covers IN and OUT value for a must/may reach definition analysis.
typedef ReachDFFwk::ReachSet ReachSet;

/// This represents results of reach definition analysis results.
typedef ReachDFFwk::DefinedMemoryInfo DefinedMemoryInfo;

/// Traits for a data-flow framework which is used to find reach definitions.
template<> struct DataFlowTraits<ReachDFFwk *> {
  typedef Forward<DFRegion * > GraphType;
  typedef DefinitionInfo ValueType;
  static ValueType topElement(ReachDFFwk *, GraphType) {
    DefinitionInfo DI;
    DI.MustReach = LocationDFValue::fullValue();
    DI.MayReach = LocationDFValue::emptyValue();
    return DI;
  }
  static ValueType boundaryCondition(ReachDFFwk *, GraphType) {
    DefinitionInfo DI;
    DI.MustReach = LocationDFValue::emptyValue();
    DI.MayReach = LocationDFValue::emptyValue();
    return DI;
  }
  static void setValue(ValueType V, DFNode *N, ReachDFFwk *DFF) {
    assert(N && "Node must not be null!");
    assert(DFF && "Data-flow framework must not be null!");
    auto I = DFF->getDefInfo().find(N);
    assert(I != DFF->getDefInfo().end() && I->get<ReachSet>() &&
      "Data-flow value must be specified!");
    auto &RS = I->get<ReachSet>();
    RS->setOut(std::move(V));
  }
  static const ValueType & getValue(DFNode *N, ReachDFFwk *DFF) {
    assert(N && "Node must not be null!");
    assert(DFF && "Data-flow framework must not be null!");
    auto I = DFF->getDefInfo().find(N);
    assert(I != DFF->getDefInfo().end() && I->get<ReachSet>() &&
      "Data-flow value must be specified!");
    auto &RS = I->get<ReachSet>();
    return RS->getOut();
  }
  static void initialize(DFNode *, ReachDFFwk *, GraphType);
  static void meetOperator(
    const ValueType &LHS, ValueType &RHS, ReachDFFwk *, GraphType) {
    RHS.MustReach.intersect(LHS.MustReach);
    RHS.MayReach.merge(LHS.MayReach);
  }
  static bool transferFunction(ValueType, DFNode *, ReachDFFwk *, GraphType);
};

/// Traits for a data-flow framework which is used to find reach definitions.
template<> struct RegionDFTraits<ReachDFFwk *> :
  DataFlowTraits<ReachDFFwk *> {
  static void expand(ReachDFFwk *, GraphType) {}
  static void collapse(ReachDFFwk *Fwk, GraphType G) {
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
}

namespace llvm {
class DefinedMemoryPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DefinedMemoryPass() : FunctionPass(ID) {
    initializeDefinedMemoryPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns results of reach definition analysis.
  tsar::DefinedMemoryInfo & getDefInfo() noexcept { return mDefInfo; }

  /// Returns results of reach definition analysis.
  const tsar::DefinedMemoryInfo & getDefInfo() const noexcept {
    return mDefInfo;
  }

  /// Executes reach definition analysis for a specified function.
  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Releases memory.
  void releaseMemory() override { mDefInfo.clear(); }

private:
  tsar::DefinedMemoryInfo mDefInfo;
};

/// Wrapper to access results of interprocedural reaching definitions analysis.
using GlobalDefinedMemoryWrapper =
  AnalysisWrapperPass<tsar::InterprocDefUseInfo>;
}
#endif//TSAR_DEFINED_MEMORY_H
