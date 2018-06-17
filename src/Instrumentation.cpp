//===- tsar_instrumentation.cpp - TSAR Instrumentation Engine ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements LLVM IR level instrumentation engine.
//
//===----------------------------------------------------------------------===//
//
#include "Instrumentation.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolutionExpander.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Analysis/ScalarEvolution.h>

#include "tsar_utility.h"
#include "Intrinsics.h"
#include "CanonicalLoop.h"
#include "DFRegionInfo.h"
#include "tsar_memory_matcher.h"
#include "tsar_transformation.h"
#include "tsar_pass_provider.h"

#include <map>
#include <vector>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "instrumentation"

typedef FunctionPassProvider<
  TransformationEnginePass,
  DFRegionInfoPass,
  LoopInfoWrapperPass,
  CanonicalLoopPass,
  MemoryMatcherImmutableWrapper,
  ScalarEvolutionWrapperPass,
  DominatorTreeWrapperPass> InstrumentationPassProvider;

STATISTIC(NumInstLoop, "Number of instrumented loops");

INITIALIZE_PROVIDER_BEGIN(InstrumentationPassProvider, "instrumentation-provider",
  "Instrumentation Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(InstrumentationPassProvider, "instrumentation-provider",
  "Instrumentation Provider")

char InstrumentationPass::ID = 0;
INITIALIZE_PASS_BEGIN(InstrumentationPass, "instrumentation",
  "LLVM IR Instrumentation", false, false)
INITIALIZE_PASS_DEPENDENCY(InstrumentationPassProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_END(InstrumentationPass, "instrumentation",
  "LLVM IR Instrumentation", false, false)


bool InstrumentationPass::runOnModule(Module &M) {
  releaseMemory();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  InstrumentationPassProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass &TEP) {
      TEP.setContext(M, TfmCtx);
  });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  InstrumentationPassProvider::initialize<MemoryMatcherImmutableWrapper>(
    [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
      Wrapper.set(*MMWrapper);
  });
  Instrumentation Instr(M, this);
  return true;
}

void InstrumentationPass::releaseMemory() {}

void InstrumentationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<InstrumentationPassProvider>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
}

ModulePass * llvm::createInstrumentationPass() {
  return new InstrumentationPass();
}

Instrumentation::Instrumentation(Module &M, InstrumentationPass *I)
  : mInstrPass(I), mDIStrings(DIStringRegister::numberOfItemTypes()) {
  const std::string& ModuleName = M.getModuleIdentifier();
  auto DIPoolTy = PointerType::getUnqual(Type::getInt8PtrTy(M.getContext()));
  mDIPool = new GlobalVariable(M, DIPoolTy, false,
    GlobalValue::LinkageTypes::ExternalLinkage,
    ConstantPointerNull::get(DIPoolTy), "sapfor.di.pool", nullptr);
  mDIPool->setAlignment(4);
  mDIPool->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  //create function for debug information initialization
  auto Type = FunctionType::get(Type::getVoidTy(M.getContext()),
    {Type::getInt64Ty(M.getContext())}, false);
  mInitDIAll = Function::Create(
    Type, GlobalValue::LinkageTypes::InternalLinkage, "sapfor.init.di.all", &M);
  mInitDIAll->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  mInitDIAll->arg_begin()->setName("Offset");
  auto EntryBB =
    BasicBlock::Create(mInitDIAll->getContext(), "entry", mInitDIAll);
  ReturnInst::Create(mInitDIAll->getContext(), EntryBB);
  reserveIncompleteDIStrings(M);
  regFunctions(M);
  regGlobals(M);
  visit(M.begin(), M.end());
  //insert call to allocate debug information pool
  auto Fun = getDeclaration(&M, IntrinsicId::allocate_pool);
  //getting numb of registrated debug strings by the value returned from
  //registrator. don't go through function to count inserted calls.
  auto Idx = ConstantInt::get(Type::getInt64Ty(M.getContext()),
    mDIStrings.numberOfIDs());
  CallInst::Create(Fun, { mDIPool, Idx}, "", &(*inst_begin(mInitDIAll)));
  regTypes(M);
  if(M.getFunction("main") != nullptr)
    instrumentateMain(M);
}

void Instrumentation::reserveIncompleteDIStrings(llvm::Module &M) {
  auto DbgLocIdx = DIStringRegister::indexOfItemType<DILocation *>();
  createInitDICall(
    Twine("type=") + "file_name" + "*" +
    "file=" + M.getSourceFileName() + "*" + "*", DbgLocIdx);
}

void Instrumentation::visitAllocaInst(llvm::AllocaInst &I) {
  DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto MD = getMetadata(&I);
  auto Idx = mDIStrings.regItem(&I);
  BasicBlock::iterator InsertBefore(I);
  ++InsertBefore;
  regValue(&I, I.getAllocatedType(), MD, Idx, *InsertBefore, *I.getModule());
}

void Instrumentation::visitReturnInst(llvm::ReturnInst &I) {
  DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto Fun = getDeclaration(I.getModule(), IntrinsicId::func_end);
  unsigned Idx = mDIStrings[I.getFunction()];
  auto DIFunc = createPointerToDI(Idx, I);
  auto Call = CallInst::Create(Fun, {DIFunc}, "", &I);
  Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
}

