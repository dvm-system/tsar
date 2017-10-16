//===--- DefinedMemory.h --- Defined Memory Analysis ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to determine must/may defined locations.
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AliasSetTracker.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/Debug.h>
#include <functional>
#include "tsar_dbg_output.h"
#include "DefinedMemory.h"
#include "DFRegionInfo.h"

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "def-mem"

char DefinedMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(DefinedMemoryPass, "def-mem",
  "Defined Memory Region Analysis", true, true)
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
#else
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
#endif
INITIALIZE_PASS_END(DefinedMemoryPass, "def-mem",
  "Defined Memory Region Analysis", true, true)

bool llvm::DefinedMemoryPass::runOnFunction(Function & F) {
  auto &RegionInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
#else
AliasAnalysis &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
#endif
  AliasSetTracker AliasTracker(AA);
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    AliasTracker.add(&*I);
  auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
  ReachDFFwk ReachDefFwk(AliasTracker, mDefInfo);
  solveDataFlowUpward(&ReachDefFwk, DFF);
  return false;
}

void DefinedMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.addRequired<DFRegionInfoPass>();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
  AU.addRequired<AliasAnalysis>();
#else
  AU.addRequired<AAResultsWrapperPass>();
#endif
  AU.setPreservesAll();
}

FunctionPass * llvm::createDefinedMemoryPass() {
  return new DefinedMemoryPass();
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
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
#else
  auto BB = I->getParent();
  auto F = BB->getParent();
  auto M = F->getParent();
  auto &DL = M->getDataLayout();
#endif
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    AddMust = [&DU](const MemoryLocation &Loc) { DU->addDef(Loc); };
    AddMay = [&DU](const MemoryLocation &Loc) { DU->addMayDef(Loc); };
    Ptr = SI->getPointerOperand();
    Value *Val = SI->getValueOperand();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
    Size = AA.getTypeStoreSize(Val->getType());
#else
    Size = DL.getTypeStoreSize(Val->getType());
#endif
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    AddMay = AddMust = [&DU](const MemoryLocation &Loc) { DU->addUse(Loc); };
    Ptr = LI->getPointerOperand();
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
    Size = AA.getTypeStoreSize(LI->getType());
#else
    Size = DL.getTypeStoreSize(LI->getType());
#endif
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
      case MustAlias:
        if (ALoc.Size == Loc.Size)
          AddMust(ALoc);
        else
          AddMay(ALoc);
          break;
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
  for (; ASetI != ASetE; ++ASetI) {
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
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
      AA.getModRefInfo(I, ALoc) != AliasAnalysis::NoModRef)
#else
      AA.getModRefInfo(I, ALoc) != MRI_NoModRef)
#endif
      AddMay(ALoc);
  }
}
}

