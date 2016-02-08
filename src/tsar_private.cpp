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
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Dominators.h>
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
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_PASS_END(PrivateRecognitionPass, "private",
                    "Private Variable Analysis", true, true)

bool PrivateRecognitionPass::runOnFunction(Function &F) {
  releaseMemory();
  LoopInfo &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DominatorTree &DomTree = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
  mAliasTracker = new AliasSetTracker(AA);
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    mAliasTracker->add(&*I);
  DFFunction DFF(&F);
  buildLoopRegion(std::make_pair(&F, &LpInfo), &DFF);
  PrivateDFFwk PrivateFWK(mAliasTracker);
  solveDataFlowUpward(&PrivateFWK, &DFF);
  LiveDFFwk LiveFwk(mAliasTracker);
  LiveSet *LS = new LiveSet;
  DFF.addAttribute<LiveAttr>(LS);
  solveDataFlowDownward(&LiveFwk, &DFF);
  resolveCandidats(&DFF);
  releaseMemory(&DFF);
  for_each(LpInfo, [this](Loop *L) {
    DebugLoc loc = L->getStartLoc();
    Base::Text Offset(L->getLoopDepth(), ' ');
    errs() << Offset;
    loc.print(errs());
    errs() << "\n";
    const DependencySet &DS = getPrivatesFor(L);
    errs() << Offset << " privates:\n";
    for (const MemoryLocation *Loc : DS[Private]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " last privates:\n";
    for (const MemoryLocation *Loc : DS[LastPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " second to last privates:\n";
    for (const MemoryLocation *Loc : DS[SecondToLastPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " dynamic privates:\n";
    for (const MemoryLocation *Loc : DS[DynamicPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " first privates:\n";
    for (const MemoryLocation *Loc : DS[FirstPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " shared variables:\n";
    for (const MemoryLocation *Loc : DS[Shared]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " dependencies:\n";
    for (const MemoryLocation *Loc : DS[Dependency]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " addresses:\n";
    for (const Value *Ptr : DS[AddressAccess]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Ptr);
      errs() << "\n";
    }
    errs() << "\n";
  });
  delete mAliasTracker, mAliasTracker = nullptr;
  return false;
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
    DenseMap<const MemoryLocation *, unsigned long long> LocBases;
    DFNode *LatchNode = R->getLatchNode();
    PrivateDFValue *LatchDF = LatchNode->getAttribute<PrivateDFAttr>();
    assert(LatchDF && "List of must defined locations must not be null!");
    // LatchDefs is a set of must define locations before a branch to
    // a next arbitrary iteration.
    const LocationDFValue &LatchDefs = LatchDF->getOut();
    for (const AliasSet &AS : DefUse->getExplicitAccesses()) {
      if (AS.isForwardingAliasSet() || AS.empty())
        continue; // The set is empty if it contains only unknown instructions.
      auto I = AS.begin(), E = AS.end();
      const MemoryLocation *CurrBase = *(*DS)[Analyze].insert(
        MemoryLocation(I.getPointer(), I.getSize(), I.getAAInfo())).first;
      auto CurrTraits =
        LocBases.insert(std::make_pair(CurrBase, NoAccess.Id)).first;
      for (; I != E; ++I) {
        MemoryLocation Loc(I.getPointer(), I.getSize(), I.getAAInfo());
        const MemoryLocation *Base = *(*DS)[Analyze].insert(Loc).first;
        if (CurrBase != Base) {
          // Current alias set contains memory locations with different bases.
          // So there are explicitly accessed locations with different bases
          // which alias each other.
          if (CurrTraits->second != Shared.Id)
            CurrTraits->second = Dependency.Id;
          break;
        }
        if (!DefUse->hasUse(Loc)) {
          if (!LS->getOut().overlap(Loc))
            CurrTraits->second &= Private.Id;
          else if (DefUse->hasDef(Loc))
            CurrTraits->second &= LastPrivate.Id;
          else if (LatchDefs.contain(Loc))
            // These location will be stored as second to last private, i.e.
            // the last definition of these locations is executed on the second
            // to the last loop iteration (on the last iteration the loop
            // condition check is executed only).
            CurrTraits->second &= SecondToLastPrivate.Id & FirstPrivate.Id;
          else
            // There is no certainty that the location is always assigned
            // the value in the loop. Therefore, it must be declared as a
            // first private, to preserve the value obtained before the loop
            // if it has not been assigned.
            CurrTraits->second &= DynamicPrivate.Id & FirstPrivate.Id;
        } else if (DefUse->hasMayDef(Loc) || DefUse->hasDef(Loc)) {
          CurrTraits->second &= Dependency.Id;
        } else {
          CurrTraits->second &= Shared.Id;
        }
      }
      for (; I != E; ++I) {
          MemoryLocation Loc(I.getPointer(), I.getSize(), I.getAAInfo());
          CurrBase = *(*DS)[Analyze].insert(Loc).first;
          CurrTraits =
            LocBases.insert(std::make_pair(CurrBase, NoAccess.Id)).first;
          if (DefUse->hasMayDef(Loc) || DefUse->hasDef(Loc))
            CurrTraits->second &= Dependency.Id;
          else
            CurrTraits->second &= Shared.Id;
        }
    }
    // Let us consider the case where location access is performed by pointer.
    // *p means that address of location should be loaded from p using 'load'.
    for (const AliasSet &AS : DefUse->getExplicitAccesses()) {
      if (AS.isForwardingAliasSet() || AS.empty())
        continue; // The set is empty if it contains only unknown instructions.
      for (AliasSet::iterator I = AS.begin(), E = AS.end(); I != E; ++I) {
        Value *V = I.getPointer();
        if (Operator::getOpcode(V) == Instruction::BitCast ||
            Operator::getOpcode(V) == Instruction::AddrSpaceCast)
          V = cast<Operator>(V)->getOperand(0);
        if (auto *LI = dyn_cast<LoadInst>(V)) {
          const MemoryLocation *Loc = *(*DS)[Analyze].insert(
            MemoryLocation(I.getPointer(), I.getSize(), I.getAAInfo())).first;
          auto LocTraits = LocBases.find(Loc);
          assert(LocTraits != LocBases.end() &&
            "Traits of location must be initialized!");
          if (LocTraits->second == Private.Id ||
              LocTraits->second == Shared.Id)
            continue;
          const MemoryLocation *Ptr =
            *(*DS)[Analyze].insert(MemoryLocation::get(LI)).first;
          auto PtrTraits = LocBases.find(Ptr);
          assert(PtrTraits != LocBases.end() &&
            "Traits of location must be initialized!");
          if (PtrTraits->second == Shared.Id)
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
          LocTraits->second = Dependency.Id;
        }
      }
    }
    for (auto &LocTraits : LocBases) {
      // TODO (kaniandr@gmail.com) : This is very conservative assumption
      // but very simple to check. Let see an example:
      // int X[3];
      // X[2] = ...
      // for (int I = 0; I < 2; ++I)
      //   X[I] = ...;
      // X[2] = X[0] + X[1] + X[2];
      // If X will be defined as a last private only, X[2] will be lost after
      // copy out from this loop. So it must be defined as a first private.
      // Such check is complex even for scalar variables, for example:
      // int X;
      // *(&X + 10) = ...
      if (LocTraits.second == LastPrivate.Id ||
          LocTraits.second == SecondToLastPrivate.Id ||
          LocTraits.second == DynamicPrivate.Id)
        LocTraits.second &= FirstPrivate.Id;
      switch (LocTraits.second) {
        case Shared.Id:
          (*DS)[Shared].insert(LocTraits.first); ++NumShared; break;
        case Dependency.Id:
          (*DS)[Dependency].insert(LocTraits.first); ++NumDeps; break;
        case Private.Id:
          (*DS)[Private].insert(LocTraits.first); ++NumPrivate; break;
        case FirstPrivate.Id:
          (*DS)[FirstPrivate].insert(LocTraits.first); ++NumFPrivate; break;
        case FirstPrivate.Id & LastPrivate.Id:
          (*DS)[FirstPrivate].insert(LocTraits.first); ++NumFPrivate;
        case LastPrivate.Id:
          (*DS)[LastPrivate].insert(LocTraits.first); ++NumLPrivate; break;
        case FirstPrivate.Id & SecondToLastPrivate.Id:
          (*DS)[FirstPrivate].insert(LocTraits.first); ++NumFPrivate;
        case SecondToLastPrivate.Id:
          (*DS)[SecondToLastPrivate].insert(LocTraits.first); ++NumSToLPrivate;
          break;
        case FirstPrivate.Id & DynamicPrivate.Id:
          (*DS)[FirstPrivate].insert(LocTraits.first); ++NumFPrivate;
        case DynamicPrivate.Id:
          (*DS)[DynamicPrivate].insert(LocTraits.first); ++NumDPrivate; break;
      }
    }
    for (llvm::Value *Ptr : DefUse->getAddressAccesses()) {
      auto I = (*DS)[Analyze].insert(MemoryLocation(Ptr, 0)).first;
      (*DS)[AddressAccess].insert((*I)->Ptr);
      ++NumAddressAccess;
    }
  }
  for (DFRegion::region_iterator I = R->region_begin(), E = R->region_end();
       I != E; ++I)
    resolveCandidats(*I);
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

bool DefUseSet::hasExplicitAccess(const llvm::MemoryLocation &Loc) const {
  for (auto I = mExplicitAccesses.begin(), E = mExplicitAccesses.end();
       I != E; ++I) {
    if (I->isForwardingAliasSet() || I->empty())
      continue;
    for (auto LocI = I->begin(), LocE = I->end(); LocI != LocE; ++LocI)
      if (LocI->getValue() == Loc.Ptr)
        return true;
  }
  return false;
}

namespace {
bool evaluateAlias(Instruction *I, AliasSetTracker *AST, DefUseSet *DU) {
  assert(I && "Instruction must not be null!");
  assert(AST && "AliasSetTracker mut not be null!");
  assert(DU && "Value of def-use attribute must not be null!");
  assert(I->mayReadOrWriteMemory() && "Instruction must access memory!");
  Value *Ptr;
  uint16_t Size;
  std::function<void(const MemoryLocation &)> AddMust, AddMay;
  AliasAnalysis &AA = AST->getAliasAnalysis();
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    AddMust = [&DU](const MemoryLocation &Loc) { DU->addDef(Loc); };
    AddMay = [&DU](const MemoryLocation &Loc) { DU->addMayDef(Loc); };
    Ptr = SI->getPointerOperand();
    Value *Val = SI->getValueOperand();
    Size = AA.getTypeStoreSize(Val->getType());
    if (isa<AllocaInst>(Val))
      DU->addAddressAccess(Val);
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    AddMay = AddMust = [&DU](const MemoryLocation &Loc) { DU->addUse(Loc); };
    Ptr = LI->getPointerOperand();
    Size = AA.getTypeStoreSize(Ptr->getType());
  } else {
    return false;
  }
  AAMDNodes AATags;
  I->getAAMetadata(AATags);
  AliasSet &ASet = AST->getAliasSetForPointer(Ptr, Size, AATags);
  MemoryLocation Loc(Ptr, Size, AATags);
  for (auto APtr : ASet) {
    MemoryLocation ALoc(APtr.getValue(), APtr.getSize(), APtr.getAAInfo());
    // A condition below is necessary even in case of store instruction.
    // It is possible that Loc1 aliases Loc2 and Loc1 is already written
    // when Loc2 is evaluated. In this case evaluation of Loc1 should not be
    // repeated.
    if (DU->hasDef(ALoc))
      continue;
    AliasResult AR = AA.alias(Loc, ALoc);
    switch (AR) {
      case MayAlias: case PartialAlias: AddMay(ALoc); break;
      case MustAlias: AddMust(ALoc); break;
    }
  }
  return true;
}

void evaluateUnknownAlias(Instruction *I, AliasSetTracker *AST, DefUseSet *DU) {
  assert(I && "Instruction must not be null!");
  assert(AST && "AliasSetTracker must not be null!");
  assert(DU && "Value of def-use attribute must not be null!");
  assert(I->mayReadOrWriteMemory() && "Instruction must access memory!");
  DU->addUnknownInst(I);
  std::function<void(const MemoryLocation &)> AddMay;
  if (I->mayReadFromMemory() && I->mayWriteToMemory())
    AddMay = [&DU](const MemoryLocation &Loc) {
      DU->addUse(Loc);
      DU->addMayDef(Loc);
    };
  else if (I->mayReadFromMemory())
    AddMay = [&DU](const MemoryLocation &Loc) { DU->addUse(Loc); };
  else
    AddMay = [&DU](const MemoryLocation &Loc) { DU->addMayDef(Loc); };
  AliasAnalysis &AA = AST->getAliasAnalysis();
  auto ASetI = AST->begin(), ASetE = AST->end();
  for ( ; ASetI != ASetE; ++ASetI) {
    if (ASetI->isForwardingAliasSet() || ASetI->empty())
      continue;
    if (ASetI->aliasesUnknownInst(I, AA))
      break;
  }
  if (ASetI == ASetE)
    return;
  for (auto APtr : *ASetI) {
    MemoryLocation ALoc(APtr.getValue(), APtr.getSize(), APtr.getAAInfo());
    if (!DU->hasDef(ALoc) &&
        AA.getModRefInfo(I, ALoc) != AliasAnalysis::NoModRef)
      AddMay(ALoc);
  }
}
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
  AliasSetTracker *AST = Fwk->getTracker();
  AliasAnalysis &AA = AST->getAliasAnalysis();
  DefUseSet *DU = new DefUseSet(AA);
  N->addAttribute<DefUseAttr>(DU);
  DFBlock *DFB = dyn_cast<DFBlock>(N);
  if (!DFB)
    return;
  BasicBlock *BB = DFB->getBlock();
  assert(BB && "Basic block must not be null!");
  for (Instruction &I : BB->getInstList()) {
    if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(&I)) {
      Value *Ptr = GEPI->getPointerOperand();
      DU->addAddressAccess(Ptr);
      continue;
    }
    if (!I.mayReadOrWriteMemory())
      continue;
    DU->addExplicitAccess(&I);
    // 1. Must/may def-use information will be set for location accessed in a
    // current instruction.
    // 2. Must/may def-use information will be set for all locations (except
    // locations with unknown descriptions) aliases location accessed in a
    // current instruction.
    // Note that this attribute will be also set for locations which are
    // accessed implicitly.
    // 3. Unknown instructions will be remembered in DefUseAttr.
    if (!evaluateAlias(&I, AST, DU))
      evaluateUnknownAlias(&I, AST, DU);
  }
  DEBUG (
    dbgs() << "[DEFUSE] Def/Use locations for the following basic block:";
    DFB->getBlock()->print(dbgs());
    dbgs() << "Outward exposed must define locations:\n";
    for (auto &Loc : DU->getDefs())
      (printLocationSource(dbgs(), Loc.Ptr), dbgs() << "\n");
    dbgs() << "Outward exposed may define locations:\n";
    for (auto &Loc : DU->getMayDefs())
      (printLocationSource(dbgs(), Loc.Ptr), dbgs() << "\n");
    dbgs() << "Outward exposed uses:\n";
    for (auto &Loc : DU->getUses())
      (printLocationSource(dbgs(), Loc.Ptr), dbgs() << "\n");
    dbgs() << "[END DEFUSE]\n";
  );
}

bool DataFlowTraits<PrivateDFFwk*>::transferFunction(
  ValueType V, DFNode *N, PrivateDFFwk *, GraphType) {
  // Note, that transfer function is never evaluated for the entry node.
  assert(N && "Node must not be null!");
  DEBUG(
    dbgs() << "[TRANSFER PRIVATE]\n";
    if (auto DFB = dyn_cast<DFBlock>(N))
      DFB->getBlock()->dump();
    dbgs() << "IN:\n";
    V.dump();
    dbgs() << "OUT:\n";
  );
  PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
  assert(PV && "Data-flow value must not be null!");
  PV->setIn(std::move(V)); // Do not use V below to avoid undefined behavior.
  if (llvm::isa<DFExit>(N)) {
    if (PV->getOut() != PV->getIn()) {
      PV->setOut(PV->getIn());
      DEBUG(
        PV->getOut().dump();
        dbgs() << "[END TRANSFER]\n";
        );
      return true;
    }
    DEBUG(
      PV->getOut().dump();
      dbgs() << "[END TRANSFER]\n";);
    return false;
  }
  DefUseSet *DU = N->getAttribute<DefUseAttr>();
  assert(DU && "Value of def-use attribute must not be null!");
  LocationDFValue newOut(LocationDFValue::emptyValue());
  newOut.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.merge(PV->getIn());
  if (PV->getOut() != newOut) {
    PV->setOut(std::move(newOut));
    DEBUG(
      PV->getOut().dump();
      dbgs() << "[END TRANSFER]\n";
    );
    return true;
  }
  DEBUG(
    PV->getOut().dump();
    dbgs() << "[END TRANSFER]\n";
  );
  return false;
}

void PrivateDFFwk::collapse(DFRegion *R) {
  assert(R && "Region must not be null!");
  typedef RegionDFTraits<PrivateDFFwk *> RT;
  DefUseSet *DefUse = new DefUseSet(mAliasTracker->getAliasAnalysis());
  R->addAttribute<DefUseAttr>(DefUse);
  assert(DefUse && "Value of def-use attribute must not be null!");
  DFLoop *L = llvm::dyn_cast<DFLoop>(R);
  if (!L)
    return;
  // ExitingDefs is a set of must define locations (Defs) for the loop.
  // These locations always have definitions inside the loop regardless
  // of execution paths of iterations of the loop.
  DFNode *ExitNode = R->getExitNode();
  const LocationDFValue &ExitingDefs = RT::getValue(ExitNode, this);
  for (DFNode *N : L->getNodes()) {
    PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
    assert(PV && "Data-flow value must not be null!");
    DefUseSet *DU = N->getAttribute<DefUseAttr>();
    assert(DU && "Value of def-use attribute must not be null!");
    // We calculate a set of locations (Uses)
    // which get values outside the loop or from previous loop iterations.
    // These locations can not be privatized.
    for (auto &Loc : DU->getUses())
      if (!PV->getIn().contain(Loc))
        DefUse->addUse(Loc);
    // It is possible that some locations are only written in the loop.
    // In this case this locations are not located at set of node uses but
    // they are located at set of node defs.
    // We calculate a set of must define locations (Defs) for the loop.
    // These locations always have definitions inside the loop regardless
    // of execution paths of iterations of the loop.
    // The set of may define locations (MayDefs) for the loop is also
    // calculated. Let us use conservative assumption to calculate this
    // set and do not perform complicated control flow analysis. So
    // this set will also include all must define locations.
    for (auto &Loc : DU->getDefs()) {
      DefUse->addMayDef(Loc);
      if (ExitingDefs.contain(Loc))
        DefUse->addDef(Loc);
    }
    for (auto &Loc : DU->getMayDefs())
      DefUse->addMayDef(Loc);
    DefUse->addExplicitAccesses(DU->getExplicitAccesses());
    for (auto Loc : DU->getAddressAccesses())
      DefUse->addAddressAccess(Loc);
    for (auto Inst : DU->getUnknownInsts())
      DefUse->addUnknownInst(Inst);
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
  LS->setOut(std::move(V)); // Do not use V below to avoid undefined behavior.
  if (isa<DFEntry>(N)) {
    if (LS->getIn() != LS->getOut()) {
      LS->setIn(LS->getOut());
      return true;
    }
    return false;
  }
  DefUseSet *DU = N->getAttribute<DefUseAttr>();
  assert(DU && "Value of def-use attribute must not be null!");
  LocationSet newIn(DU->getUses());
  for (auto &Loc : LS->getOut()) {
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
    for (auto &Loc : newIn)
      (printLocationSource(dbgs(), Loc.Ptr), dbgs() << "\n");
    dbgs() << "OUT:\n";
    for (auto &Loc : V)
      (printLocationSource(dbgs(), Loc.Ptr), dbgs() << "\n");
    dbgs() << "[END LIVE]\n";
  );
  if (LS->getIn() != newIn) {
    LS->setIn(std::move(newIn));
    return true;
  }
  return false;
}