std::tuple<Value *, Value *, Value *, bool>
Instrumentation::computeLoopBounds(Loop &L, IntegerType &IntTy,
    ScalarEvolution &SE, DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS) {
  auto *Region = RI.getRegionFor(&L);
  assert(Region && "Region must not be null!");
  auto CanonItr = CS.find_as(Region);
  if (CanonItr == CS.end())
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  auto *Header = L.getHeader();
  auto *End = (*CanonItr)->getEnd();
  if (!End)
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  auto *EndTy = End->getType();
  if (!EndTy || !EndTy->isIntegerTy() ||
      EndTy->getIntegerBitWidth() > IntTy.getBitWidth())
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  bool Signed = false;
  bool Unsigned = false;
  for (auto *U : End->users())
    if (auto *Cmp = dyn_cast<CmpInst>(U)) {
      Signed |= Cmp->isSigned();
      Unsigned |= Cmp->isUnsigned();
    }
  // Is sign known?
  if (Signed == Unsigned)
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  auto InstrMD = MDNode::get(Header->getContext(), {});
  assert(L.getLoopPreheader() && "For-loop must have a preheader!");
  auto &InsertBefore = L.getLoopPreheader()->back();
  // Compute start if possible.
  auto *Start = (*CanonItr)->getStart();
  if (Start) {
    auto StartTy = Start->getType();
    if (StartTy && StartTy->isIntegerTy()) {
      if (StartTy->getIntegerBitWidth() > IntTy.getBitWidth()) {
        Start = nullptr;
      } else if (StartTy->getIntegerBitWidth() < IntTy.getBitWidth()) {
        Start = CastInst::Create(Signed ? Instruction::SExt : Instruction::ZExt,
          Start, &IntTy, "loop.start", &InsertBefore);
        cast<Instruction>(Start)->setMetadata("sapfor.da", InstrMD);
      }
    }
  }
  // It is unsafe to compute step and end bound if for-loop is not canonical.
  // In this case step and end bound may depend on the loop iteration.
  if (!(*CanonItr)->isCanonical())
    return std::make_tuple(Start, nullptr, nullptr, Signed);
  // Compute end if possible.
  if (isa<Instruction>(End)) {
    SmallVector<Instruction *, 8> EndClone;
    if (!cloneChain(cast<Instruction>(End), EndClone, &InsertBefore, &DT)) {
      End = nullptr;
    } else {
      for (auto I = EndClone.rbegin(), EI = EndClone.rend(); I != EI; ++I) {
        (*I)->insertBefore(&InsertBefore);
        (*I)->setMetadata("sapfor.da", InstrMD);
      }
      if (!EndClone.empty())
        End = EndClone.front();
    }
  }
  if (End && EndTy->getIntegerBitWidth() < IntTy.getBitWidth()) {
    End = CastInst::Create(Signed ? Instruction::SExt : Instruction::ZExt,
      End, &IntTy, "loop.end", &InsertBefore);
    cast<Instruction>(End)->setMetadata("sapfor.da", InstrMD);
  }
  auto Step = computeSCEV(
    (*CanonItr)->getStep(), IntTy, Signed, SE, DT, InsertBefore);
  return std::make_tuple(Start, End, Step, Signed);
}

Value * Instrumentation::computeSCEV(const SCEV *ExprSCEV,
    IntegerType &IntTy, bool Signed, ScalarEvolution &SE, DominatorTree &DT,
    Instruction &InsertBefore) {
  if (!ExprSCEV)
    return nullptr;
  auto *ExprTy = ExprSCEV->getType();
  if (!ExprTy || !ExprTy->isIntegerTy() ||
      ExprTy->getIntegerBitWidth() > IntTy.getBitWidth())
    return nullptr;
  auto InstrMD = MDNode::get(InsertBefore.getContext(), {});
  if (ExprTy->getIntegerBitWidth() < IntTy.getBitWidth())
    ExprSCEV = Signed ? SE.getSignExtendExpr(ExprSCEV, &IntTy) :
      SE.getZeroExtendExpr(ExprSCEV, &IntTy);
  SCEVExpander Exp(SE, InsertBefore.getModule()->getDataLayout(), "");
  auto Expr = Exp.expandCodeFor(ExprSCEV, &IntTy, &InsertBefore);
  SmallVector<Use *, 4> ExprNotDom;
  if (auto ExprInst = dyn_cast<Instruction>(Expr)) {
    if (findNotDom(ExprInst, &InsertBefore, &DT, ExprNotDom)) {
      SmallVector<Instruction *, 8> ExprClone;
      if (!cloneChain(ExprInst, ExprClone, &InsertBefore, &DT))
        return nullptr;
      for (auto I = ExprClone.rbegin(), EI = ExprClone.rend(); I != EI; ++I) {
        (*I)->insertBefore(cast<Instruction>(&InsertBefore));
        (*I)->setMetadata("sapfor.da", InstrMD);
      }
      if (!ExprClone.empty())
        Expr = ExprClone.front();
    } else {
      setMDForDeadInstructions(ExprInst);
      for (auto *Op : ExprNotDom) {
        SmallVector<Instruction *, 8> ExprClone;
        if (!cloneChain(cast<Instruction>(Op), ExprClone, &InsertBefore, &DT)) {
          deleteDeadInstructions(ExprInst);
          return nullptr;
        }
        for (auto I = ExprClone.rbegin(), EI = ExprClone.rend(); I != EI; ++I) {
          (*I)->insertBefore(cast<Instruction>(Op->getUser()));
          (*I)->setMetadata("sapfor.da", InstrMD);
        }
        Op->getUser()->setOperand(Op->getOperandNo(), ExprClone.front());
      }
    }
  }
  return Expr;
}