void DataFlowTraits<ReachDFFwk*>::initialize(
  DFNode *N, ReachDFFwk *DFF, GraphType) {
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null");
  if (llvm::isa<DFRegion>(N))
    return;
  AliasSetTracker *AST = DFF->getTracker();
  AliasAnalysis &AA = AST->getAliasAnalysis();
  auto Pair = DFF->getDefInfo().insert(std::make_pair(N, std::make_tuple(
    llvm::make_unique<DefUseSet>(AA),
    llvm::make_unique<ReachSet>())));
  // DefUseSet will be calculated here for nodes different to regions.
  // For nodes which represented regions this attribute has been already
  // calculated in collapse() function.
  auto &DU = Pair.first->get<DefUseSet>();
  auto *DFB = dyn_cast<DFBlock>(N);
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
    if (!evaluateAlias(&I, AST, DU.get()))
      evaluateUnknownAlias(&I, AST, DU.get());
  }
  DEBUG(
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

bool DataFlowTraits<ReachDFFwk*>::transferFunction(
  ValueType V, DFNode *N, ReachDFFwk *DFF, GraphType) {
  // Note, that transfer function is never evaluated for the entry node.
  assert(N && "Node must not be null!");
  assert(DFF && "Data-flow framework must not be null");
  DEBUG(
    dbgs() << "[TRANSFER REACH]\n";
  if (auto DFB = dyn_cast<DFBlock>(N))
    DFB->getBlock()->dump();
  dbgs() << "IN:\n";
  dbgs() << "MUST REACH DEFINITIONS:\n";
  V.MustReach.dump();
  dbgs() << "MAY REACH DEFINITIONS:\n";
  V.MayReach.dump();
  dbgs() << "OUT:\n";
  );
  auto I = DFF->getDefInfo().find(N);
  assert(I != DFF->getDefInfo().end() &&
    I->get<ReachSet>() && I->get<DefUseSet>() &&
    "Data-flow value must be specified!");
  auto &RS = I->get<ReachSet>();
  RS->setIn(std::move(V)); // Do not use V below to avoid undefined behavior.
  if (llvm::isa<DFExit>(N)) {
    if (RS->getOut().MustReach != RS->getIn().MustReach ||
        RS->getOut().MayReach != RS->getIn().MayReach) {
      RS->setOut(RS->getIn());
      DEBUG(
        dbgs() << "MUST REACH DEFINITIONS:\n";
      RS->getOut().MustReach.dump();
      dbgs() << "MAY REACH DEFINITIONS:\n";
      RS->getOut().MayReach.dump();
      dbgs() << "[END TRANSFER]\n";
      );
      return true;
    }
    DEBUG(
      dbgs() << "MUST REACH DEFINITIONS:\n";
    RS->getOut().MustReach.dump();
    dbgs() << "MAY REACH DEFINITIONS:\n";
    RS->getOut().MayReach.dump();
    dbgs() << "[END TRANSFER]\n";
    );
    return false;
  }
  auto &DU = I->get<DefUseSet>();
  assert(DU && "Value of def-use attribute must not be null!");
  DefinitionInfo newOut;
  newOut.MustReach = std::move(LocationDFValue::emptyValue());
  newOut.MustReach.insert(DU->getDefs().begin(), DU->getDefs().end());
  newOut.MustReach.merge(RS->getIn().MustReach);
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
  newOut.MayReach.merge(RS->getIn().MayReach);
  if (RS->getOut().MustReach != newOut.MustReach ||
    RS->getOut().MayReach != newOut.MayReach) {
    RS->setOut(std::move(newOut));
    DEBUG(
      dbgs() << "MUST REACH DEFINITIONS:\n";
    RS->getOut().MustReach.dump();
    dbgs() << "MAY REACH DEFINITIONS:\n";
    RS->getOut().MayReach.dump();
    dbgs() << "[END TRANSFER]\n";
    );
    return true;
  }
  DEBUG(
    dbgs() << "MUST REACH DEFINITIONS:\n";
  RS->getOut().MustReach.dump();
  dbgs() << "MAY REACH DEFINITIONS:\n";
  RS->getOut().MayReach.dump();
  dbgs() << "[END TRANSFER]\n";
  );
  return false;
}

void ReachDFFwk::collapse(DFRegion *R) {
  assert(R && "Region must not be null!");
  typedef RegionDFTraits<ReachDFFwk *> RT;
  AliasSetTracker *AST = getTracker();
  AliasAnalysis &AA = AST->getAliasAnalysis();
  auto Pair = getDefInfo().insert(std::make_pair(R, std::make_tuple(
    llvm::make_unique<DefUseSet>(AA),
    llvm::make_unique<ReachSet>())));
  auto &DefUse = Pair.first->get<DefUseSet>();
  assert(DefUse && "Value of def-use attribute must not be null!");
  // ExitingDefs.MustReach is a set of must define locations (Defs) for the
  // loop. These locations always have definitions inside the loop regardless
  // of execution paths of iterations of the loop.
  DFNode *ExitNode = R->getExitNode();
  const DefinitionInfo &ExitingDefs = RT::getValue(ExitNode, this);
  for (DFNode *N : R->getNodes()) {
    auto DefItr = getDefInfo().find(N);
    assert(DefItr != getDefInfo().end() &&
      DefItr->get<ReachSet>() && DefItr->get<DefUseSet>() &&
      "Data-flow value must be specified!");
    auto &RS = DefItr->get<ReachSet>();
    auto &DU = DefItr->get<DefUseSet>();
    // We calculate a set of locations (Uses)
    // which get values outside the loop or from previous loop iterations.
    // These locations can not be privatized.
    for (auto &Loc : DU->getUses())
      if (!RS->getIn().MustReach.contain(Loc))
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
