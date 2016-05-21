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
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Dominators.h>
#include <functional>
#include <utility.h>
#include <declaration.h>
#include "tsar_private.h"
#include "tsar_graph.h"
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
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
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
  DefUseSet *DefUse = DFF.getAttribute<DefUseAttr>();
  // If inter-procedural analysis is not performed conservative assumption for
  // live variable analysis should be made. All locations except 'alloca' are
  // considered as alive before exit from this function.
  LocationSet MayLives;
  for (MemoryLocation &Loc : DefUse->getDefs()) {
    if (!Loc.Ptr || !isa<AllocaInst>(Loc.Ptr))
      MayLives.insert(Loc);
  }
  for (MemoryLocation &Loc : DefUse->getMayDefs()) {
    if (!Loc.Ptr || !isa<AllocaInst>(Loc.Ptr))
      MayLives.insert(Loc);
  }
  LS->setOut(std::move(MayLives));
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
    for (const MemoryLocation *Loc : DS[trait::Private]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " last privates:\n";
    for (const MemoryLocation *Loc : DS[trait::LastPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " second to last privates:\n";
    for (const MemoryLocation *Loc : DS[trait::SecondToLastPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " dynamic privates:\n";
    for (const MemoryLocation *Loc : DS[trait::DynamicPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " first privates:\n";
    for (const MemoryLocation *Loc : DS[trait::FirstPrivate]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " shared variables:\n";
    for (const MemoryLocation *Loc : DS[trait::Shared]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " dependencies:\n";
    for (const MemoryLocation *Loc : DS[trait::Dependency]) {
      errs() << Offset << "  ";
      printLocationSource(errs(), Loc->Ptr);
      errs() << "\n";
    }
    errs() << Offset << " addresses:\n";
    for (const Value *Ptr : DS[trait::AddressAccess]) {
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
    TraitMap LocBases;
    resolveAccesses(R->getLatchNode(), R->getExitNode(),
      DefUse, LS, LocBases, DS);
    resolvePointers(DefUse, LocBases, DS);
    storeResults(DefUse, LocBases, DS);
    resolveAddresses(L, DefUse, DS);
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
    // as analysis which is performed for variables form a source code.
    // For example, the base location for (short&)X is a memory location with
    // a size equal to size_of(short) regardless the size of X which might have
    // type int. Be careful when results of this analysis is propagated for
    // variables from a source code.
    // for (...) { (short&X) = ... ;} ... = X;
    // The short part of X will be recognized as last private, but the whole
    // variable X must be also set to first private to preserve the value
    // obtained before the loop.
    const MemoryLocation *CurrBase = *(*DS)[trait::Analyze].insert(
      MemoryLocation(I.getPointer(), I.getSize(), I.getAAInfo())).first;
    auto CurrTraits =
      LocBases.insert(std::make_pair(CurrBase, trait::NoAccess)).first;
    for (; I != E; ++I) {
      MemoryLocation Loc(I.getPointer(), I.getSize(), I.getAAInfo());
      const MemoryLocation *Base = *(*DS)[trait::Analyze].insert(Loc).first;
      if (CurrBase != Base) {
        // Current alias set contains memory locations with different bases.
        // So there are explicitly accessed locations with different bases
        // which alias each other.
        if (CurrTraits->second != trait::Shared)
          CurrTraits->second = trait::Dependency;
        break;
      }
      if (!DefUse->hasUse(Loc)) {
        if (!LS->getOut().overlap(Loc))
          CurrTraits->second &= trait::Private;
        else if (DefUse->hasDef(Loc))
          CurrTraits->second &= trait::LastPrivate;
        else if (LatchDefs.MustReach.contain(Loc) &&
          !ExitingDefs.MayReach.overlap(Loc))
          // These location will be stored as second to last private, i.e.
          // the last definition of these locations is executed on the
          // second to the last loop iteration (on the last iteration the
          // loop condition check is executed only).
          // It is possible that there is only one (last) iteration in
          // the loop. In this case the location has not been assigned and
          // must be declared as a first private.
          CurrTraits->second &=
            trait::SecondToLastPrivate & trait::FirstPrivate;
        else
          // There is no certainty that the location is always assigned
          // the value in the loop. Therefore, it must be declared as a
          // first private, to preserve the value obtained before the loop
          // if it has not been assigned.
          CurrTraits->second &= trait::DynamicPrivate & trait::FirstPrivate;
      } else if (DefUse->hasMayDef(Loc) || DefUse->hasDef(Loc)) {
        CurrTraits->second &= trait::Dependency;
      } else {
        CurrTraits->second &= trait::Shared;
      }
    }
    for (; I != E; ++I) {
      MemoryLocation Loc(I.getPointer(), I.getSize(), I.getAAInfo());
      CurrBase = *(*DS)[trait::Analyze].insert(Loc).first;
      CurrTraits =
        LocBases.insert(std::make_pair(CurrBase, trait::NoAccess)).first;
      if (DefUse->hasMayDef(Loc) || DefUse->hasDef(Loc))
        CurrTraits->second &= trait::Dependency;
      else
        CurrTraits->second &= trait::Shared;
    }
    // The following two tests check whether whole base location will be written
    // in the loop. Let us propose the following explanation. Consider a loop
    // where some location Loc is written and this memory is going to be read
    // after an exit from this loop. It is possible that base for this location
    // BaseLoc covers this location, so not a whole memory comprises BaseLoc is
    // written in the loop. To avoid a loss of data stored before the loop
    // execution in a part of memory which is not written after copy out from
    // this loop the BaseLoc must be also treated as a first private.
    if (CurrTraits->second == trait::LastPrivate &&
        !ExitingDefs.MustReach.contain(*CurrBase))
      CurrTraits->second &= trait::FirstPrivate;
    if (CurrTraits->second == trait::SecondToLastPrivate &&
        !LatchDefs.MustReach.contain(*CurrBase))
      CurrTraits->second &= trait::FirstPrivate;
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
        const MemoryLocation *Loc = *(*DS)[trait::Analyze].insert(
          MemoryLocation(I.getPointer(), I.getSize(), I.getAAInfo())).first;
        auto LocTraits = LocBases.find(Loc);
        assert(LocTraits != LocBases.end() &&
          "Traits of location must be initialized!");
        if (LocTraits->second == trait::Private ||
          LocTraits->second == trait::Shared)
          continue;
        const MemoryLocation *Ptr =
          *(*DS)[trait::Analyze].insert(MemoryLocation::get(LI)).first;
        auto PtrTraits = LocBases.find(Ptr);
        assert(PtrTraits != LocBases.end() &&
          "Traits of location must be initialized!");
        if (PtrTraits->second == trait::Shared)
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
        LocTraits->second = trait::Dependency;
      }
    }
  }
}

void PrivateRecognitionPass::storeResults(const tsar::DefUseSet *DefUse,
    TraitMap &LocBases, tsar::DependencySet *DS) {
  assert(DS && "Dependency set must not be null!");
  for (auto &LocTraits : LocBases) {
    switch (LocTraits.second) {
      case trait::Shared:
        (*DS)[trait::Shared].insert(LocTraits.first); ++NumShared; break;
      case trait::Dependency:
        (*DS)[trait::Dependency].insert(LocTraits.first); ++NumDeps; break;
      case trait::Private:
        (*DS)[trait::Private].insert(LocTraits.first); ++NumPrivate; break;
      case trait::FirstPrivate:
        (*DS)[trait::FirstPrivate].insert(LocTraits.first);
        ++NumFPrivate;
        break;
      case trait::FirstPrivate & trait::LastPrivate:
        (*DS)[trait::FirstPrivate].insert(LocTraits.first); ++NumFPrivate;
      case trait::LastPrivate:
        (*DS)[trait::LastPrivate].insert(LocTraits.first); ++NumLPrivate;
        break;
      case trait::FirstPrivate & trait::SecondToLastPrivate:
        (*DS)[trait::FirstPrivate].insert(LocTraits.first); ++NumFPrivate;
      case trait::SecondToLastPrivate:
        (*DS)[trait::SecondToLastPrivate].insert(LocTraits.first);
        ++NumSToLPrivate;
        break;
      case trait::FirstPrivate & trait::DynamicPrivate:
        (*DS)[trait::FirstPrivate].insert(LocTraits.first); ++NumFPrivate;
      case trait::DynamicPrivate:
        (*DS)[trait::DynamicPrivate].insert(LocTraits.first); ++NumDPrivate;
        break;
    }
  }
}

void PrivateRecognitionPass::resolveAddresses(
  const DFLoop *L, const tsar::DefUseSet *DefUse, tsar::DependencySet *DS) {
  assert(L && "Loop must not be null!");
  assert(DefUse && "Def-use set must not be null!");
  assert(DS && "Dependency set must not be null!");
  assert(DefUse == L->getAttribute<DefUseAttr>() &&
    "Def-use set must be related to the specified loop!");
  assert(DS == L->getAttribute<DependencyAttr>() &&
    "Dependency set must be related to the specified loop!");
  for (llvm::Value *Ptr : DefUse->getAddressAccesses()) {
    auto I = (*DS)[trait::Analyze].insert(MemoryLocation(Ptr, 0)).first;
    // Do not remember an address:
    // * if it is stored in some location, for example isa<LoadInst>((*I)->Ptr),
    //  locations are analyzed separately;
    // * if it points to a temporary location and should not be analyzed:
    // for example, a result of a call can be a pointer.
    if (!(*I)->Ptr ||
        !isa<AllocaInst>((*I)->Ptr) && !isa<GlobalVariable>((*I)->Ptr))
      continue;
    Loop *Lp = L->getLoop();
    // If this is an address of a location declared in the loop do not
    // remember it.
    if (auto AI = dyn_cast<AllocaInst>((*I)->Ptr))
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
        (*DS)[trait::AddressAccess].insert((*I)->Ptr);
        ++NumAddressAccess;
        break;
      }
    }
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
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
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
  assert(AST && "AliasSetTracker must not be null!");
  assert(DU && "Value of def-use attribute must not be null!");
  assert(I->mayReadOrWriteMemory() && "Instruction must access memory!");
  Value *Ptr;
  uint64_t Size;
  std::function<void(const MemoryLocation &)> AddMust, AddMay;
  AliasAnalysis &AA = AST->getAliasAnalysis();
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    AddMust = [&DU](const MemoryLocation &Loc) { DU->addDef(Loc); };
    AddMay = [&DU](const MemoryLocation &Loc) { DU->addMayDef(Loc); };
    Ptr = SI->getPointerOperand();
    Value *Val = SI->getValueOperand();
    Size = AA.getTypeStoreSize(Val->getType());
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    AddMay = AddMust = [&DU](const MemoryLocation &Loc) { DU->addUse(Loc); };
    Ptr = LI->getPointerOperand();
    Size = AA.getTypeStoreSize(LI->getType());
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
    if (I.getType() && I.getType()->isPointerTy())
      DU->addAddressAccess(&I);
    for (auto OpI = I.value_op_begin(), OpE = I.value_op_end();
         OpI != OpE; ++OpI) {
      if (isa<AllocaInst>(*OpI) || isa<GlobalVariable>(*OpI))
        DU->addAddressAccess(*OpI);
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
    dbgs() << "MUST REACH DEFINITIONS:\n";
    V.MustReach.dump();
    dbgs() << "MAY REACH DEFINITIONS:\n";
    V.MayReach.dump();
    dbgs() << "OUT:\n";
  );
  PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
  assert(PV && "Data-flow value must not be null!");
  PV->setIn(std::move(V)); // Do not use V below to avoid undefined behavior.
  if (llvm::isa<DFExit>(N)) {
    if (PV->getOut().MustReach != PV->getIn().MustReach ||
        PV->getOut().MayReach != PV->getIn().MayReach) {
      PV->setOut(PV->getIn());
      DEBUG(
        dbgs() << "MUST REACH DEFINITIONS:\n";
        PV->getOut().MustReach.dump();
        dbgs() << "MAY REACH DEFINITIONS:\n";
        PV->getOut().MayReach.dump();
        dbgs() << "[END TRANSFER]\n";
      );
      return true;
    }
    DEBUG(
      dbgs() << "MUST REACH DEFINITIONS:\n";
      PV->getOut().MustReach.dump();
      dbgs() << "MAY REACH DEFINITIONS:\n";
      PV->getOut().MayReach.dump();
      dbgs() << "[END TRANSFER]\n";
    );
    return false;
  }
  DefUseSet *DU = N->getAttribute<DefUseAttr>();
  assert(DU && "Value of def-use attribute must not be null!");
  DefinitionInfo newOut;
  newOut.MustReach = std::move(LocationDFValue::emptyValue());
  newOut.MustReach.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.MustReach.merge(PV->getIn().MustReach);
  newOut.MayReach = std::move(LocationDFValue::emptyValue());
  // newOut.MayReach must contain both must and may defined locations.
  // Let us consider an example:
  // for(...) {
  //   if (...) {
  //     X = ...
  //     break;
  // }
  // MustReach must not contain X, because if conditional is always false
  // the X variable will not be written. But MayReach must contain X.
  // In the basic block that is associated with a body of if statement X is a
  // must defined location. So it is necessary to insert must defined locations
  // in the MayReach collection.
  newOut.MayReach.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.MayReach.insert(DU->getMayDefs().begin(), DU->getMayDefs().end());
  newOut.MayReach.merge(PV->getIn().MayReach);
  if (PV->getOut().MustReach != newOut.MustReach ||
      PV->getOut().MayReach != newOut.MayReach) {
    PV->setOut(std::move(newOut));
    DEBUG(
      dbgs() << "MUST REACH DEFINITIONS:\n";
      PV->getOut().MustReach.dump();
      dbgs() << "MAY REACH DEFINITIONS:\n";
      PV->getOut().MayReach.dump();
      dbgs() << "[END TRANSFER]\n";
    );
    return true;
  }
  DEBUG(
    dbgs() << "MUST REACH DEFINITIONS:\n";
    PV->getOut().MustReach.dump();
    dbgs() << "MAY REACH DEFINITIONS:\n";
    PV->getOut().MayReach.dump();
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
  // ExitingDefs.MustReach is a set of must define locations (Defs) for the
  // loop. These locations always have definitions inside the loop regardless
  // of execution paths of iterations of the loop.
  DFNode *ExitNode = R->getExitNode();
  const DefinitionInfo &ExitingDefs = RT::getValue(ExitNode, this);
  for (DFNode *N : R->getNodes()) {
    PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
    assert(PV && "Data-flow value must not be null!");
    DefUseSet *DU = N->getAttribute<DefUseAttr>();
    assert(DU && "Value of def-use attribute must not be null!");
    // We calculate a set of locations (Uses)
    // which get values outside the loop or from previous loop iterations.
    // These locations can not be privatized.
    for (auto &Loc : DU->getUses())
      if (!PV->getIn().MustReach.contain(Loc))
        DefUse->addUse(Loc);
    // It is possible that some locations are only written in the loop.
    // In this case this locations are not located at set of node uses but
    // they are located at set of node defs.
    // We calculate a set of must define locations (Defs) for the loop.
    // These locations always have definitions inside the loop regardless
    // of execution paths of iterations of the loop.
    // The set of may define locations (MayDefs) for the loop is also
    // calculated.
    // Note that if ExitingDefs.MustReach does not comprises a location it
    // means that it may have definition it the loop but it does not mean
    // that ExitingDefs.MayReach comprises it. In the following example
    // may definitions for the loop contains X, but ExitingDefs.MayReach does
    // not contain it (there is no iteration on which exit from this loop
    // occurs and X is written).
    // for (;;) {
    //   if (...)
    //     break;
    //   if (...)
    //     X = ...;
    // }
    for (auto &Loc : DU->getDefs()) {
      if (ExitingDefs.MustReach.contain(Loc))
        DefUse->addDef(Loc);
      else
        DefUse->addMayDef(Loc);
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