void Instrumentation::deleteDeadInstructions(Instruction *From) {
  if (!From->use_empty())
    return;
  for (unsigned OpIdx = 0, OpIdxE = From->getNumOperands();
       OpIdx != OpIdxE; ++OpIdx) {
    Value *OpV = From->getOperand(OpIdx);
    From->setOperand(OpIdx, nullptr);
    if (auto *I = dyn_cast<Instruction>(OpV))
      deleteDeadInstructions(I);
  }
  From->eraseFromParent();
}

void Instrumentation::setMDForDeadInstructions(llvm::Instruction *From) {
  if (!From->use_empty())
    return;
  From->setMetadata("sapfor.da", MDNode::get(From->getContext(), {}));
  for (auto &Op : From->operands())
    if (auto I = dyn_cast<Instruction>(&Op))
      setMDForSingleUseInstructions(I);
}

void Instrumentation::setMDForSingleUseInstructions(Instruction *From) {
  if (From->getNumUses() != 1)
    return;
  From->setMetadata("sapfor.da", MDNode::get(From->getContext(), {}));
  for (auto &Op : From->operands())
    if (auto I = dyn_cast<Instruction>(&Op))
      setMDForSingleUseInstructions(I);
}

void Instrumentation::loopBeginInstr(Loop *L, DIStringRegister::IdTy DILoopIdx,
    ScalarEvolution &SE, DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS) {
  auto *Header = L->getHeader();
  auto InstrMD = MDNode::get(Header->getContext(), {});
  Instruction *InsertBefore = nullptr;
  Value *Start = nullptr, *End = nullptr, *Step = nullptr;
  bool Signed = false;
  auto *Int64Ty = Type::getInt64Ty(Header->getContext());
  if (auto Preheader = L->getLoopPreheader()) {
    InsertBefore = Preheader->getTerminator();
    std::tie(Start, End, Step, Signed) =
      computeLoopBounds(*L, *Int64Ty, SE, DT, RI, CS);
  } else {
    auto *NewBB = BasicBlock::Create(Header->getContext(),
      "preheader", Header->getParent(), Header);
    InsertBefore = BranchInst::Create(Header, NewBB);
    InsertBefore->setMetadata("sapfor.da", InstrMD);
    for (auto *PredBB : predecessors(Header)) {
      if (L->contains(PredBB))
        continue;
      auto *PredBranch = PredBB->getTerminator();
      for (unsigned SuccIdx = 0, SuccIdxE = PredBranch->getNumSuccessors();
           SuccIdx < SuccIdxE; ++SuccIdx)
        if (PredBranch->getSuccessor(SuccIdx) ==  Header)
          PredBranch->setSuccessor(SuccIdx, NewBB);
    }
  }
  auto DbgLoc = L->getLocRange();
  std::string StartLoc = DbgLoc.getStart() ?
    ("line1=" + Twine(DbgLoc.getStart().getLine()) + "*" +
      "col1=" + Twine(DbgLoc.getStart().getCol()) + "*").str() :
    std::string("");
  std::string EndLoc = DbgLoc.getEnd() ?
    ("line1=" + Twine(DbgLoc.getEnd().getLine()) + "*" +
      "col1=" + Twine(DbgLoc.getEnd().getCol()) + "*").str() :
    std::string("");
  LoopBoundKind BoundFlag = LoopBoundIsUnknown;
  BoundFlag |= Start ? LoopStartIsKnown : LoopBoundIsUnknown;
  BoundFlag |= End ? LoopEndIsKnown : LoopBoundIsUnknown;
  BoundFlag |= Step ? LoopStepIsKnown : LoopBoundIsUnknown;
  BoundFlag |= !Signed ? LoopBoundUnsigned : LoopBoundIsUnknown;
  createInitDICall(
    Twine("type=") + "seqloop" + "*" +
    "file=" + Header->getModule()->getSourceFileName() + "*" +
    "bounds=" + Twine(BoundFlag) + "*" +
    StartLoc + EndLoc + "*", DILoopIdx);
  auto *DILoop = createPointerToDI(DILoopIdx, *InsertBefore);
  Start = Start ? Start : ConstantInt::get(Int64Ty, 0);
  End = End ? End : ConstantInt::get(Int64Ty, 0);
  Step = Step ? Step : ConstantInt::get(Int64Ty, 0);
  auto Fun = getDeclaration(Header->getModule(), IntrinsicId::sl_begin);
  auto Call = CallInst::Create(
    Fun, {DILoop, Start, End, Step}, "", InsertBefore);
  Call->setMetadata("sapfor.da", InstrMD);
}

