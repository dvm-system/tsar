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
#include "llvm/ADT/Statistic.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "llvm/IR/InstIterator.h"
#include <llvm/Analysis/LoopInfo.h>
#include "llvm/Analysis/ScalarEvolution.h"
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/Config/llvm-config.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include <llvm/Analysis/Dominators.h>
#else
#include <llvm/IR/Dominators.h>
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 5)
#include <llvm/DebugInfo.h>
#else
#include <llvm/IR/DebugInfo.h>
#endif

#include <utility.h>
#include "tsar_private.h"
#include "tsar_graph.h"
#include "tsar_pass.h"
#include "tsar_utility.h"

#include <declaration.h>
#include "tsar_dbg_output.h"

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "private"

STATISTIC(NumPrivate, "Number of private allocas found");
STATISTIC(NumLPrivate, "Number of last private allocas found");
STATISTIC(NumSToLPrivate, "Number of second to last private allocas found");
STATISTIC(NumDPrivate, "Number of dynamic private allocas found");
STATISTIC(NumDeps, "Number of unsorted dependencies found");
STATISTIC(NumShared, "Number of shraed allocas found");

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
  BasicBlock &BB = F.getEntryBlock();
  for (BasicBlock::iterator I = BB.begin(), EI = --BB.end(); I != EI; ++I) {
    AllocaInst *AI = dyn_cast<AllocaInst>(I);
    if (AI && isAllocaPromotable(AI)) 
      mAnlsAllocas.insert(AI);
  }
  if (mAnlsAllocas.empty())
    return false;
  DFFunction DFF(&F);
  buildLoopRegion(std::make_pair(&F, &LpInfo), &DFF);
  PrivateDFFwk PrivateFWK(mAnlsAllocas, mPrivates);
  solveDataFlowUpward(&PrivateFWK, &DFF);
  LiveDFFwk LiveFwk(mAnlsAllocas);
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
    for (AllocaInst *AI : DS[Private]) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), AI);
    }
    errs() << Offset << " last privates:\n";
    for (AllocaInst *AI : DS[LastPrivate]) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), AI);
    }
    errs() << Offset << " second to last privates:\n";
    for (AllocaInst *AI: DS[SecondToLastPrivate]) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), AI);
    }
    errs() << Offset << " dynamic privates:\n";
    for (AllocaInst *AI: DS[DynamicPrivate]) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), AI);
    }
    errs() << Offset << " shared variables:\n";
    for (AllocaInst *AI : DS[Shared]) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), AI);
    }
    errs() << Offset << " dependencies:\n";
    for (AllocaInst *AI : DS[Dependency]) {
      errs() << Offset << "  ";
      printAllocaSource(errs(), AI);
    }
    errs() << "\n";
  });
  return false;
}

