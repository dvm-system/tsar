//===--- tsar_private.cpp - Private Variable Analyzer -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to analyze variables which can be privatized.
//
//===----------------------------------------------------------------------===//

#include <llvm/Config/llvm-config.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <utility.h>
#include "tsar_dbg_output.h"
#include "DefinedMemory.h"
#include "DFRegionInfo.h"
#include "tsar_private.h"
#include "tsar_graph.h"
#include "LiveMemory.h"
#include "tsar_utility.h"

using namespace llvm;
using namespace tsar;
using bcl::operator "" _b;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private"

STATISTIC(NumPrivate, "Number of private locations found");
STATISTIC(NumLPrivate, "Number of last private locations found");
STATISTIC(NumSToLPrivate, "Number of second to last private locations found");
STATISTIC(NumDPrivate, "Number of dynamic private locations found");
STATISTIC(NumFPrivate, "Number of first private locations found");
STATISTIC(NumDeps, "Number of unsorted dependencies found");
STATISTIC(NumShared, "Number of shraed locations found");
STATISTIC(NumAddressAccess, "Number of locations address of which is evaluated");

char PrivateRecognitionPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateRecognitionPass, "private",
                      "Private Variable Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(LiveMemoryPass)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
#else
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
#endif
INITIALIZE_PASS_END(PrivateRecognitionPass, "private",
                    "Private Variable Analysis", true, true)

bool PrivateRecognitionPass::runOnFunction(Function &F) {
  releaseMemory();
#ifdef DEBUG
  for (const BasicBlock &BB : F)
    assert((&F.getEntryBlock() == &BB || BB.getNumUses() > 0 )&&
      "Data-flow graph must not contain unreachable nodes!");
#endif
  LoopInfo &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
#else
  AliasAnalysis &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
#endif
  DFRegionInfo &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  getAnalysis<DefinedMemoryPass>();
  getAnalysis<LiveMemoryPass>();
  mAliasTracker = new AliasSetTracker(AA);
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    mAliasTracker->add(&*I);
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  resolveCandidats(DFF);
  releaseMemory(DFF);
  delete mAliasTracker, mAliasTracker = nullptr;
  return false;
}

namespace {
/// \brief Identifiers of recognized traits.
///
/// This is a helpful enumeration which must not be used outside the private
/// recognition pass. It is easy to join different traits. For example,
/// Shared & LastPrivate = 00110 = LastPrivate & FirstPrivate. So if some part
/// of memory locations is shared and other part is last private a union is
/// last private and first private (for details see resolve... methods).
enum TraitId : unsigned long long {
  NoAccess =             11111_b,
  Shared =               11110_b,
  Private =              01111_b,
  FirstPrivate =         01110_b,
  SecondToLastPrivate =  01011_b,
  LastPrivate =          00111_b,
  DynamicPrivate =       00011_b,
  Dependency =           00000_b
};
}

void PrivateRecognitionPass::resolveCandidats(DFRegion *R) {
  assert(R && "Region must not be null!");
  if (auto *L = dyn_cast<DFLoop>(R)) {
    DependencySet *DS = new DependencySet;
    mPrivates.insert(std::make_pair(L->getLoop(), DS));
    R->addAttribute<DependencyAttr>(DS);
    DefUseSet *DefUse = R->getAttribute<DefUseAttr>();
    assert(DefUse && "Value of def-use attribute must not be null");
    LiveSet *LS = R->getAttribute<LiveAttr>();
    assert(LS && "List of live locations must not be null!");
    // Analysis will performed for base locations. This means that results
    // for two elements of array a[i] and a[j] will be generalized for a whole
    // array 'a'. The key in following map LocBases is a base location.
    TraitMap LocBases;
    resolveAccesses(R->getLatchNode(), R->getExitNode(),
      DefUse, LS, LocBases, DS);
    resolvePointers(DefUse, LocBases, DS);
    storeResults(DefUse, LocBases, DS);
    // resolveAddresses() uses DS so storeResults() had to be already executed.
    resolveAddresses(L, DefUse, LocBases, DS);
  }
  for (DFRegion::region_iterator I = R->region_begin(), E = R->region_end();
       I != E; ++I)
    resolveCandidats(*I);
}