void Instrumentation::loopEndInstr(Loop *L, DIStringRegister::IdTy DILoopIdx) {
  assert(L && "Loop must not be null!");
  auto *Header = L->getHeader();
  auto InstrMD = MDNode::get(Header->getContext(), {});
  for (auto *BB : L->blocks())
    for (auto *SuccBB : successors(BB)) {
      if (L->contains(SuccBB))
        continue;
      auto *ExitBB = BasicBlock::Create(Header->getContext(),
        SuccBB->getName(), Header->getParent(), SuccBB);
      auto *InsertBefore = BranchInst::Create(SuccBB, ExitBB);
      InsertBefore->setMetadata("sapfor.da", InstrMD);
      auto *ExitingBranch = BB->getTerminator();
      for (unsigned SuccIdx = 0, SuccIdxE = ExitingBranch->getNumSuccessors();
           SuccIdx < SuccIdxE; ++SuccIdx) {
        if (ExitingBranch->getSuccessor(SuccIdx) == SuccBB)
          ExitingBranch->setSuccessor(SuccIdx, ExitBB);
      }
      auto DILoop = createPointerToDI(DILoopIdx, *InsertBefore);
      auto Fun = getDeclaration(Header->getModule(), IntrinsicId::sl_end);
      auto Call = CallInst::Create(Fun, {DILoop}, "", InsertBefore);
      Call->setMetadata("sapfor.da", InstrMD);
    }
}

void Instrumentation::loopIterInstr(Loop *L, DIStringRegister::IdTy DILoopIdx) {
  assert(L && "Loop must not be null!");
  auto *Header = L->getHeader();
  auto InstrMD = MDNode::get(Header->getContext(), {});
  auto &InsertBefore = *Header->getFirstInsertionPt();
  auto *Int64Ty = Type::getInt64Ty(Header->getContext());
  auto *CountPHI = PHINode::Create(Int64Ty, 0, "loop.count", &Header->front());
  CountPHI->setMetadata("sapfor.da", InstrMD);
  auto *Int1 = ConstantInt::get(Int64Ty, 1);
  assert(L->getLoopPreheader() &&
    "Preheader must be already created if it did not exist!");
  CountPHI->addIncoming(Int1, L->getLoopPreheader());
  auto *Inc = BinaryOperator::CreateNUW(BinaryOperator::Add, CountPHI,
    ConstantInt::get(Int64Ty, 1), "inc", &InsertBefore);
  Inc->setMetadata("sapfor.da", InstrMD);
  SmallVector<BasicBlock *, 4> Latches;
  L->getLoopLatches(Latches);
  for (auto *Latch : Latches)
    CountPHI->addIncoming(Inc, Latch);
  auto *DILoop = createPointerToDI(DILoopIdx, *Inc);
  auto Fun = getDeclaration(Header->getModule(), IntrinsicId::sl_iter);
  auto *Call = CallInst::Create(Fun, {DILoop, CountPHI}, "", Inc);
  Call->setMetadata("sapfor.da", InstrMD);
}

void Instrumentation::regLoops(llvm::Function &F, llvm::LoopInfo &LI,
    llvm::ScalarEvolution &SE, llvm::DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS) {
  for_each(LI, [this, &SE, &DT, &RI, &CS](Loop *L) {
    DEBUG(dbgs() << "[INSTR]: process loop "; L->print(dbgs()); dbgs() << "\n");
    auto Idx = mDIStrings.regItem(L);
    loopBeginInstr(L, Idx, SE, DT, RI, CS);
    loopEndInstr(L, Idx);
    loopIterInstr(L, Idx);
  });
}

void Instrumentation::visit(Function &F) {
  // Some functions have not been marked with "sapfor.da" yet. For example,
  // functions which have been created after registration of all functions.
  // So, we set this property here.
  IntrinsicId InstrLibId;
  if (getTsarLibFunc(F.getName(), InstrLibId)) {
    F.setMetadata("sapfor.da", MDNode::get(F.getContext(), {}));
    return;
  }
  if (F.empty() || F.getMetadata("sapfor.da"))
    return;
  visitFunction(F);
  visit(F.begin(), F.end());
}

void Instrumentation::regFunction(Value &F, Type *ReturnTy, unsigned Rank,
  DISubprogram *MD, DIStringRegister::IdTy Idx, Module &M) {
  DEBUG(dbgs() << "[INSTR]: register function ";
    F.printAsOperand(dbgs()); dbgs() << "\n");
  std::string DeclStr = !MD ? F.getName().empty() ? std::string("") :
    (Twine("name1=") + F.getName() + "*").str() :
    ("line1=" + Twine(MD->getLine()) + "*" +
      "name1=" + MD->getName() + "*").str();
  auto ReturnTypeId = mTypes.regItem(ReturnTy);
  createInitDICall(Twine("type=") + "function" + "*" +
    "file=" + M.getSourceFileName() + "*" +
    "vtype=" + Twine(ReturnTypeId) + "*" +
    "rank=" + Twine(Rank) + "*" +
    DeclStr + "*", Idx);
}

