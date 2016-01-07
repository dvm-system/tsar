//===--- tsar_private.cpp - Private Variable Analyzer -----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to analyze variables which can be privatized.
//
//===----------------------------------------------------------------------===//

#include <llvm/Pass.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include <llvm/DebugInfo.h>
#include <llvm/Analysis/Dominators.h>
#else
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Dominators.h>
#endif
#include <functional>
#include <utility.h>
#include <declaration.h>
#include "tsar_private.h"
#include "tsar_graph.h"
#include "tsar_pass.h"
#include "tsar_utility.h"
#include "tsar_dbg_output.h"

using namespace llvm;
using namespace tsar;
using Utility::operator "" _b;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private"

STATISTIC(NumPrivate, "Number of private locations found");
STATISTIC(NumLPrivate, "Number of last private locations found");
STATISTIC(NumSToLPrivate, "Number of second to last private locations found");
STATISTIC(NumDPrivate, "Number of dynamic private locations found");
STATISTIC(NumFPrivate, "Number of dynamic first private locations found");
STATISTIC(NumDeps, "Number of unsorted dependencies found");
STATISTIC(NumShared, "Number of shraed locations found");

char PrivateRecognitionPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateRecognitionPass, "private",
                      "Private Variable Analysis", true, true)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
INITIALIZE_PASS_DEPENDENCY(DominatorTree)
#else
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
#endif
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 7)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
#else
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
#endif
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_PASS_END(PrivateRecognitionPass, "private",
                    "Private Variable Analysis", true, true)

bool PrivateRecognitionPass::runOnFunction(Function &F) {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 7)
  LoopInfo &LpInfo = getAnalysis<LoopInfo>();
#else
  LoopInfo &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
#endif
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
  DominatorTreeBase<BasicBlock> &DomTree = *(getAnalysis<DominatorTree>().DT);
#else
  DominatorTree &DomTree = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