void PrivateRecognitionPass::resolveAccesses(const DFNode *LatchNode,
    const DFNode *ExitNode, const tsar::DefUseSet *DefUse,
    const tsar::LiveSet *LS, TraitMap &LocBases, tsar::DependencySet *DS) {
  assert(LatchNode && "Latch node must not be null!");
  assert(ExitNode && "Exit node must not be null!");
  assert(DefUse && "Def-use set must not be null!");
  assert(LS && "Live set must not be null!");
  assert(DS && "Dependency set must not be null!");
  PrivateDFValue *LatchDF = LatchNode->getAttribute<PrivateDFAttr>();
  assert(LatchDF && "List of must/may defined locations must not be null!");
  // LatchDefs is a set of must/may define locations before a branch to
  // a next arbitrary iteration.
  const DefinitionInfo &LatchDefs = LatchDF->getOut();
  // ExitingDefs is a set of must and may define locations which obtains
  // definitions in the iteration in which exit from a loop takes place.
  PrivateDFValue *ExitDF = ExitNode->getAttribute<PrivateDFAttr>();
  assert(ExitDF && "List of must/may defined locations must not be null!");
  const DefinitionInfo &ExitingDefs = ExitDF->getOut();
  for (const AliasSet &AS : DefUse->getExplicitAccesses()) {
    if (AS.isForwardingAliasSet() || AS.empty())
      continue; // The set is empty if it contains only unknown instructions.
    auto I = AS.begin(), E = AS.end();
    // Note that analysis which is performed for base locations is not the same
    // as the analysis which is performed for variables from a source code.
    // For example, the base location for (short&)X is a memory location with
    // a size equal to the size_of(short), regardless the size of X which might
    // have type int. Be careful when results of this analysis are propagated
    // for variables from a source code.
    // for (...) { (short&X) = ... ;} ... = X;
    // The short part of X will be recognized as last private, but the whole
    // variable X must be also set to first private to preserve the value
    // obtained before the loop.
    const MemoryLocation *CurrBase = DS->base(
      MemoryLocation(I.getPointer(), I.getSize(), I.getAAInfo()));
    auto CurrTraits = LocBases.insert(std::make_pair(CurrBase, NoAccess)).first;
    for (; I != E; ++I) {
      MemoryLocation Loc(I.getPointer(), I.getSize(), I.getAAInfo());
      const MemoryLocation *Base = DS->base(Loc);
      if (CurrBase != Base) {
        // Current alias set contains memory locations with different bases.
        // So there are explicitly accessed locations with different bases
        // which alias each other.
        if (CurrTraits->second != Shared)
          CurrTraits->second = Dependency;
        break;
      }
      if (!DefUse->hasUse(Loc)) {
        if (!LS->getOut().overlap(Loc))
          CurrTraits->second &= Private;
        else if (DefUse->hasDef(Loc))
          CurrTraits->second &= LastPrivate;
        else if (LatchDefs.MustReach.contain(Loc) &&
          !ExitingDefs.MayReach.overlap(Loc))
          // These location will be stored as second to last private, i.e.
          // the last definition of these locations is executed on the
          // second to the last loop iteration (on the last iteration the
          // loop condition check is executed only).
          // It is possible that there is only one (last) iteration in
          // the loop. In this case the location has not been assigned and
          // must be declared as a first private.
          CurrTraits->second &= SecondToLastPrivate & FirstPrivate;
        else
          // There is no certainty that the location is always assigned
          // the value in the loop. Therefore, it must be declared as a
          // first private, to preserve the value obtained before the loop
          // if it has not been assigned.
          CurrTraits->second &= DynamicPrivate & FirstPrivate;
      } else if (DefUse->hasMayDef(Loc) || DefUse->hasDef(Loc)) {
        CurrTraits->second &= Dependency;
      } else {
        CurrTraits->second &= Shared;
      }
    }
    for (; I != E; ++I) {
      MemoryLocation Loc(I.getPointer(), I.getSize(), I.getAAInfo());
      CurrBase = DS->base(Loc);
      CurrTraits = LocBases.insert(std::make_pair(CurrBase, NoAccess)).first;
      if (DefUse->hasMayDef(Loc) || DefUse->hasDef(Loc))
        CurrTraits->second &= Dependency;
      else
        CurrTraits->second &= Shared;
    }
    // The following two tests check whether whole base location will be written
    // in the loop. Let us propose the following explanation. Consider a loop
    // where some location Loc is written and this memory is going to be read
    // after the program exit from this loop. It is possible that the base for
    // this location BaseLoc covers this location, so not a whole memory that
    // comprises BaseLoc is written in the loop. To avoid a loss of data stored
    // before the loop execution in a part of memory which is not written after
    // copy out from this loop the BaseLoc must be also set as a first private.
    if (CurrTraits->second == LastPrivate &&
        !ExitingDefs.MustReach.contain(*CurrBase))
      CurrTraits->second &= FirstPrivate;
    if (CurrTraits->second == SecondToLastPrivate &&
        !LatchDefs.MustReach.contain(*CurrBase))
      CurrTraits->second &= FirstPrivate;
  }
}