void Instrumentation::visitFunction(llvm::Function &F) {
  // Change linkage for inline functions, to avoid merge of a function which
  // should not be instrumented with this function. For example, call of
  // a function which has been instrumented from dynamic analyzer may produce
  // infinite loop. The other example, is call of some system functions before
  // call of main (sprintf... in case of Microsoft implementation of STD). In
  // this case pool of metadata is not allocated yet.
  if(F.getLinkage() == Function::LinkOnceAnyLinkage ||
     F.getLinkage() == Function::LinkOnceODRLinkage)
    F.setLinkage(Function::InternalLinkage);
  auto *M = F.getParent();
  auto *MD = F.getSubprogram();
  auto Idx = mDIStrings[&F];
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto &FirstInst = *inst_begin(F);
  auto DIFunc = createPointerToDI(Idx, FirstInst);
  auto Call = CallInst::Create(Fun, {DIFunc}, "", &FirstInst);
  Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
  regArgs(F, DIFunc);
  auto &Provider = mInstrPass->getAnalysis<InstrumentationPassProvider>(F);
  auto &LoopInfo = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  auto &RegionInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
  auto &CanonicalLoop = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &SE = Provider.get<ScalarEvolutionWrapperPass>().getSE();
  auto &DT = Provider.get<DominatorTreeWrapperPass>().getDomTree();
  regLoops(F, LoopInfo, SE, DT, RegionInfo, CanonicalLoop);
}

void Instrumentation::regArgs(Function &F, LoadInst *DIFunc) {
  auto InstrMD = MDNode::get(F.getContext(), {});
  auto *BytePtrTy = Type::getInt8PtrTy(F.getContext());
  auto *SizeTy = Type::getInt64Ty(F.getContext());
  for (auto &Arg : F.args()) {
    if (Arg.getNumUses() != 1)
      continue;
    auto *U = dyn_cast<StoreInst>(*Arg.user_begin());
    if (!U)
      continue;
    auto *Alloca = dyn_cast<AllocaInst>(U->getPointerOperand());
    if (!Alloca)
      continue;
    auto AllocaMD = getMetadata(Alloca);
    if (!AllocaMD || !AllocaMD->isParameter())
      continue;
    DEBUG(dbgs() << "[INSTR]: register "; Alloca->print(dbgs());
      dbgs() << " as argument "; Arg.print(dbgs());
      dbgs() << " with no " << Arg.getArgNo() << "\n");
    auto AllocaAddr =
      new BitCastInst(Alloca, BytePtrTy, Alloca->getName() + ".addr", U);
    AllocaAddr->setMetadata("sapfor.da", InstrMD);
    auto Pos = ConstantInt::get(SizeTy, Arg.getArgNo());
    unsigned Rank;
    uint64_t ArraySize;
    std::tie(Rank, ArraySize) = arraySize(Alloca->getAllocatedType());
    CallInst *Call = nullptr;
    if (Rank != 0) {
      auto *Size = ConstantInt::get(SizeTy, ArraySize);
      auto *Fun = getDeclaration(F.getParent(), IntrinsicId::reg_dummy_arr);
      Call = CallInst::Create(Fun, { DIFunc, Size, AllocaAddr, Pos }, "");
    } else {
      auto *Fun = getDeclaration(F.getParent(), IntrinsicId::reg_dummy_var);
      Call = CallInst::Create(Fun, { DIFunc, AllocaAddr, Pos }, "");
    }
    Call->insertBefore(U);
    Call->setMetadata("sapfor.da", InstrMD);
  }
}

void Instrumentation::visitCallSite(llvm::CallSite CS) {
  /// TODO (kaniandr@gmail.com): may be some other intrinsics also should be
  /// ignored, see llvm::AliasSetTracker::addUnknown() for details.
  switch (CS.getIntrinsicID()) {
  case llvm::Intrinsic::dbg_declare: case llvm::Intrinsic::dbg_value:
  case llvm::Intrinsic::assume:
    return;
  }
  DIStringRegister::IdTy FuncIdx = 0;
  if (auto *Callee = llvm::dyn_cast<llvm::Function>(
        CS.getCalledValue()->stripPointerCasts())) {
    IntrinsicId LibId;
    // Do not check for 'sapfor.da' metadata only because it may not be set
    // for some functions of dynamic analyzer yet. However, it is necessary to
    // check for 'sapfor.da' to ignore some internal utility functions which
    // have been created.
    if(Callee->getMetadata("sapfor.da") ||
       getTsarLibFunc(Callee->getName(), LibId))
      return;
    FuncIdx = mDIStrings[Callee];
  } else {
    FuncIdx = mDIStrings.regItem(CS.getCalledValue());
  }
  auto *Inst = CS.getInstruction();
  DEBUG(dbgs() << "[INSTR]: process "; Inst->print(dbgs()); dbgs() << "\n");
  auto DbgLocIdx = regDebugLoc(Inst->getDebugLoc());
  auto DILoc = createPointerToDI(DbgLocIdx, *Inst);
  auto DIFunc = createPointerToDI(FuncIdx, *Inst);
  auto *M = Inst->getModule();
  auto Fun = getDeclaration(M, tsar::IntrinsicId::func_call_begin);
  auto CallBegin = llvm::CallInst::Create(Fun, {DILoc, DIFunc}, "", Inst);
  auto InstrMD = MDNode::get(M->getContext(), {});
  CallBegin->setMetadata("sapfor.da", InstrMD);
  Fun = getDeclaration(M, tsar::IntrinsicId::func_call_end);
  auto CallEnd = llvm::CallInst::Create(Fun, {DIFunc}, "");
  CallEnd->insertAfter(Inst);
  CallBegin->setMetadata("sapfor.da", InstrMD);
}