#endif
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
  mAliasTracker = new AliasSetTracker(AA);
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    mAliasTracker->add(&*I);
  DFFunction DFF(&F);
  buildLoopRegion(std::make_pair(&F, &LpInfo), &DFF);
  PrivateDFFwk PrivateFWK(mAliasTracker, mPrivates);
  solveDataFlowUpward(&PrivateFWK, &DFF);
  LiveDFFwk LiveFwk(mAliasTracker);
  LiveSet *LS = new LiveSet;
  DFF.addAttribute<LiveAttr>(LS);
  solveDataFlowDownward(&LiveFwk, &DFF);
  resolveCandidats(&DFF);
  for_each(LpInfo, [this](Loop *L) {
    DebugLoc loc = L->getStartLoc();
    Base::Text Offset(L->getLoopDepth(), ' ');
    errs() << Offset;
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 7)
    loc.print(getGlobalContext(), errs());
#else
    loc.print(errs());
#endif
    errs() << "\n";
    const DependencySet &DS = getPrivatesFor(L);
    errs() << Offset << " privates:\n";
    for (Value *Loc : DS[Private]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << Offset << " last privates:\n";
    for (Value *Loc : DS[LastPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << Offset << " second to last privates:\n";
    for (Value *Loc : DS[SecondToLastPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << Offset << " dynamic privates:\n";
    for (Value *Loc : DS[DynamicPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << Offset << " first privates:\n";
    for (Value *Loc : DS[FirstPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << Offset << " shared variables:\n";
    for (Value *Loc : DS[Shared]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << Offset << " dependencies:\n";
    for (Value *Loc : DS[Dependency]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc);
      errs() << "\n";
    }
    errs() << "\n";
  });
  delete mAliasTracker, mAliasTracker = nullptr;
  return false;
}

void PrivateRecognitionPass::resolveCandidats(DFRegion *R) {
  assert(R && "Region must not be null!");
  if (isa<DFLoop>(R)) {
    DefUseSet *DefUse = R->getAttribute<DefUseAttr>();
    assert(DefUse && "Value of def-use attribute must not be null");
    DependencySet *DS = R->getAttribute<DependencyAttr>();
    assert(DS && "List of privatizable candidats must not be null!");
    LiveSet *LS = R->getAttribute<LiveAttr>();
    assert(LS && "List of live locations must not be null!");
    DenseMap<Value *, unsigned long long> LocBases;
    constexpr auto NoAccessFlag = 111111_b;
    constexpr auto SharedFlag = 011111_b;
    constexpr auto PrivateFlag = 001111_b;
    constexpr auto LPrivateFlag = 000111_b;
    constexpr auto FLPrivateFlag = 000110_b;
    constexpr auto SToLPrivateFlag = 001011_b;
    constexpr auto FSToLPrivateFlag = 001010_b;
    constexpr auto FPrivateFlag = 001110_b;
    constexpr auto DPrivateFlag = 000011_b;
    constexpr auto FDPrivateFlag = 000010_b;
    constexpr auto DependencyFlag = 000000_b;
    for (Value *Loc : DefUse->getExplicitAccesses()) {
      if (LS->getOut().count(Loc) == 0) {
        if (DS->is(LastPrivate, Loc)) {
          (*DS)[LastPrivate].erase(Loc), --NumLPrivate;
          (*DS)[Private].insert(Loc), ++NumPrivate;
        } else if (DS->is(SecondToLastPrivate, Loc)) {
          (*DS)[SecondToLastPrivate].erase(Loc), --NumSToLPrivate;
          (*DS)[Private].insert(Loc), ++NumPrivate;
        } else if (DS->is(DynamicPrivate, Loc)) {
          (*DS)[DynamicPrivate].erase(Loc), --NumDPrivate;
          (*DS)[Private].insert(Loc), ++NumPrivate;
        }
      }
      // Let us consider the case where location access is performed by pointer.
      if (isa<LoadInst>(Loc)) {
        LoadInst *LI = cast<LoadInst>(Loc);
        Value *Ptr = LI->getPointerOperand();
        if (!DS->is(Shared, Ptr)) {
          if (DS->is(LastPrivate, Loc)) {
            (*DS)[LastPrivate].erase(Loc), --NumLPrivate;
            (*DS)[Dependency].insert(Loc), ++NumDeps;
          } else if (DS->is(SecondToLastPrivate, Loc)) {
            (*DS)[SecondToLastPrivate].erase(Loc), --NumSToLPrivate;
            (*DS)[Dependency].insert(Loc), ++NumDeps;
          } else if (DS->is(DynamicPrivate, Loc)) {
            (*DS)[DynamicPrivate].erase(Loc), --NumDPrivate;
            (*DS)[Dependency].insert(Loc), ++NumDeps;
          }
        }
      }
      Value *Base = findLocationBase(Loc);
      auto BaseInfo = LocBases.insert(std::make_pair(Base, NoAccessFlag));
      BaseInfo.first->second &= [&](Value *Loc) {
        if (DS->is(Shared, Loc))
          return SharedFlag;
        if (DS->is(Private, Loc))
          return PrivateFlag;
        if (DS->is(LastPrivate, Loc))
          return LPrivateFlag;
        if (DS->is(SecondToLastPrivate, Loc))
          return SToLPrivateFlag;
        if (DS->is(DynamicPrivate, Loc))
          return DPrivateFlag;
        if (DS->is(Dependency, Loc))
          return DependencyFlag;
        return NoAccessFlag;
      }(Loc);
    }
    NumShared = NumPrivate = NumFPrivate = NumLPrivate = 0;
    NumSToLPrivate = NumDPrivate = NumDeps = 0;
    (*DS)[Shared].clear();
    (*DS)[Private].clear();
    (*DS)[FirstPrivate].clear();
    (*DS)[LastPrivate].clear();
    (*DS)[SecondToLastPrivate].clear();
    (*DS)[DynamicPrivate].clear();
    (*DS)[Dependency].clear();
    for (auto BaseInfo : LocBases) {
      Value *Loc = BaseInfo.first;
      switch (BaseInfo.second) {
        case SharedFlag: (*DS)[Shared].insert(Loc); ++NumShared; break;
        case PrivateFlag: (*DS)[Private].insert(Loc); ++NumPrivate; break;
        case FLPrivateFlag: (*DS)[FirstPrivate].insert(Loc); ++NumFPrivate;
        case LPrivateFlag: (*DS)[LastPrivate].insert(Loc); ++NumLPrivate; break;
        case FSToLPrivateFlag: (*DS)[FirstPrivate].insert(Loc); ++NumFPrivate;
        case SToLPrivateFlag:
          (*DS)[SecondToLastPrivate].insert(Loc); ++NumSToLPrivate; break;
        case FPrivateFlag: (*DS)[FirstPrivate].insert(Loc); ++NumFPrivate; break;
        case FDPrivateFlag: (*DS)[FirstPrivate].insert(Loc); ++NumFPrivate;
        case DPrivateFlag:
          (*DS)[DynamicPrivate].insert(Loc); ++NumDPrivate; break;
        case DependencyFlag: (*DS)[Dependency].insert(Loc); ++NumDeps; break;
      }
    }
    // The following analysis has not been performed for base locations
    // which differ from accessed location. For example, it has been 
    // performed for a[i] but not for all location which is started at *a.
    for (auto BaseInfo : LocBases) {
      Value *Loc = BaseInfo.first;
      if (DS->is(Shared, Loc))
        continue;
      AliasAnalysis &AA = mAliasTracker->getAliasAnalysis();
      uint64_t LocSize = AA.getTypeStoreSize(Loc->getType());
      AliasSet &ASet =
        mAliasTracker->getAliasSetForPointer(Loc, LocSize, AAMDNodes());
      for (auto Ptr : ASet)
        if (Loc != Ptr.getValue() &&
          LocBases.count(Ptr.getValue()) != 0) {
          if (DS->is(Dependency, Loc))
            break;
          (*DS)[Dependency].insert(Loc), ++NumDeps;
          if (DS->is(FirstPrivate, Loc))
            (*DS)[Private].erase(Loc), --NumFPrivate;
          if (DS->is(Private, Loc))
            (*DS)[Private].erase(Loc), --NumPrivate;
          else if (DS->is(LastPrivate, Loc))
            (*DS)[LastPrivate].erase(Loc), --NumLPrivate;
          else if (DS->is(SecondToLastPrivate, Loc))
            (*DS)[SecondToLastPrivate].erase(Loc), --NumSToLPrivate;
          else if (DS->is(DynamicPrivate, Loc))
            (*DS)[DynamicPrivate].erase(Loc), --NumDPrivate;
          break;
        }
    }
  }
  for (DFRegion::region_iterator I = R->region_begin(), E = R->region_end();
       I != E; ++I)
    resolveCandidats(*I);
}

void PrivateRecognitionPass::getAnalysisUsage(AnalysisUsage &AU) const {
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
  AU.addRequired<DominatorTree>();
#else
  AU.addRequired<DominatorTreeWrapperPass>();
#endif
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 7)
  AU.addRequired<LoopInfo>();
#else
  AU.addRequired<LoopInfoWrapperPass>();
#endif
  AU.addRequired<AliasAnalysis>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createPrivateRecognitionPass() {
  return new PrivateRecognitionPass();
}

void DataFlowTraits<PrivateDFFwk*>::initialize(
    DFNode *N, PrivateDFFwk *Fwk, GraphType) {
  assert(N && "Node must not be null!");
  assert(Fwk && "Data-flow framework must not be null");
  PrivateDFValue *V = new PrivateDFValue;
  N->addAttribute<PrivateDFAttr>(V);
  if (llvm::isa<DFRegion>(N))
    return;
  // DefUseAttr will be set here for nodes different to regions.
  // For nodes which represented regions this attribute has been already set
  // in collapse() function.
  DefUseSet *DU = new DefUseSet;
  N->addAttribute<DefUseAttr>(DU);
  DFBlock *DFB = dyn_cast<DFBlock>(N);
  if (!DFB)
    return;
  BasicBlock *BB = DFB->getBlock();
  assert(BB && "Basic block must not be null!");
  AliasSetTracker *T = Fwk->getTracker();
  AliasAnalysis &AA = T->getAliasAnalysis();
  for (Instruction &I : BB->getInstList()) {
    std::function<bool(Value *)> AddNeed;
    std::function<void(Value *)> AddMust, AddMay;
    Value *Loc;
    uint64_t LocSize;
    if (StoreInst *SI = dyn_cast<StoreInst>(&I)) {
      AddNeed = [](Value *Loc) { return true; };
      AddMust = [&DU](Value *Loc) {DU->addDef(Loc); };
      AddMay = [&DU](Value *Loc) {DU->addMayDef(Loc); };
      Loc = SI->getPointerOperand();
      Value *Val = SI->getValueOperand();
      LocSize = AA.getTypeStoreSize(Val->getType());
      if (isa<AllocaInst>(Val))
        DU->addAddressAccess(Val);
    } else if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
      AddNeed = [&DU](Value *Loc) { return !DU->hasDef(Loc); };
      AddMay = AddMust = [&DU](Value *Loc) {DU->addUse(Loc); };
      Loc = LI->getPointerOperand();
      LocSize = AA.getTypeStoreSize(Loc->getType());
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(&I)) {
      Value *Val = GEPI->getPointerOperand();
      if (isa<AllocaInst>(Val))
        DU->addAddressAccess(Val);
      continue;
    } else {
      continue;
    }
    DU->addExplicitAccess(Loc);
    AAMDNodes AAInfo;
    I.getAAMetadata(AAInfo);
    AliasSet &ASet = T->getAliasSetForPointer(Loc, LocSize, AAInfo);
    // DefUseAttr will be set for all locations from alias set.
    // This attribute will be also set for locations which are accessed
    // implicitly.
    for (auto Ptr : ASet) {
      if (!AddNeed(Ptr.getValue()))
        continue;
      AliasResult AR = AA.alias(Loc, LocSize, Ptr.getValue(), Ptr.getSize());
      switch (AR) {
        case MayAlias:
        case PartialAlias: AddMay(Ptr.getValue()); break;
        case MustAlias: AddMust(Ptr.getValue()); break;
        default: assert("Unknown results of an alias query!"); break;
      }
    }
  }
  DEBUG (
    dbgs() << "[DEFUSE] Def/Use locations for the following basic block:";
    DFB->getBlock()->print(dbgs());
    dbgs() << "Outward exposed must define locations:\n";
    for (Value *Loc : DU->getDefs())
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "Outward exposed may define locations:\n";
    for (Value *Loc : DU->getMayDefs())
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "Outward exposed uses:\n";
    for (Value *Loc : DU->getUses())
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "[END DEFUSE]\n";
  );
}

bool DataFlowTraits<PrivateDFFwk*>::transferFunction(
  ValueType V, DFNode *N, PrivateDFFwk *, GraphType) {
  // Note, that transfer function is never evaluated for the entry node.
  assert(N && "Node must not be null!");
  PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
  assert(PV && "Data-flow value must not be null!");
  PV->setIn(std::move(V));
  if (llvm::isa<DFExit>(N)) {
    if (PV->getOut() != V) {
      PV->setOut(std::move(V));
      return true;
    }
    return false;
  }
  DefUseSet *DU = N->getAttribute<DefUseAttr>();
  assert(DU && "Value of def-use attribute must not be null!");
  LocationDFValue newOut(LocationDFValue::emptyValue());
  newOut.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.merge(V);
  if (PV->getOut() != newOut) {
    PV->setOut(std::move(newOut));
    return true;
  }
  return false;
}

void PrivateDFFwk::collapse(DFRegion *R) {
  assert(R && "Region must not be null!");
  typedef RegionDFTraits<PrivateDFFwk *> RT;
  DefUseSet *DefUse = new DefUseSet;
  R->addAttribute<DefUseAttr>(DefUse);
  assert(DefUse && "Value of def-use attribute must not be null!");
  DFLoop *L = llvm::dyn_cast<DFLoop>(R);
  if (!L)
    return;
  // We need two types of defs:
  // * ExitingDefs is a set of must define locations (Defs) for the loop.
  //   These locations always have definitions inside the loop regardless
  //   of execution paths of iterations of the loop.
  // * LatchDefs is a set of must define locations before a branch to
  //   a next arbitrary iteration.
  DFNode *ExitNode = R->getExitNode();
  const LocationDFValue &ExitingDefs = RT::getValue(ExitNode, this);
  DFNode *LatchNode = R->getLatchNode();
  const LocationDFValue &LatchDefs = RT::getValue(LatchNode, this);
  for (DFNode *N : L->getNodes()) {
    PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
    assert(PV && "Data-flow value must not be null!");
    DefUseSet *DU = N->getAttribute<DefUseAttr>();
    assert(DU && "Value of def-use attribute must not be null!");
    // We calculate a set of locations (Uses)
    // which get values outside the loop or from previouse loop iterations.
    // These locations can not be privatized.
    for (Value *Loc : DU->getUses()) {
      if (!PV->getIn().exist(Loc))
        DefUse->addUse(Loc);
    }
    // It is possible that some locations are only written in the loop.
    // In this case this locations are not located at set of node uses but
    // they are located at set of node defs.
    // We calculate a set of must define locations (Defs) for the loop.
    // These locations always have definitions inside the loop regardless
    // of execution paths of iterations of the loop.
    // The set of may define locations (MayDefs) for the loop is also
    // calculated. This set also include all must define locations.
    for (Value *Loc : DU->getDefs()) {
      DefUse->addMayDef(Loc);
      if (ExitingDefs.exist(Loc))
        DefUse->addDef(Loc);
    }
    for (Value *Loc : DU->getMayDefs())
      DefUse->addMayDef(Loc);
    for (Value *Loc : DU->getExplicitAccesses())
      DefUse->addExplicitAccess(Loc);
    for (Value *Loc : DU->getAddressAccesses())
      DefUse->addAddressAccess(Loc);
  }
  // Calculation of a last private variables differs depending on internal
  // representation of a loop. There are two type of representations.
  // 1. The first type has a following pattern:
  //   iter: if (...) goto exit;
  //             ...
  //         goto iter;
  //   exit:
  // For example, representation of a for-loop refers to this type.
  // In this case locations from the LatchDefs collection should be used
  // to determine candidates for last private variables. These locations will be
  // stored in the SecondToLastPrivates collection, i.e. the last definition of
  // these locations is executed on the second to the last loop iteration
  // (on the last iteration the loop condition check is executed only).
  // 2. The second type has a following patterm:
  //   iter:
  //             ...
  //         if (...) goto exit; else goto iter;
  //   exit:
  // For example, representation of a do-while-loop refers to this type.
  // In this case locations from the ExitDefs collection should be used.
  // The result will be stored in the LastPrivates collection.
  // In some cases it is impossible to determine in static an iteration
  // where the last definition of an location have been executed. Such locations
  // will be stored in the DynamicPrivates collection.
  // Note, in this step only candidates for last privates and privates
  // variables are calculated. The result should be corrected further.
  DependencySet *DS = new DependencySet;
  mPrivates.insert(std::make_pair(L->getLoop(), DS));
  R->addAttribute<DependencyAttr>(DS);
  assert(DS && "Result of analysis must not be null!");
  AliasAnalysis &AA = mAliasTracker->getAliasAnalysis();
  for (Value *Loc : DefUse->getExplicitAccesses()) {
    if (!DefUse->hasUse(Loc)) {
      uint64_t LocSize = AA.getTypeStoreSize(Loc->getType());
      AliasSet &ASet =
        mAliasTracker->getAliasSetForPointer(Loc, LocSize, AAMDNodes());
      bool isDependency = false;
      for (auto Ptr : ASet)
        if (Loc != Ptr.getValue() &&
            DefUse->hasExplicitAccess(Ptr.getValue())) {
          (*DS)[Dependency].insert(Loc), ++NumDeps;
          isDependency = true;
          break;
        }
      if (isDependency)
        continue;
      if (DefUse->hasDef(Loc))
        (*DS)[LastPrivate].insert(Loc), ++NumLPrivate;
      else if (LatchDefs.exist(Loc))
        (*DS)[SecondToLastPrivate].insert(Loc), ++NumSToLPrivate;
      else
        (*DS)[DynamicPrivate].insert(Loc), ++NumDPrivate;
    }
    else if (DefUse->hasMayDef(Loc)) {
      (*DS)[Dependency].insert(Loc), ++NumDeps;
    }
    else {
      (*DS)[Shared].insert(Loc), ++NumShared;
    }
  }
}

void DataFlowTraits<LiveDFFwk *>::initialize(
    DFNode *N, LiveDFFwk *Fwk, GraphType) {
  assert(N && "Node must not be null!");
  assert(Fwk && "Data-flow framework must not be null!");
  assert(N->getAttribute<DefUseAttr>() &&
    "Value of def-use attribute must not be null!");
  LiveSet *LS = new LiveSet;
  N->addAttribute<LiveAttr>(LS);
}

bool DataFlowTraits<LiveDFFwk*>::transferFunction(
    ValueType V, DFNode *N, LiveDFFwk *, GraphType) {
  // Note, that transfer function is never evaluated for the exit node.
  assert(N && "Node must not be null!");
  LiveSet *LS = N->getAttribute<LiveAttr>();
  assert(LS && "Data-flow value must not be null!");
  LS->setOut(std::move(V));
  if (isa<DFEntry>(N)) {
    if (LS->getIn() != V) {
      LS->setIn(std::move(V));
      return true;
    }
    return false;
  }
  DefUseSet *DU = N->getAttribute<DefUseAttr>();
  assert(DU && "Value of def-use attribute must not be null!");
  LiveDFFwk::LocationSet newIn(DU->getUses());
  for (Value *Loc : V) {
    if (!DU->hasDef(Loc))
      newIn.insert(Loc);
  }
  DEBUG(
    dbgs() << "[LIVE] Live locations analysis, transfer function results for:";
    if (isa<DFBlock>(N)) {
      cast<DFBlock>(N)->getBlock()->print(dbgs());
    }
    else if (isa<DFLoop>(N)) {
      dbgs() << " loop with the following header:";
      cast<DFLoop>(N)->getLoop()->getHeader()->print(dbgs());
    } else {
      dbgs() << " unknown node.\n";
    }
    dbgs() << "IN:\n";
    for (Value *Loc : newIn)
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "OUT:\n";
    for (Value *Loc : V)
      (printLocationSource(dbgs(), Loc), dbgs() << "\n");
    dbgs() << "[END LIVE]\n";
  );
  if (LS->getIn() != newIn) {
    LS->setIn(std::move(newIn));
    return true;
  }
  return false;
}