void PrivateRecognitionPass::resolvePointers(const tsar::DefUseSet *DefUse,
    TraitMap &LocBases, tsar::DependencySet *DS) {
  assert(DefUse && "Def-use set must not be null!");
  assert(DS && "Dependency set must not be null!");
  for (const AliasSet &AS : DefUse->getExplicitAccesses()) {
    if (AS.isForwardingAliasSet() || AS.empty())
      continue; // The set is empty if it contains only unknown instructions.
    for (AliasSet::iterator I = AS.begin(), E = AS.end(); I != E; ++I) {
      Value *V = I.getPointer();
      if (Operator::getOpcode(V) == Instruction::BitCast ||
        Operator::getOpcode(V) == Instruction::AddrSpaceCast ||
        Operator::getOpcode(V) == Instruction::IntToPtr)
        V = cast<Operator>(V)->getOperand(0);
      // *p means that address of location should be loaded from p using 'load'.
      if (auto *LI = dyn_cast<LoadInst>(V)) {
        const MemoryLocation *Loc = DS->base(
          MemoryLocation(I.getPointer(), I.getSize(), I.getAAInfo()));
        auto LocTraits = LocBases.find(Loc);
        assert(LocTraits != LocBases.end() &&
          "Traits of location must be initialized!");
        if (LocTraits->second == Private ||
          LocTraits->second == Shared)
          continue;
        const MemoryLocation *Ptr = DS->base(MemoryLocation::get(LI));
        auto PtrTraits = LocBases.find(Ptr);
        assert(PtrTraits != LocBases.end() &&
          "Traits of location must be initialized!");
        if (PtrTraits->second == Shared)
          continue;
        // Location can not be declared as copy in or copy out without
        // additional analysis because we do not know which memory must
        // be copy. Let see an example:
        // for (...) { P = &X; *P = ...; P = &Y; } after loop P = &Y, not &X.
        // P = &Y; for (...) { *P = ...; P = &X; } before loop P = &Y, not &X.
        // Note that case when location is shared, but pointer is not shared
        // may be difficulty to implement for distributed memory, for example:
        // for(...) { P = ...; ... = *P; } It is not evident which memory
        // should be copy to each processor.
        LocTraits->second = Dependency;
      }
    }
  }
}