std::tuple<Value *, Value *, Value *, Value *>
Instrumentation::regMemoryAccessArgs(Value *Ptr, const DebugLoc &DbgLoc,
    Instruction &InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto BasePtr = Ptr->stripInBoundsOffsets();
  DIStringRegister::IdTy OpIdx = 0;
  if (auto AI = dyn_cast<AllocaInst>(BasePtr)) {
    OpIdx = mDIStrings[AI];
  } else if (auto GV = dyn_cast<GlobalVariable>(BasePtr)) {
    OpIdx = mDIStrings[GV];
  } else {
    OpIdx = mDIStrings.regItem(BasePtr);
    regValue(BasePtr, BasePtr->getType(), nullptr, OpIdx, InsertBefore,
      *InsertBefore.getModule());
  }
  auto DbgLocIdx = regDebugLoc(DbgLoc);
  auto DILoc = createPointerToDI(DbgLocIdx, InsertBefore);
  auto Addr = new BitCastInst(Ptr,
    Type::getInt8PtrTy(Ctx), "addr", &InsertBefore);
  auto *MD = MDNode::get(Ctx, {});
  Addr->setMetadata("sapfor.da", MD);
  auto DIVar = createPointerToDI(OpIdx, *DILoc);
  auto BasePtrTy = cast_or_null<PointerType>(BasePtr->getType());
  llvm::Instruction *ArrayBase =
    (BasePtrTy && isa<ArrayType>(BasePtrTy->getElementType())) ?
      new BitCastInst(BasePtr, Type::getInt8PtrTy(Ctx),
        BasePtr->getName() + ".arraybase", &InsertBefore) : nullptr;
  if (ArrayBase)
   ArrayBase->setMetadata("sapfor.da", MD);
  return std::make_tuple(DILoc, Addr, DIVar, ArrayBase);
}

void Instrumentation::visitLoadInst(LoadInst &I) {
  if (I.getMetadata("sapfor.da"))
    return;
  DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto *M = I.getModule();
  llvm::Value *DILoc, *Addr, *DIVar, *ArrayBase;
  std::tie(DILoc, Addr, DIVar, ArrayBase) =
    regMemoryAccessArgs(I.getPointerOperand(), I.getDebugLoc(), I);
  if (ArrayBase) {
    auto *Fun = getDeclaration(M, IntrinsicId::read_arr);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar, ArrayBase}, "", &I);
    Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
  } else {
    auto *Fun = getDeclaration(M, IntrinsicId::read_var);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar}, "", &I);
    Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
  }
}

void Instrumentation::visitStoreInst(llvm::StoreInst &I) {
  if (I.getMetadata("sapfor.da"))
    return;
  DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  BasicBlock::iterator InsertBefore(I);
  ++InsertBefore;
  auto *M = I.getModule();
  llvm::Value *DILoc, *Addr, *DIVar, *ArrayBase;
  std::tie(DILoc, Addr, DIVar, ArrayBase) =
    regMemoryAccessArgs(I.getPointerOperand(), I.getDebugLoc(), *InsertBefore);
  if (!Addr)
    return;
  if (ArrayBase) {
    auto *Fun = getDeclaration(M, IntrinsicId::write_arr_end);
    auto Call = CallInst::Create(Fun, { DILoc, Addr, DIVar, ArrayBase }, "");
    Call->insertBefore(&*InsertBefore);
    Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
  } else {
    auto *Fun = getDeclaration(M, IntrinsicId::write_var_end);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar}, "", &*InsertBefore);
    Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
  }
}