void PrivateRecognitionPass::resolveCandidats(DFRegion *R) {
  assert(R && "Region must not be null!");
  if (llvm::isa<DFLoop>(R)) {
    DependencySet *DS = R->getAttribute<DependencyAttr>();
    assert(DS && "List of privatizable candidats must not be null!");
    LiveSet *LS = R->getAttribute<LiveAttr>();
    assert(LS && "List of live allocas must not be null!");
    for (llvm::AllocaInst *AI : mAnlsAllocas) {
      if (LS->getOut().count(AI) != 0)
        continue;
      if (DS->is(LastPrivate, AI)) {
        (*DS)[LastPrivate].erase(AI), --NumLPrivate;
        (*DS)[Private].insert(AI), ++NumPrivate;
      }
      else if (DS->is(SecondToLastPrivate, AI)) {
        (*DS)[SecondToLastPrivate].erase(AI), --NumSToLPrivate;
        (*DS)[Private].insert(AI), ++NumPrivate;
      }
      else if (DS->is(DynamicPrivate, AI)) {
        (*DS)[DynamicPrivate].erase(AI), --NumDPrivate;
        (*DS)[Private].insert(AI), ++NumPrivate;
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
  AU.addRequired<ScalarEvolution>();
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
  for (Instruction &I : BB->getInstList()) {
    if (isa<StoreInst>(I)) {
      if (!isa<AllocaInst>(I.getOperand(1))) {
        // TODO(kaniandr@gmail.com): implement this feature.
        errs() << "line " << I.getDebugLoc().getLine() <<
          ": unimplemented feature: a value can be stored in variable only!\n";
        exit(1);
      }
      AllocaInst *AI = cast<AllocaInst>(I.getOperand(1));
      if (Fwk->isAnalyse(AI))
        DU->addDef(AI);
    } else if (isa<LoadInst>(I)) {
      if (!isa<AllocaInst>(I.getOperand(0))) {
        // TODO(kaniandr@gmail.com): implement this feature.
        errs() << "line " << I.getDebugLoc().getLine() <<
          ": unimplemented feature: a value can be loaded from variable only!\n";
        exit(1);
      }
      AllocaInst *AI = cast<AllocaInst>(I.getOperand(0));
      if (Fwk->isAnalyse(AI) && !DU->hasDef(AI))
        DU->addUse(AI);
    }
  }
  DEBUG (
    dbgs() << "[DEFUSE] Def/Use allocas for the following basic block:";
    DFB->getBlock()->print(dbgs());
    dbgs() << "Outward exposed definitions:\n";
    for (AllocaInst *AI : DU->getDefs())
      printAllocaSource(dbgs(), AI);
    dbgs() << "Outward exposed uses:\n";
    for (AllocaInst *AI : DU->getUses())
      printAllocaSource(dbgs(), AI);
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
  AllocaDFValue newOut(AllocaDFValue::emptyValue());
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
  // * ExitingDefs is a set of must define allocas (Defs) for the loop.
  //   These allocas always have definitions inside the loop regardless
  //   of execution paths of iterations of the loop.
  // * LatchDefs is a set of must define allocas before a branch to
  //   a next arbitrary iteration.
  DFNode *ExitNode = R->getExitNode();  
  const AllocaDFValue &ExitingDefs = RT::getValue(ExitNode, this);
  DFNode *LatchNode = R->getLatchNode();
  const AllocaDFValue &LatchDefs = RT::getValue(LatchNode, this);
  DependencySet::AllocaSet AllNodesAccesses;
  DependencySet::AllocaSet MayDefs;
  for (DFNode *N : L->getNodes()) {
    PrivateDFValue *PV = N->getAttribute<PrivateDFAttr>();
    assert(PV && "Data-flow value must not be null!");
    DefUseSet *DU = N->getAttribute<DefUseAttr>();
    assert(DU && "Value of def-use attribute must not be null!");
    // First, we calculat a set of allocas accessed in loop nodes.
    // Second, we calculate a set of allocas (Uses)
    // which get values outside the loop or from previouse loop iterations.
    // These allocas can not be privatized.
    for (AllocaInst *AI : DU->getUses()) {
      AllNodesAccesses.insert(AI);
      if (!PV->getIn().exist(AI))
        DefUse->addUse(AI);
    }
    // It is possible that some allocas are only written in the loop.
    // In this case this allocas are not located at set of node uses but
    // they are located at set of node defs.
    // We also calculate a set of must define allocas (Defs) for the loop.
    // These allocas always have definitions inside the loop regardless
    // of execution paths of iterations of the loop.
    // The set of may define allocas (MayDefs) for the loop is also calculated.
    // This set also include all must define allocas.
    for (AllocaInst *AI : DU->getDefs()) {
      AllNodesAccesses.insert(AI);
      MayDefs.insert(AI);
      if (ExitingDefs.exist(AI))
        DefUse->addDef(AI);
    }
  }
  // Calculation of a last private variables differs depending on internal
  // representation of a loop. There are two type of representations.
  // 1. The first type has a following pattern:
  //   iter: if (...) goto exit;
  //             ...
  //         goto iter;
  //   exit:
  // For example, representation of a for-loop refers to this type.
  // In this case allocas from the LatchDefs collection should be used
  // to determine candidates for last private variables. These allocas will be
  // stored in the SecondToLastPrivates collection, i.e. the last definition of
  // these allocas is executed on the second to the last loop iteration
  // (on the last iteration the loop condition check is executed only).
  // 2. The second type has a following patterm:
  //   iter:
  //             ...
  //         if (...) goto exit; else goto iter;
  //   exit:
  // For example, representation of a do-while-loop refers to this type.
  // In this case allocas from the ExitDefs collection should be used.
  // The result will be stored in the LastPrivates collection.
  // In some cases it is impossible to determine in static an iteration
  // where the last definition of an alloca have been executed. Such allocas
  // will be stored in the DynamicPrivates collection.
  // Note, in this step only candidates for last privates and privates
  // variables are calculated. The result should be corrected further.
  DependencySet *DS = new DependencySet;
  mPrivates.insert(std::make_pair(L->getLoop(), DS));
  R->addAttribute<DependencyAttr>(DS);
  assert(DS && "Result of analysis must not be null!");
  for (AllocaInst *AI : AllNodesAccesses)
    if (!DefUse->hasUse(AI))
      if (DefUse->hasDef(AI))
        (*DS)[LastPrivate].insert(AI), ++NumLPrivate;
      else if (LatchDefs.exist(AI))
        (*DS)[SecondToLastPrivate].insert(AI), ++NumSToLPrivate;
      else
        (*DS)[DynamicPrivate].insert(AI), ++NumDPrivate;
    else if (MayDefs.count(AI) != 0)
      (*DS)[Dependency].insert(AI), ++NumDeps;
    else
      (*DS)[Shared].insert(AI), ++NumShared;
}

void DataFlowTraits<LiveDFFwk *>::initialize(DFNode *N, LiveDFFwk *Fwk, GraphType) {
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
  LiveDFFwk::AllocaSet newIn(DU->getUses());
  for (AllocaInst *AI : V) {
    if (!DU->hasDef(AI))
      newIn.insert(AI);
  }
  DEBUG(
    dbgs() << "[LIVE] Live allocas analysis, transfer function results for:";
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
    for (AllocaInst *AI : newIn)
      printAllocaSource(dbgs(), AI);
    dbgs() << "OUT:\n";
    for (AllocaInst *AI : V)
      printAllocaSource(dbgs(), AI);
    dbgs() << "[END LIVE]\n";
  );
  if (LS->getIn() != newIn) {
    LS->setIn(std::move(newIn));
    return true;
  }
  return false;
}