void PrivateRecognitionPass::resolveAddresses(
    const DFLoop *L, const tsar::DefUseSet *DefUse,
    TraitMap &LocBases, tsar::DependencySet *DS) {
  assert(L && "Loop must not be null!");
  assert(DefUse && "Def-use set must not be null!");
  assert(DS && "Dependency set must not be null!");
  assert(DefUse == L->getAttribute<DefUseAttr>() &&
    "Def-use set must be related to the specified loop!");
  assert(DS == L->getAttribute<DependencyAttr>() &&
    "Dependency set must be related to the specified loop!");
  for (llvm::Value *Ptr : DefUse->getAddressAccesses()) {
    const llvm::MemoryLocation * Base = DS->base(MemoryLocation(Ptr, 0));
    // Do not remember an address:
    // * if it is stored in some location, for example isa<LoadInst>((*I)->Ptr),
    //  locations are analyzed separately;
    // * if it points to a temporary location and should not be analyzed:
    // for example, a result of a call can be a pointer.
    if (!Base->Ptr ||
        !isa<AllocaInst>(Base->Ptr) && !isa<GlobalVariable>(Base->Ptr))
      continue;
    Loop *Lp = L->getLoop();
    // If this is an address of a location declared in the loop do not
    // remember it.
    if (auto AI = dyn_cast<AllocaInst>(Base->Ptr))
      if (Lp->contains(AI->getParent()))
        continue;
    for (auto User : Ptr->users()) {
      auto UI = dyn_cast<Instruction>(User);
      if (!UI || !Lp->contains(UI->getParent()))
        continue;
      // The address is used inside the loop.
      // Remember it if it is used for computation instead of memory access or
      // if we do not know how it will be used.
      if (isa<PtrToIntInst>(User) ||
        (isa<StoreInst>(User) &&
          cast<StoreInst>(User)->getValueOperand() == Ptr)) {
        auto I = DS->find(*Base);
        if (I != DS->end()) {
          I->set<trait::AddressAccess>(nullptr);
        } else {
          DependencyDescriptor Dptr;
          Dptr.set<trait::NoAccess, trait::AddressAccess>();
          DS->insert(*Base, Dptr);
        }
        ++NumAddressAccess;
        break;
      }
    }
  }
}
#include <array>

void PrivateRecognitionPass::storeResults(const tsar::DefUseSet *DefUse,
  TraitMap &LocBases, tsar::DependencySet *DS) {
  assert(DS && "Dependency set must not be null!");
  for (auto &LocTraits : LocBases) {
    DependencyDescriptor Dptr;
    switch (LocTraits.second) {
    case Shared: Dptr.set<trait::Shared>(); ++NumShared; break;
    case Dependency: Dptr.set<trait::Dependency>(); ++NumDeps; break;
    case Private: Dptr.set<trait::Private>(); ++NumPrivate; break;
    case FirstPrivate: Dptr.set<trait::FirstPrivate>(); ++NumFPrivate; break;
    case FirstPrivate & LastPrivate:
      Dptr.set<trait::FirstPrivate>(); ++NumFPrivate;
    case LastPrivate:
      Dptr.set<trait::LastPrivate>(); ++NumLPrivate;
      break;
    case FirstPrivate & SecondToLastPrivate:
      Dptr.set<trait::FirstPrivate>(); ++NumFPrivate;
    case SecondToLastPrivate:
      Dptr.set<trait::SecondToLastPrivate>(); ++NumSToLPrivate;
      ++NumSToLPrivate;
      break;
    case FirstPrivate & DynamicPrivate:
      Dptr.set<trait::FirstPrivate>(); ++NumFPrivate;
    case DynamicPrivate:
      Dptr.set<trait::DynamicPrivate>(); ++NumDPrivate;
      break;
    }
    DS->insert(*LocTraits.first, Dptr);
  }
}