void Instrumentation::regTypes(Module& M) {
  if (mTypes.numberOfIDs() == 0)
    return;
  auto &Ctx = M.getContext();
  // Get all registered types and fill std::vector<llvm::Constant*>
  // with local indexes and sizes of these types.
  auto &Types = mTypes.getRegister<llvm::Type *>();
  auto *Int64Ty = Type::getInt64Ty(Ctx);
  auto *Int0 = ConstantInt::get(Int64Ty, 0);
  std::vector<Constant* > Ids, Sizes;
  auto &DL = M.getDataLayout();
  for(auto &Pair: Types) {
    auto *TypeId = Constant::getIntegerValue(Int64Ty,
      APInt(64, Pair.get<TypeRegister::IdTy>()));
    Ids.push_back(TypeId);
    auto *TypeSize = Pair.get<Type *>()->isSized() ?
      Constant::getIntegerValue(Int64Ty,
        APInt(64, DL.getTypeSizeInBits(Pair.get<Type *>()))) : Int0;
    Sizes.push_back(TypeSize);
  }
  // Create global values for IDs and sizes. initialize them with local values.
  auto ArrayTy = ArrayType::get(Int64Ty, Types.size());
  auto IdsArray = new GlobalVariable(M, ArrayTy, false,
    GlobalValue::LinkageTypes::InternalLinkage,
    ConstantArray::get(ArrayTy, Ids), "sapfor.type.ids", nullptr);
  IdsArray->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto SizesArray = new GlobalVariable(M, ArrayTy, false,
    GlobalValue::LinkageTypes::InternalLinkage,
    ConstantArray::get(ArrayTy, Sizes), "sapfor.type.sizes", nullptr);
  SizesArray->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  // Create function to update local indexes of types.
  auto FuncType =
    FunctionType::get(Type::getInt64Ty(Ctx), { Int64Ty }, false);
  auto RegTypeFunc = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.register.type", &M);
  RegTypeFunc->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto EntryBB = BasicBlock::Create(Ctx, "entry", RegTypeFunc);
  auto *StartId = &*RegTypeFunc->arg_begin();
  StartId->setName("startid");
  // Create loop to update indexes: NewTypeId = StartId + LocalTypId;
  auto *LoopBB = BasicBlock::Create(Ctx, "loop", RegTypeFunc);
  BranchInst::Create(LoopBB, EntryBB);
  auto *Counter = PHINode::Create(Int64Ty, 0, "typeidx", LoopBB);
  Counter->addIncoming(Int0, EntryBB);
  auto *GEP = GetElementPtrInst::Create(
    nullptr, IdsArray, { Int0, Counter }, "arrayidx", LoopBB);
  auto *LocalTypeId = new LoadInst(GEP, "typeid", false, 0, LoopBB);
  auto Add = BinaryOperator::CreateNUW(
    BinaryOperator::Add, LocalTypeId, StartId, "add", LoopBB);
  new StoreInst(Add, GEP, false, 0, LoopBB);
  auto Inc = BinaryOperator::CreateNUW(BinaryOperator::Add, Counter,
    ConstantInt::get(Int64Ty, 1), "inc", LoopBB);
  Counter->addIncoming(Inc, LoopBB);
  auto *Size = ConstantInt::get(Int64Ty, Types.size());
  auto *Cmp = new ICmpInst(*LoopBB, CmpInst::ICMP_ULT, Inc, Size, "cmp");
  auto *EndBB = BasicBlock::Create(M.getContext(), "end", RegTypeFunc);
  BranchInst::Create(LoopBB, EndBB, Cmp, LoopBB);
  // Return number of registered types.
  ReturnInst::Create(Ctx, Size, EndBB);
}

void Instrumentation::createInitDICall(const llvm::Twine &Str,
    DIStringRegister::IdTy Idx) {
  assert(mDIPool && "Pool of metadata strings must not be null!");
  assert(mInitDIAll &&
    "Metadata strings initialization function must not be null!");
  auto &BB = mInitDIAll->getEntryBlock();
  auto *T = BB.getTerminator();
  assert(T && "Terminator must not be null!");
  auto *M = mInitDIAll->getParent();
  auto InitDIFunc = getDeclaration(M, IntrinsicId::init_di);
  auto IdxV = ConstantInt::get(Type::getInt64Ty(M->getContext()), Idx);
  auto DIPoolPtr = new LoadInst(mDIPool, "dipool", T);
  auto GEP =
    GetElementPtrInst::Create(nullptr, DIPoolPtr, { IdxV }, "arrayidx", T);
  SmallString<256> SingleStr;
  auto DIString = createDIStringPtr(Str.toStringRef(SingleStr), *T);
  auto Offset = &*mInitDIAll->arg_begin();
  CallInst::Create(InitDIFunc, {GEP, DIString, Offset}, "", T);
}

GetElementPtrInst* Instrumentation::createDIStringPtr(
    StringRef Str, Instruction &InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto &M = *InsertBefore.getModule();
  auto Data = llvm::ConstantDataArray::getString(Ctx, Str);
  auto Var = new llvm::GlobalVariable(
    M, Data->getType(), true, GlobalValue::InternalLinkage, Data);
  Var->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto Int0 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0);
  return GetElementPtrInst::CreateInBounds(
    Var, { Int0,Int0 }, "distring", &InsertBefore);
}

LoadInst* Instrumentation::createPointerToDI(
    DIStringRegister::IdTy Idx, Instruction& InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto *MD = MDNode::get(Ctx, {});
  auto IdxV = ConstantInt::get(Type::getInt64Ty(Ctx), Idx);
  auto DIPoolPtr = new LoadInst(mDIPool, "dipool", &InsertBefore);
  DIPoolPtr->setMetadata("sapfor.da", MD);
  auto GEP = GetElementPtrInst::Create(nullptr, DIPoolPtr, {IdxV}, "arrayidx");
  GEP->setMetadata("sapfor.da", MD);
  GEP->insertAfter(DIPoolPtr);
  GEP->setIsInBounds(true);
  auto DI = new LoadInst(GEP, "di");
  DI->setMetadata("sapfor.da", MD);
  DI->insertAfter(GEP);
  return DI;
}

auto Instrumentation::regDebugLoc(
    const DebugLoc &DbgLoc) -> DIStringRegister::IdTy {
  assert(mDIPool && "Pool of metadata strings must not be null!");
  assert(mInitDIAll &&
    "Metadata strings initialization function must not be null!");
  // We use reserved index if source location is unknown.
  if (!DbgLoc)
    return DIStringRegister::indexOfItemType<DILocation *>();
  auto DbgLocIdx = mDIStrings.regItem(DbgLoc.get());
  createInitDICall(
    Twine("type=") + "file_name" + "*" +
    "line1=" + Twine(DbgLoc.getLine()) + "*" +
    "col1=" + Twine(DbgLoc.getCol()) + "*" + "*", DbgLocIdx);
  return DbgLocIdx;
}

void Instrumentation::regValue(Value *V, Type *T, DIVariable *MD,
    DIStringRegister::IdTy Idx,  Instruction &InsertBefore, Module &M) {
  assert(V && "Variable must not be null!");
  DEBUG(dbgs() << "[INSTR]: register variable ";
    V->printAsOperand(dbgs()); dbgs() << "\n");
  auto DeclStr = MD ? (Twine("line1=") + Twine(MD->getLine()) + "*" +
    "name1=" + MD->getName() + "*").str() : std::string("");
  unsigned TypeId = mTypes.regItem(T);
  unsigned Rank;
  uint64_t ArraySize;
  std::tie(Rank, ArraySize) = arraySize(T);
  auto TypeStr = Rank == 0 ? (Twine("var_name") + "*").str() :
    (Twine("arr_name") + "*" + "rank=" + Twine(Rank) + "*").str();
  createInitDICall(
    Twine("type=") + TypeStr +
    "file=" + M.getSourceFileName() + "*" +
    "vtype=" + Twine(TypeId) + "*" + DeclStr + "*",
    Idx);
  auto DIVar = createPointerToDI(Idx, InsertBefore);
  auto VarAddr = new BitCastInst(V,
    Type::getInt8PtrTy(M.getContext()), V->getName() + ".addr", &InsertBefore);
  VarAddr->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  CallInst *Call = nullptr;
  if (Rank != 0) {
    auto Size = ConstantInt::get(Type::getInt64Ty(M.getContext()), ArraySize);
    auto Fun = getDeclaration(&M, IntrinsicId::reg_arr);
    Call = CallInst::Create(Fun, { DIVar, Size, VarAddr }, "", &InsertBefore);
  } else {
    auto Fun = getDeclaration(&M, IntrinsicId::reg_var);
   Call = CallInst::Create(Fun, { DIVar, VarAddr }, "", &InsertBefore);
  }
  Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
}

void Instrumentation::regFunctions(Module& M) {
  for (auto &F : M) {
    IntrinsicId LibId;
    if (getTsarLibFunc(F.getName(), LibId)) {
      F.setMetadata("sapfor.da", MDNode::get(F.getContext(), {}));
      continue;
    }
    if (F.getMetadata("sapfor.da"))
      continue;
    /// TODO (kaniandr@gmail.com): may be some other intrinsics also should be
    /// ignored, see llvm::AliasSetTracker::addUnknown() for details.
    switch (F.getIntrinsicID()) {
    case llvm::Intrinsic::dbg_declare: case llvm::Intrinsic::dbg_value:
    case llvm::Intrinsic::assume:
      continue;
    }
    auto Idx = mDIStrings.regItem(&F);
    regFunction(F, F.getReturnType(), F.getFunctionType()->getNumParams(),
      F.getSubprogram(), Idx, M);
  }
}

void Instrumentation::regGlobals(Module& M) {
  auto &Ctx = M.getContext();
  auto FuncType = FunctionType::get(Type::getVoidTy(Ctx), false);
  auto RegGlobalFunc = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.register.global", &M);
  RegGlobalFunc->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto *EntryBB = BasicBlock::Create(Ctx, "entry", RegGlobalFunc);
  auto *RetInst = ReturnInst::Create(mInitDIAll->getContext(), EntryBB);
  DIStringRegister::IdTy RegisteredGLobals = 0;
  for (auto I = M.global_begin(), EI = M.global_end(); I != EI; ++I) {
    if (I->getMetadata("sapfor.da"))
      continue;
    ++RegisteredGLobals;
    auto Idx = mDIStrings.regItem(&(*I));
    auto *MD = getMetadata(&*I);
    regValue(&*I, I->getValueType(), MD, Idx, *RetInst, M);
  }
  if (RegisteredGLobals == 0)
    RegGlobalFunc->eraseFromParent();
}

void Instrumentation::instrumentateMain(Module& M) {
  auto MainFunc = M.getFunction("main");
  if(!MainFunc || !mInitDIAll)
    return;
  auto &BB = MainFunc->getEntryBlock();
  auto Int0 = llvm::ConstantInt::get(Type::getInt64Ty(M.getContext()), 0);
  if (auto *RegTypeFunc = M.getFunction("sapfor.register.type")) {
    auto Call = CallInst::Create(RegTypeFunc, { Int0 }, "", &BB.front());
    Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  }
  if (auto *RegGlobalFunc = M.getFunction("sapfor.register.global")) {
    auto Call = CallInst::Create(RegGlobalFunc, "", &BB.front());
    Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  }
  auto Call = CallInst::Create(mInitDIAll, {Int0}, "", &BB.front());
  Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
}