void PrivateRecognitionPass::releaseMemory(DFRegion *R) {
  assert(R && "Region must not be null!");
  DefUseSet *DefUse = R->removeAttribute<DefUseAttr>();
  if (DefUse)
    delete DefUse;
  LiveSet *LS = R->removeAttribute<LiveAttr>();
  if (LS)
    delete LS;
  PrivateDFValue *PV = R->removeAttribute<PrivateDFAttr>();
  if (PV)
    delete PV;
  for (auto I = R->node_begin(), E = R->node_end(); I != E; ++I) {
    if (auto InnerRegion = dyn_cast<DFRegion>(*I)) {
      releaseMemory(InnerRegion);
    } else {
      DefUseSet *DefUse = (*I)->removeAttribute<DefUseAttr>();
      if (DefUse)
        delete DefUse;
      LiveSet *LS = (*I)->removeAttribute<LiveAttr>();
      if (LS)
        delete LS;
      PrivateDFValue *PV = (*I)->removeAttribute<PrivateDFAttr>();
      if (PV)
        delete PV;
    }
  }
}

void PrivateRecognitionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<LiveMemoryPass>();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AU.addRequired<AliasAnalysis>();
#else
  AU.addRequired<AAResultsWrapperPass>();
#endif
  AU.setPreservesAll();
}

namespace {
/// This functor stores representation of a trait in a static map as a string.
class TraitToStringFunctor {
public:
  /// Static map from trait to its string representation.
  typedef bcl::StaticTraitMap<
    std::string, DependencyDescriptor> TraitToStringMap;

  /// Creates the functor.
  TraitToStringFunctor(TraitToStringMap &Map, llvm::StringRef Offset) :
    mMap(&Map), mOffset(Offset) {}

  /// Stores representation of a trait in a static map as a string.
  template<class Trait> void operator()() {
    assert(mTS && "Trait set must not be null!");
    llvm::raw_string_ostream OS(mMap->value<Trait>());
    OS << mOffset;
    printLocationSource(OS, mTS->memory()->Ptr);
    OS << "\n";
  }

  /// Returns a static trait map.
  TraitToStringMap & getStringMap() { return *mMap; }

  /// \brief Returns current trait set.
  ///
  /// \pre Trait set must not be null and has been specified by setTraitSet().
  LocationTraitSet & getTraitSet() {
    assert(mTS && "Trait set must not be null!");
    return *mTS;
  }

  /// Specifies current trait set.
  void setTraitSet(LocationTraitSet &TS) { mTS = &TS; }

private:
  TraitToStringMap *mMap;
  LocationTraitSet *mTS;
  std::string mOffset;
};

/// Prints a static map from trait to its string representation to a specified
/// output stream.
class TraitToStringPrinter {
public:
  /// Creates functor.
  TraitToStringPrinter(llvm::raw_ostream &OS, llvm::StringRef Offset) :
    mOS(OS), mOffset(Offset) {}

  /// Prints a specified trait.
  template<class Trait> void operator()(llvm::StringRef Str) {
    if (Str.empty())
      return;
    mOS << mOffset << Trait::toString() << ":\n" << Str;
  }

private:
  llvm::raw_ostream &mOS;
  std::string mOffset;
};
}

void PrivateRecognitionPass::print(raw_ostream &OS, const Module *M) const {
  LoopInfo &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  for_each(LpInfo, [this, &OS](Loop *L) {
    DebugLoc Loc = L->getStartLoc();
    std::string Offset(L->getLoopDepth(), ' ');
    OS << Offset;
    Loc.print(OS);
    OS << "\n";
    const DependencySet &DS = getPrivatesFor(L);
    TraitToStringFunctor::TraitToStringMap TraitToStr;
    TraitToStringFunctor ToStrFunctor(TraitToStr, Offset + "  ");
    for (auto &TS : DS) {
      ToStrFunctor.setTraitSet(TS);
      TS.for_each(ToStrFunctor);
    }
    TraitToStr.for_each(TraitToStringPrinter(OS, Offset + " "));
  });
}

FunctionPass *llvm::createPrivateRecognitionPass() {
  return new PrivateRecognitionPass();
}
