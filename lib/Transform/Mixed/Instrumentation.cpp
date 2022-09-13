//===- Istrumentation.cpp -- LLVM IR Instrumentation Engine -----*- C++ -*-===//
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
// This file implements methods to perform IR-level instrumentation.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Mixed/Instrumentation.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Intrinsics.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/PassProvider.h"
#include "tsar/Transform/IR/MetadataUtils.h"
#include "tsar/Transform/IR/Utils.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>
#include <vector>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "instr-llvm"

using InstrumentationPassProvider = FunctionPassProvider<
  TransformationEnginePass,
  DFRegionInfoPass,
  LoopInfoWrapperPass,
  CanonicalLoopPass,
  MemoryMatcherImmutableWrapper,
  ScalarEvolutionWrapperPass,
  DominatorTreeWrapperPass>;

STATISTIC(NumFunction, "Number of functions");
STATISTIC(NumFunctionVisited, "Number of processed functions");
STATISTIC(NumLoop, "Number of processed loops");
STATISTIC(NumType, "Number of registered types");
STATISTIC(NumVariable, "Number of registered variables");
STATISTIC(NumScalar, "Number of registered scalar variables");
STATISTIC(NumArray, "Number of registered arrays");
STATISTIC(NumCall, "Number of registered calls");
STATISTIC(NumMemoryAccesses, "Number of registered memory accesses");
STATISTIC(NumLoad, "Number of registered loads from the memory");
STATISTIC(NumLoadScalar, "Number of registered loads from scalars");
STATISTIC(NumLoadArray, "Number of registered loads from arrays");
STATISTIC(NumStore, "Number of registered stores to the memory");
STATISTIC(NumStoreScalar, "Number of registered stores to scalars");
STATISTIC(NumStoreArray, "Number of registered stores to arrays");

INITIALIZE_PROVIDER_BEGIN(InstrumentationPassProvider, "instr-llvm-provider",
  "Instrumentation Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(InstrumentationPassProvider, "instr-llvm-provider",
  "Instrumentation Provider")

char InstrumentationPass::ID = 0;
INITIALIZE_PASS_BEGIN(InstrumentationPass, "instr-llvm",
  "LLVM IR Instrumentation", false, false)
INITIALIZE_PASS_DEPENDENCY(InstrumentationPassProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_END(InstrumentationPass, "instr-llvm",
  "LLVM IR Instrumentation", false, false)

bool InstrumentationPass::runOnModule(Module &M) {
  releaseMemory();
  auto &TfmInfo = getAnalysis<TransformationEnginePass>().get();
  InstrumentationPassProvider::initialize<TransformationEnginePass>(
    [&TfmInfo](TransformationEnginePass &TEP) {
      TEP.set(TfmInfo);
  });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  InstrumentationPassProvider::initialize<MemoryMatcherImmutableWrapper>(
    [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
      Wrapper.set(*MMWrapper);
  });
  if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
    InstrumentationPassProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  Instrumentation::visit(M, *this);
  Function *EntryPoint = nullptr;
  if (!mInstrEntry.empty())
    EntryPoint = M.getFunction(mInstrEntry);
  else if (!(EntryPoint = M.getFunction("main")))
    EntryPoint = M.getFunction("MAIN_");
  if (EntryPoint)
    visitEntryPoint(*EntryPoint, { &M });
  else
    M.getContext().diagnose(DiagnosticInfoInlineAsm("entry point is not found"));
  return true;
}

void InstrumentationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<InstrumentationPassProvider>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
}

ModulePass * llvm::createInstrumentationPass(
    StringRef InstrEntry, ArrayRef<std::string> StartFrom) {
  return new InstrumentationPass(InstrEntry, StartFrom);
}

Function * tsar::createEmptyInitDI(Module &M, Type &IdTy) {
  auto &Ctx = M.getContext();
  auto FuncType = FunctionType::get(Type::getVoidTy(Ctx), { &IdTy }, false);
  auto Func = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.init.di", &M);
  addNameDAMetadata(*Func, "sapfor.da", "sapfor.init.di");
  Func->arg_begin()->setName("startid");
  auto EntryBB = BasicBlock::Create(Ctx, "entry", Func);
  ReturnInst::Create(Ctx, EntryBB);
  return Func;
}

std::pair<GlobalVariable *, Type *> tsar::getOrCreateDIPool(Module &M) {
  auto *DIPoolTy = PointerType::getUnqual(Type::getInt8PtrTy(M.getContext()));
  if (auto *DIPool = M.getNamedValue("sapfor.di.pool")) {
    if (isa<GlobalVariable>(DIPool) && DIPool->getValueType() == DIPoolTy &&
        cast<GlobalVariable>(DIPool)->getMetadata("sapfor.da"))
      return std::pair{cast<GlobalVariable>(DIPool),
                       cast<Type>(Type::getInt8PtrTy(M.getContext()))};
    return {nullptr, nullptr};
  }
  auto DIPool = new GlobalVariable(M, DIPoolTy, false,
    GlobalValue::LinkageTypes::ExternalLinkage,
    ConstantPointerNull::get(DIPoolTy), "sapfor.di.pool", nullptr);
  assert(DIPool->getName() == "sapfor.di.pool" &&
    "Unable to crate a metadata pool!");
  DIPool->setAlignment(MaybeAlign(4));
  DIPool->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  return std::pair{DIPool, cast<Type>(Type::getInt8PtrTy(M.getContext()))};
}

Type * tsar::getInstrIdType(LLVMContext &Ctx) {
  auto InitDIFuncTy = getType(Ctx, IntrinsicId::init_di);
  assert(InitDIFuncTy->getNumParams() > 2 &&
    "Intrinsic 'init_di' must has at least 3 arguments!");
  return InitDIFuncTy->getParamType(2);
}

static void collectCallee(CallGraphNode &CGN,
    DenseSet<Function *> &TransitiveCallees) {
  for (auto &Callee : CGN) {
    if (Callee.second->getFunction()) {
      if (TransitiveCallees.insert(Callee.second->getFunction()).second)
        collectCallee(*Callee.second, TransitiveCallees);
    } else if (Callee.first) {
      auto *Call = dyn_cast<CallBase>(*Callee.first);
      if (!Call)
        continue;
      auto F =
        dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
      if (!F)
        continue;
      if (TransitiveCallees.insert(F).second)
        collectCallee(*Callee.second, TransitiveCallees);
    }
  }
}

void Instrumentation::excludeFunctions(Module &M) {
  if (mInstrPass->getStartFrom().empty())
    return;
  auto &CG = mInstrPass->getAnalysis<CallGraphWrapperPass>().getCallGraph();
  DenseSet<Function *> TransitiveCallees;
  for (auto &Name : mInstrPass->getStartFrom()) {
    auto F = M.getFunction(Name);
    if (!F) {
      M.getContext().diagnose(DiagnosticInfoInlineAsm(
        Twine("unknown function '") + Name + "'", DS_Warning));
      continue;
    }
    TransitiveCallees.insert(F);
    if (auto CGN = CG[F]) {
      collectCallee(*CGN, TransitiveCallees);
    }
  }
  for (auto &F : M) {
    if (!TransitiveCallees.count(&F))
      F.setMetadata("sapfor.da.ignore", MDNode::get(M.getContext(), {}));
  }
}

void Instrumentation::visitModule(Module &M, InstrumentationPass &IP) {
  mInstrPass = &IP;
  mDIStrings.clear(DIStringRegister::numberOfItemTypes());
  mTypes.clear();
  auto &Ctx = M.getContext();
  std::tie(mDIPool, mDIPoolElementTy) = getOrCreateDIPool(M);
  auto IdTy = getInstrIdType(Ctx);
  assert(IdTy && "Offset type must not be null!");
  mInitDIAll = createEmptyInitDI(M, *IdTy);
  reserveIncompleteDIStrings(M);
  excludeFunctions(M);
  regFunctions(M);
  regGlobals(M);
  visit(M.begin(), M.end());
  regTypes(M);
  auto Int64Ty = Type::getInt64Ty(M.getContext());
  auto PoolSize = ConstantInt::get(IdTy,
    APInt(Int64Ty->getBitWidth(), mDIStrings.numberOfIDs()));
  addNameDAMetadata(*mDIPool, "sapfor.da", "sapfor.di.pool",
    { ConstantAsMetadata::get(PoolSize) });
  NumVariable += NumScalar + NumArray;
  NumLoad += NumLoadScalar + NumLoadArray;
  NumStore += NumStore + NumStoreArray;
  NumMemoryAccesses += NumLoad + NumStore;
}

void Instrumentation::reserveIncompleteDIStrings(llvm::Module &M) {
  auto DbgLocIdx = DIStringRegister::indexOfItemType<DILocation *>();
  SmallString<128> Path;
  auto EC{sys::fs::real_path(M.getSourceFileName(), Path)};
  createInitDICall(
      Twine("type=") + "file_name" + "*" + "file=" +
          (EC ? StringRef{M.getSourceFileName()} : StringRef{Path}) + "*" + "*",
      DbgLocIdx);
}

void Instrumentation::visitAllocaInst(llvm::AllocaInst &I) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  SmallVector<DIMemoryLocation, 1> DILocs;
  auto DIM = findMetadata(&I, DILocs);
  auto Info = mDIStrings.regItem(&I);
  // An `alloca` may be registered as a dummy argument,
  // so avoid double registration.
  if (!Info.second)
    return;
  BasicBlock::iterator InsertBefore(I);
  ++InsertBefore;
  regValue(&I, I.getAllocatedType(), I.getArraySize(), DIM ? &*DIM : nullptr,
    Info.first, *InsertBefore, *I.getModule());
}

void Instrumentation::visitReturnInst(llvm::ReturnInst &I) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
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
  bool Signed = (*CanonItr)->isSigned();
  bool Unsigned = (*CanonItr)->isUnsigned();
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
  auto SLBeginFunc = getDeclaration(Header->getModule(), IntrinsicId::sl_begin);
  auto SLBeginFuncTy = SLBeginFunc.getFunctionType();
  assert(SLBeginFuncTy->getNumParams() > 3 && "Too few arguments!");
  auto *SizeTy = dyn_cast<IntegerType>(SLBeginFuncTy->getParamType(1));
  assert(SizeTy && "Bound expression must has an integer type!");
  assert(SLBeginFuncTy->getParamType(2) == SizeTy &&
    "Loop bound expressions have different types!");
  assert(SLBeginFuncTy->getParamType(3) == SizeTy &&
    "Loop bound expressions have different types!");
  if (auto Preheader = L->getLoopPreheader()) {
    InsertBefore = Preheader->getTerminator();
    std::tie(Start, End, Step, Signed) =
      computeLoopBounds(*L, *SizeTy, SE, DT, RI, CS);
  } else {
    auto *NewBB = BasicBlock::Create(Header->getContext(),
      "preheader", Header->getParent(), Header);
    const auto Predecessors = predecessors(Header);
    auto PredBB = Predecessors.begin();
    while (PredBB != Predecessors.end()) {
      if (L->contains(*PredBB)) {
        PredBB++;
        continue;
      }
      auto Pred = PredBB;
      PredBB++;
      auto *PredBranch = (*Pred)->getTerminator();
      for (unsigned SuccIdx = 0, SuccIdxE = PredBranch->getNumSuccessors();
           SuccIdx < SuccIdxE; ++SuccIdx)
        if (PredBranch->getSuccessor(SuccIdx) ==  Header)
          PredBranch->setSuccessor(SuccIdx, NewBB);
    }
    InsertBefore = BranchInst::Create(Header, NewBB);
    InsertBefore->setMetadata("sapfor.da", InstrMD);
    for(auto &Phi : Header->phis()) {
      unsigned IncomingIdx = 0;
      for (auto IncomingBB: Phi.blocks()) {
        for (auto PredBB : predecessors(NewBB)) {
          if (PredBB == IncomingBB) {
            Phi.setIncomingBlock(IncomingIdx, NewBB);
            break;
          }
        }
        IncomingIdx++;
      }
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
  auto MDFunc = Header->getParent()->getSubprogram();
  SmallString<128> PathToFile;
  if (MDFunc && MDFunc->getFile())
    getAbsolutePath(*MDFunc, PathToFile);
  else if (!sys::fs::real_path(Header->getModule()->getSourceFileName(),
                               PathToFile))
    PathToFile = Header->getModule()->getSourceFileName();
  createInitDICall(
    Twine("type=") + "seqloop" + "*" +
    "file=" + PathToFile + "*" +
    "bounds=" + Twine(BoundFlag) + "*" +
    StartLoc + EndLoc + "*", DILoopIdx);
  auto *DILoop = createPointerToDI(DILoopIdx, *InsertBefore);
  Start = Start ? Start : ConstantInt::get(SizeTy, 0);
  End = End ? End : ConstantInt::get(SizeTy, 0);
  Step = Step ? Step : ConstantInt::get(SizeTy, 0);
  auto Call = CallInst::Create(
    SLBeginFunc, {DILoop, Start, End, Step}, "", InsertBefore);
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
  for_each_loop(LI, [this, &SE, &DT, &RI, &CS, &F](Loop *L) {
    LLVM_DEBUG(dbgs()<<"[INSTR]: process loop " << L->getHeader()->getName() <<"\n");
    auto Idx = mDIStrings.regItem(LoopUnique(&F, L)).first;
    loopBeginInstr(L, Idx, SE, DT, RI, CS);
    loopEndInstr(L, Idx);
    loopIterInstr(L, Idx);
    ++NumLoop;
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
  if (F.getMetadata("sapfor.da") || F.getMetadata("sapfor.da.ignore"))
    return;
  ++NumFunction;
  if (F.empty())
    return;
  visitFunction(F);
  visit(F.begin(), F.end());
  mDT = nullptr;
}

void Instrumentation::regFunction(Value &F, Type *ReturnTy, unsigned Rank,
  DINode *MD, DIStringRegister::IdTy Idx, Module &M) {
  LLVM_DEBUG(dbgs() << "[INSTR]: register function ";
    F.printAsOperand(dbgs()); dbgs() << "\n");
  std::string DeclStr;
  SmallString<128> Filename{M.getSourceFileName()};
  if (!MD) {
    if (!F.getName().empty())
      DeclStr = (Twine("name1=") + F.getName() + "*").str();
    if (!sys::fs::real_path(M.getSourceFileName(), Filename))
      Filename = M.getSourceFileName();
  } else if (auto DI = dyn_cast<DISubprogram>(MD)) {
    DeclStr = ("line1=" + Twine(DI->getLine()) + "*" +
      "name1=" + DI->getName() + "*").str();
    if (DI->getFile())
      getAbsolutePath(*DI, Filename);
  } else if (auto DI = dyn_cast<DIVariable>(MD)) {
    DeclStr = ("line1=" + Twine(DI->getLine()) + "*" +
      "name1=" + DI->getName() + "*").str();
    if (DI->getFile())
      getAbsolutePath(*DI->getFile(), Filename);
  }
  auto ReturnTypeId = mTypes.regItem(ReturnTy).first;
  createInitDICall(Twine("type=") + "function" + "*" +
    "file=" + Filename + "*" +
    "vtype=" + Twine(ReturnTypeId) + "*" +
    "rank=" + Twine(Rank) + "*" +
    DeclStr + "*", Idx);
}

void Instrumentation::visitFunction(llvm::Function &F) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process function ";
    F.printAsOperand(dbgs()); dbgs() << "\n");
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
  ++NumFunctionVisited;
  Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
  regArgs(F, DIFunc);
  auto &Provider = mInstrPass->getAnalysis<InstrumentationPassProvider>(F);
  auto &LoopInfo = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  auto &RegionInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
  auto &CanonicalLoop = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &SE = Provider.get<ScalarEvolutionWrapperPass>().getSE();
  mDT = &Provider.get<DominatorTreeWrapperPass>().getDomTree();
  regLoops(F, LoopInfo, SE, *mDT, RegionInfo, CanonicalLoop);
}

void Instrumentation::regArgs(Function &F, LoadInst *DIFunc) {
  auto InstrMD = MDNode::get(F.getContext(), {});
  auto *BytePtrTy = Type::getInt8PtrTy(F.getContext());
  for (auto &Arg : F.args()) {
    LLVM_DEBUG(dbgs() << "[INSTR]: process argument " << Arg.getArgNo() << "\n");
    SmallVector<DIMemoryLocation, 1> DILocs;
    auto DIM = findMetadata(&Arg, DILocs);
    Value *ArgValue = &Arg;
    Type *ArgType = nullptr;
    Value *ArraySize = nullptr;
    DIStringRegister::IdTy Idx = 0;
    BasicBlock::iterator InsertBefore = BasicBlock::iterator(DIFunc);
    InsertBefore++;
    if (!DIM) {
      LLVM_DEBUG(dbgs() << "[INSTR]: search for 'alloca' which stores argument value\n");
      if (Arg.getNumUses() != 1)
        continue;
      auto *U = dyn_cast<StoreInst>(*Arg.user_begin());
      if (!U)
        continue;
      auto *AI = dyn_cast<AllocaInst>(U->getPointerOperand());
      if (!AI)
        continue;
      DIM = findMetadata(AI, DILocs);
      ArgValue = AI;
      ArgType = AI->getAllocatedType();
      ArraySize = AI->getArraySize();
      Idx = mDIStrings.regItem(AI).first;
      InsertBefore = BasicBlock::iterator(AI);
      ++InsertBefore;
    } else if (FindDbgAddrUses(&Arg).empty()) {
      // This argument has attached dbg.value intrinsics. This means that it
      // is a register which correspond to an argument, no a pointer to an
      // argument. So, we should not perform instrumentation for registers.
      continue;
    } else {
      ArgType = ArgValue->getType();
      if (ArgType->getTypeID() != Type::TypeID::PointerTyID)
        continue;
      ArraySize = ConstantInt::get(Type::getInt64Ty(F.getContext()), 1);
      Idx = mDIStrings.regItem(ArgValue).first;
    }
    assert(!DIM || DIM->isValid() && isa<DILocalVariable>(DIM->Var) &&
      "Invalid metadata!");
    if (!DIM || !cast<DILocalVariable>(DIM->Var)->isParameter())
      continue;
    LLVM_DEBUG(dbgs() << "[INSTR]: register "; ArgValue->print(dbgs());
      dbgs() << " as argument "; Arg.print(dbgs());
      dbgs() << " with no " << Arg.getArgNo() << "\n");
    auto &M = *F.getParent();
    SmallVector<Value *, 3> Args;
    auto SizeArgTy =
      getType(F.getContext(), IntrinsicId::reg_dummy_arr)->getParamType(1);
    regValueArgs(ArgValue, ArgType, ArraySize, SizeArgTy, &*DIM, Idx,
      *InsertBefore, M, Args);
    CallInst *Call = nullptr;
    if (Args.size() > 2) {
      auto Func = getDeclaration(&M, IntrinsicId::reg_dummy_arr);
      auto FuncTy = Func.getFunctionType();
      assert(FuncTy->getNumParams() > 4 && "Too few arguments!");
      auto Pos = ConstantInt::get(FuncTy->getParamType(4), Arg.getArgNo());
      Args.append({ DIFunc, Pos });
      Call =
        CallInst::Create(FuncTy, Func.getCallee(), Args, "", &*InsertBefore);
    } else {
      auto Func = getDeclaration(&M, IntrinsicId::reg_dummy_var);
      auto FuncTy = Func.getFunctionType();
      assert(FuncTy->getNumParams() > 3 && "Too few arguments!");
      auto Pos = ConstantInt::get(FuncTy->getParamType(3), Arg.getArgNo());
      Args.append({ DIFunc, Pos });
      Call =
        CallInst::Create(FuncTy, Func.getCallee(), Args, "", &*InsertBefore);
    }
    Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  }
}

void Instrumentation::visitCallBase(llvm::CallBase &Call) {
  if (isDbgInfoIntrinsic(Call.getIntrinsicID()) ||
      isMemoryMarkerIntrinsic(Call.getIntrinsicID()))
    return;
  DIStringRegister::IdTy FuncIdx = 0;
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; Call.print(dbgs()); dbgs() << "\n");
  auto *M = Call.getModule();
  if (auto *Callee = llvm::dyn_cast<llvm::Function>(
        Call.getCalledOperand()->stripPointerCasts())) {
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
    auto CalledValue = Call.getCalledOperand();
    auto Info = mDIStrings.regItem(CalledValue);
    FuncIdx = Info.first;
    if (Info.second) {
      auto FuncTy = Call.getFunctionType();
      assert(FuncTy && "Function type must not be null!");
      assert(mDT && "Dominator tree must not be null!");
      auto DIM = buildDIMemory(MemoryLocation::getAfter(CalledValue),
        M->getContext(), M->getDataLayout(), *mDT);
      regFunction(*CalledValue, FuncTy->getReturnType(), FuncTy->getNumParams(),
        DIM ? DIM->Var : nullptr, FuncIdx, *M);
    }
  }
  auto DbgLocIdx = regDebugLoc(Call.getDebugLoc());
  auto DILoc = createPointerToDI(DbgLocIdx, Call);
  auto DIFunc = createPointerToDI(FuncIdx, Call);
  auto Fun = getDeclaration(M, tsar::IntrinsicId::func_call_begin);
  auto CallBegin = llvm::CallInst::Create(Fun, {DILoc, DIFunc}, "", &Call);
  auto InstrMD = MDNode::get(M->getContext(), {});
  CallBegin->setMetadata("sapfor.da", InstrMD);
  Fun = getDeclaration(M, tsar::IntrinsicId::func_call_end);
  auto CallEnd = llvm::CallInst::Create(Fun, {DIFunc}, "");
  CallEnd->insertAfter(&Call);
  CallBegin->setMetadata("sapfor.da", InstrMD);
  ++NumCall;
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
    auto Info = mDIStrings.regItem(BasePtr);
    OpIdx = Info.first;
    if (Info.second) {
      auto M = InsertBefore.getModule();
      assert(mDT && "Dominator tree must not be null!");
      auto DIM = buildDIMemory(MemoryLocation::getAfter(BasePtr), Ctx,
                               M->getDataLayout(), *mDT);
      auto ArraySize = ConstantInt::get(Type::getInt64Ty(Ctx), 1);
      regValue(BasePtr, BasePtr->getType(), ArraySize,
        DIM ? &*DIM : nullptr, OpIdx, InsertBefore, *InsertBefore.getModule());
    }
  }
  auto DbgLocIdx = regDebugLoc(DbgLoc);
  auto DILoc = createPointerToDI(DbgLocIdx, InsertBefore);
  auto Addr = new BitCastInst(Ptr,
    Type::getInt8PtrTy(Ctx), "addr", &InsertBefore);
  auto *MD = MDNode::get(Ctx, {});
  Addr->setMetadata("sapfor.da", MD);
  auto DIVar = createPointerToDI(OpIdx, *DILoc);
  auto BasePtrTy = cast_or_null<PointerType>(BasePtr->getType());
  auto PointeeTy{getPointerElementType(*BasePtr)};
  llvm::Instruction *ArrayBase =
    ((PointeeTy && isa<ArrayType>(PointeeTy)) ||
     (isa<AllocaInst>(BasePtr) &&
      cast<AllocaInst>(BasePtr)->isArrayAllocation())) ?
      new BitCastInst(BasePtr, Type::getInt8PtrTy(Ctx),
        BasePtr->getName() + ".arraybase", &InsertBefore) : nullptr;
  if (ArrayBase)
   ArrayBase->setMetadata("sapfor.da", MD);
  return std::make_tuple(DILoc, Addr, DIVar, ArrayBase);
}

void Instrumentation::visitInstruction(Instruction &I) {
  if (I.mayReadOrWriteMemory()) {
    SmallString<64> IStr;
    raw_svector_ostream OS(IStr);
    I.print(OS);
    auto M = I.getModule();
    auto Func = I.getFunction();
    assert(Func && "Function must not be null!");
    auto MD = Func->getSubprogram();
    auto Filename = MD ? MD->getFilename() : StringRef(M->getSourceFileName());
    I.getContext().diagnose(DiagnosticInfoInlineAsm(I,
      Twine("unsupported RW instruction ") + OS.str() + " in " + Filename,
      DS_Warning));
  }
}

void Instrumentation::regReadMemory(Instruction &I, Value &Ptr) {
  if (I.getMetadata("sapfor.da"))
    return;
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto *M = I.getModule();
  llvm::Value *DILoc, *Addr, *DIVar, *ArrayBase;
  std::tie(DILoc, Addr, DIVar, ArrayBase) =
    regMemoryAccessArgs(&Ptr, I.getDebugLoc(), I);
  if (ArrayBase) {
    auto Fun = getDeclaration(M, IntrinsicId::read_arr);
    auto Call = CallInst::Create(Fun.getFunctionType(), Fun.getCallee(),
      {DILoc, Addr, DIVar, ArrayBase}, "", &I);
    Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
    ++NumLoadArray;
  } else {
    auto Fun = getDeclaration(M, IntrinsicId::read_var);
    auto Call = CallInst::Create(Fun.getFunctionType(), Fun.getCallee(),
      {DILoc, Addr, DIVar}, "", &I);
    Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
    ++NumLoadScalar;
  }
}

void Instrumentation::regWriteMemory(Instruction &I, Value &Ptr) {
  if (I.getMetadata("sapfor.da"))
    return;
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  BasicBlock::iterator InsertBefore(I);
  ++InsertBefore;
  auto *M = I.getModule();
  llvm::Value *DILoc, *Addr, *DIVar, *ArrayBase;
  std::tie(DILoc, Addr, DIVar, ArrayBase) =
    regMemoryAccessArgs(&Ptr, I.getDebugLoc(), *InsertBefore);
  if (!Addr)
    return;
  if (ArrayBase) {
    auto Fun = getDeclaration(M, IntrinsicId::write_arr_end);
    auto Call = CallInst::Create(Fun.getFunctionType(), Fun.getCallee(),
      { DILoc, Addr, DIVar, ArrayBase }, "");
    Call->insertBefore(&*InsertBefore);
    Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
    ++NumStoreArray;
  } else {
    auto Fun = getDeclaration(M, IntrinsicId::write_var_end);
    auto Call = CallInst::Create(Fun.getFunctionType(), Fun.getCallee(),
      {DILoc, Addr, DIVar}, "", &*InsertBefore);
    Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
    ++NumStoreScalar;
  }
}

void Instrumentation::visitLoadInst(LoadInst &I) {
  regReadMemory(I, *I.getPointerOperand());
}

void Instrumentation::visitStoreInst(StoreInst &I) {
  regWriteMemory(I, *I.getPointerOperand());
}

void Instrumentation::visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {
  regReadMemory(I, *I.getPointerOperand());
  regWriteMemory(I, *I.getPointerOperand());
}

void Instrumentation::visitAtomicRMWInst(AtomicRMWInst &I) {
  regReadMemory(I, *I.getPointerOperand());
  regWriteMemory(I, *I.getPointerOperand());
}

void Instrumentation::regTypes(Module& M) {
  if (mTypes.numberOfIDs() == 0)
    return;
  auto &Ctx = M.getContext();
  // Get all registered types and fill std::vector<llvm::Constant*>
  // with local indexes and sizes of these types.
  auto &Types = mTypes.getRegister<llvm::Type *>();
  auto DeclTypeFunc = getDeclaration(&M, IntrinsicId::decl_types);
  auto *SizeTy = DeclTypeFunc.getFunctionType()->getParamType(0);
  auto *Int0 = ConstantInt::get(SizeTy, 0);
  std::vector<Constant* > Ids, Sizes;
  auto &DL = M.getDataLayout();
  for(auto &Pair: Types) {
    auto *TypeId = Constant::getIntegerValue(SizeTy,
      APInt(64, Pair.get<TypeRegister::IdTy>()));
    Ids.push_back(TypeId);
    auto *TypeSize = Pair.get<Type *>()->isSized() ?
      Constant::getIntegerValue(SizeTy,
        APInt(64, DL.getTypeSizeInBits(Pair.get<Type *>()))) : Int0;
    Sizes.push_back(TypeSize);
  }
  // Create global values for IDs and sizes. initialize them with local values.
  auto ArrayTy = ArrayType::get(SizeTy, Types.size());
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
    FunctionType::get(Type::getVoidTy(Ctx), { SizeTy }, false);
  auto RegTypeFunc = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.register.type", &M);
  auto *Size = ConstantInt::get(SizeTy, Types.size());
  addNameDAMetadata(*RegTypeFunc, "sapfor.da", "sapfor.register.type",
    {ConstantAsMetadata::get(Size)});
  RegTypeFunc->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto EntryBB = BasicBlock::Create(Ctx, "entry", RegTypeFunc);
  auto *StartId = &*RegTypeFunc->arg_begin();
  StartId->setName("startid");
  // Create loop to update indexes: NewTypeId = StartId + LocalTypId;
  auto *LoopBB = BasicBlock::Create(Ctx, "loop", RegTypeFunc);
  BranchInst::Create(LoopBB, EntryBB);
  auto *Counter = PHINode::Create(SizeTy, 0, "typeidx", LoopBB);
  Counter->addIncoming(Int0, EntryBB);
  auto *GEP = GetElementPtrInst::Create(
    IdsArray->getValueType(), IdsArray, { Int0, Counter }, "arrayidx", LoopBB);
  auto *LocalTypeId = new LoadInst(
    GEP->getResultElementType(), GEP, "typeid", false, LoopBB);
  auto Add = BinaryOperator::CreateNUW(
    BinaryOperator::Add, LocalTypeId, StartId, "add", LoopBB);
  new StoreInst(Add, GEP, false, LoopBB);
  auto Inc = BinaryOperator::CreateNUW(BinaryOperator::Add, Counter,
    ConstantInt::get(SizeTy, 1), "inc", LoopBB);
  Counter->addIncoming(Inc, LoopBB);
  auto *Cmp = new ICmpInst(*LoopBB, CmpInst::ICMP_ULT, Inc, Size, "cmp");
  auto *EndBB = BasicBlock::Create(M.getContext(), "end", RegTypeFunc);
  BranchInst::Create(LoopBB, EndBB, Cmp, LoopBB);
  auto *IdsArg = GetElementPtrInst::Create(IdsArray->getValueType(), IdsArray,
    { Int0, Int0 }, "ids", EndBB);
  auto *SizesArg = GetElementPtrInst::Create(
      SizesArray->getValueType(), SizesArray, {Int0, Int0}, "sizes", EndBB);
  CallInst::Create(DeclTypeFunc, { Size, IdsArg, SizesArg }, "", EndBB);
  ReturnInst::Create(Ctx, EndBB);
  NumType += Ids.size();
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
  auto DIPoolPtr = new LoadInst(mDIPool->getValueType(), mDIPool, "dipool", T);
  auto GEP = GetElementPtrInst::Create(mDIPoolElementTy, DIPoolPtr, {IdxV},
                                       "arrayidx", T);
  SmallString<256> SingleStr;
  auto DIString = createDIStringPtr(Str.toStringRef(SingleStr), *T);
  auto Offset = &*mInitDIAll->arg_begin();
  CallInst::Create(InitDIFunc.getFunctionType(), InitDIFunc.getCallee(),
     {GEP, DIString, Offset}, "", T);
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
    Var->getValueType(), Var, { Int0,Int0 }, "distring", &InsertBefore);
}

LoadInst* Instrumentation::createPointerToDI(
    DIStringRegister::IdTy Idx, Instruction& InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto *MD = MDNode::get(Ctx, {});
  auto IdxV = ConstantInt::get(Type::getInt64Ty(Ctx), Idx);
  auto DIPoolPtr =
    new LoadInst(mDIPool->getValueType(), mDIPool, "dipool", &InsertBefore);
  DIPoolPtr->setMetadata("sapfor.da", MD);
  auto GEP = GetElementPtrInst::Create(mDIPoolElementTy, DIPoolPtr, {IdxV},
                                       "arrayidx");
  GEP->setMetadata("sapfor.da", MD);
  GEP->insertAfter(DIPoolPtr);
  GEP->setIsInBounds(true);
  auto &DL = InsertBefore.getModule()->getDataLayout();
  auto DI = new LoadInst(GEP->getResultElementType(), GEP, "di", false,
                         DL.getABITypeAlign(GEP->getResultElementType()));
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
  auto DbgLocInfo = mDIStrings.regItem(DbgLoc.get());
  if (!DbgLocInfo.second)
    return DbgLocInfo.first;
  std::string ColStr = !DbgLoc.getCol() ? std::string("") :
    ("col1=" + Twine(DbgLoc.getCol()) + "*").str();
  auto *Scope = cast<DIScope>(DbgLoc->getScope());
  SmallString<128> Filename;
  if (Scope->getFile())
    getAbsolutePath(*Scope, Filename);
  createInitDICall(
    Twine("type=") + "file_name" + "*" +
    "file=" + Filename+ "*" +
    "line1=" + Twine(DbgLoc.getLine()) + "*" + ColStr + "*", DbgLocInfo.first);
  return DbgLocInfo.first;
}

void Instrumentation::regValue(Value *V, Type *T, Value *ArraySize,
    const DIMemoryLocation *DIM, DIStringRegister::IdTy Idx,
    Instruction &InsertBefore, Module &M) {
  SmallVector<Value *, 3> Args;
  auto SizeArgTy = getType(M.getContext(), IntrinsicId::reg_arr)->getParamType(1);
  regValueArgs(V, T, ArraySize, SizeArgTy, DIM, Idx, InsertBefore, M, Args);
  CallInst *Call = nullptr;
  if (Args.size() > 2) {
    auto Func = getDeclaration(&M, IntrinsicId::reg_arr);
    auto FuncTy = Func.getFunctionType();
    assert(FuncTy->getNumParams() > 2 && "Too few arguments!");
    Call = CallInst::Create(FuncTy, Func.getCallee(), Args, "", &InsertBefore);
  } else {
    auto Func = getDeclaration(&M, IntrinsicId::reg_var);
    Call = CallInst::Create(Func.getFunctionType(), Func.getCallee(),
      Args, "", &InsertBefore);
  }
  Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
}

void Instrumentation::regValueArgs(Value *V, Type *T,
    Value *ArraySize, Type *SizeArgTy,
    const DIMemoryLocation *DIM, DIStringRegister::IdTy Idx,
    Instruction &InsertBefore, Module &M, SmallVectorImpl<Value *> &Args) {
  assert(V && "Variable must not be null!");
  assert(T && "Type must not be null!");
  assert(ArraySize && "Size of allocated memory must not be null!");
  assert(SizeArgTy && "Type of ArraySize parameter of registration function must not be null!");
  LLVM_DEBUG(dbgs()<<"[INSTR]: register variable "<<(DIM ? "" : "without metadata ");
    V->printAsOperand(dbgs()); dbgs() << "\n");
  SmallString<128> Filename;
  auto DeclStr =
      DIM && DIM->isValid() ? DIM->Loc && DIM->Loc->getScope()->getFile() ?
    (Twine("file=") + getAbsolutePath(*DIM->Loc->getScope(), Filename) + "*" +
      "line1=" + Twine(DIM->Loc->getLine()) + "*"
      "col1=" + Twine(DIM->Loc->getColumn()) + "*").str() :
    (Twine("file=") +
      (DIM->Var->getFile() ?
        getAbsolutePath(*DIM->Var->getFile(), Filename) :
        StringRef(Filename)) + "*" +
      "line1=" + Twine(DIM->Var->getLine()) + "*").str() :
    (Twine("file=") +
      (sys::fs::real_path(M.getSourceFileName(), Filename) ?
        Filename = M.getSourceFileName(), Filename : Filename)
      + "*").str();
  std::string NameStr;
  if (DIM && DIM->isValid())
    if (auto DWLang = getLanguage(*DIM->Var)) {
      SmallString<16> DIName;
      if (unparseToString(*DWLang, *DIM, DIName)) {
        std::replace(DIName.begin(), DIName.end(), '*', '^');
        NameStr = ("name1=" + DIName + "*").str();
      }
    }
  unsigned Rank;
  uint64_t ArraySizeFromTy;
  Type *ElTy;
  std::tie(Rank, ArraySizeFromTy, ElTy) = arraySize(T);
  if (!isa<ConstantInt>(ArraySize) || !cast<ConstantInt>(ArraySize)->isOne())
      ++Rank;
  unsigned TypeId = mTypes.regItem(ElTy).first;
  auto TypeStr = Rank == 0 ? (Twine("var_name") + "*").str() :
    (Twine("arr_name") + "*" + "rank=" + Twine(Rank) + "*").str();
  createInitDICall(
    Twine("type=") + TypeStr +
    "vtype=" + Twine(TypeId) + "*" + DeclStr + NameStr +
    "local=" + (isa<AllocaInst>(V) ? "1" : "0") + "*" + "*",
    Idx);
  auto DIVar = createPointerToDI(Idx, InsertBefore);
  auto VarAddr = new BitCastInst(V,
    Type::getInt8PtrTy(M.getContext()), V->getName() + ".addr", &InsertBefore);
  VarAddr->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  if (Rank != 0) {
    Value *Size = ConstantInt::get(SizeArgTy, ArraySizeFromTy);
    if (!isa<ConstantInt>(ArraySize) || !cast<ConstantInt>(ArraySize)->isOne()) {
      Instruction *ComputeSize = CastInst::CreateIntegerCast(
        ArraySize, SizeArgTy, false, "cast", &InsertBefore);
      ComputeSize->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
      if (ArraySizeFromTy != 1) {
        ComputeSize = BinaryOperator::CreateNUW(BinaryOperator::Mul,
          Size, ComputeSize, "array.size", &InsertBefore);
        ComputeSize->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
      }
      Size = ComputeSize;
    }
    Args.append({ DIVar, Size, VarAddr });
    ++NumArray;
  } else {
    Args.append({ DIVar, VarAddr });
   ++NumScalar;
  }
}

void Instrumentation::regFunctions(Module& M) {
  for (auto &F : M) {
    IntrinsicId LibId;
    if (getTsarLibFunc(F.getName(), LibId)) {
      F.setMetadata("sapfor.da", MDNode::get(F.getContext(), {}));
      continue;
    }
    if (F.getMetadata("sapfor.da") || F.getMetadata("sapfor.da.ignore"))
      continue;
    if (isDbgInfoIntrinsic(F.getIntrinsicID()) ||
        isMemoryMarkerIntrinsic(F.getIntrinsicID()))
      continue;
    auto Idx = mDIStrings.regItem(&F).first;
    regFunction(F, F.getReturnType(), F.getFunctionType()->getNumParams(),
      findMetadata(&F), Idx, M);
  }
}

void Instrumentation::regGlobals(Module& M) {
  auto &Ctx = M.getContext();
  auto FuncType = FunctionType::get(Type::getVoidTy(Ctx), false);
  auto RegGlobalFunc = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.register.global", &M);
  auto *EntryBB = BasicBlock::Create(Ctx, "entry", RegGlobalFunc);
  auto *RetInst = ReturnInst::Create(mInitDIAll->getContext(), EntryBB);
  DIStringRegister::IdTy RegisteredGLobals = 0;
  for (auto I = M.global_begin(), EI = M.global_end(); I != EI; ++I) {
    if (I->getMetadata("sapfor.da"))
      continue;
    ++RegisteredGLobals;
    auto Idx = mDIStrings.regItem(&(*I)).first;
    SmallVector<DIMemoryLocation, 1> DILocs;
    auto DIM = findMetadata(&*I, DILocs);
    auto ArraySize = ConstantInt::get(Type::getInt64Ty(Ctx), 1);
    regValue(&*I, I->getValueType(), ArraySize,
      DIM ? &*DIM : nullptr, Idx, *RetInst, M);
  }
  if (RegisteredGLobals == 0)
    RegGlobalFunc->eraseFromParent();
  else
    addNameDAMetadata(*RegGlobalFunc, "sapfor.da", "sapfor.register.global");
}

/// Find available suffix for a specified name of a global object to resolve
/// conflicts between names in a specified module.
static Optional<unsigned> findAvailableSuffix(Module &M, unsigned MinSuffix,
    StringRef Name) {
  while (M.getNamedValue((Name + Twine(MinSuffix)).str())) {
    ++MinSuffix;
    if (MinSuffix == std::numeric_limits<unsigned>::max())
      return None;
  }
  return MinSuffix;
}

/// Find available suffix for a specified name of a global object to resolve
/// conflicts between names in a specified modules.
static Optional<unsigned> findAvailableSuffix(Module &M, unsigned MinSuffix,
    StringRef Name, ArrayRef<Module *> Modules) {
  auto Suffix = findAvailableSuffix(M, MinSuffix, Name);
  if (!Suffix)
    return None;
  for (auto *OtherM : Modules) {
    if (OtherM == &M)
      continue;
    Suffix = findAvailableSuffix(*OtherM, *Suffix, Name);
    if (!Suffix)
      return None;
  }
  return Suffix;
}

void tsar::visitEntryPoint(Function &Entry, ArrayRef<Module *> Modules) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process entry point ";
    Entry.printAsOperand(dbgs()); dbgs() << "\n");
  // Erase all existent initialization functions from the modules and remember
  // index of metadata operand which points to the removed function.
  DenseMap<Module *, unsigned> InitMDToFuncOp;
  for (auto *M : Modules)
    if (auto OpIdx = eraseFromParent(*M, "sapfor.da", "sapfor.init.module"))
      InitMDToFuncOp.try_emplace(M, *OpIdx);
  Optional<unsigned> Suffix = 0;
  std::vector<unsigned> InitSuffixes;
  auto PoolSizeTy = Type::getInt64Ty(Entry.getContext());
  APInt PoolSize(PoolSizeTy->getBitWidth(), 0);
  for (auto *M: Modules) {
    LLVM_DEBUG(dbgs() << "[INSTR]: initialize module "
      << M->getSourceFileName() << "\n");
    auto NamedMD = M->getNamedMetadata("sapfor.da");
    if (!NamedMD) {
      M->getContext().diagnose(DiagnosticInfoInlineAsm(
        Twine("ignore ") + M->getSourceFileName() + " due to instrumentation "
        "is not available", DS_Warning));
      continue;
    }
    Suffix = findAvailableSuffix(*M, *Suffix, "sapfor.init.module", Modules);
    if (!Suffix)
      report_fatal_error(Twine("unable to initialize instrumentation for ") +
        M->getSourceFileName() + ": can not generate unique name"
        "of external function");
    InitSuffixes.push_back(*Suffix);
    // Now, we create a function to initialize instrumentation.
    auto IdTy = getInstrIdType(M->getContext());
    auto InitFuncTy = FunctionType::get(IdTy, { IdTy }, false);
    auto InitFunc = Function::Create(InitFuncTy, GlobalValue::ExternalLinkage,
      "sapfor.init.module" + Twine(*Suffix), M);
    assert(InitFunc->getName() == ("sapfor.init.module" + Twine(*Suffix)).str()
      && "Unable to initialized instrumentation for a module!");
    InitFunc->arg_begin()->setName("startid");
    auto BB = BasicBlock::Create(M->getContext(), "entry", InitFunc);
    auto NamedOpItr = InitMDToFuncOp.find(M);
    if (NamedOpItr == InitMDToFuncOp.end()) {
      addNameDAMetadata(*InitFunc, "sapfor.da", "sapfor.init.module");
    } else {
      auto InitMD = getMDOfKind(*NamedMD, "sapfor.init.module");
      InitFunc->setMetadata("sapfor.da", InitMD);
      InitMD->replaceOperandWith(NamedOpItr->second,
        ValueAsMetadata::get(InitFunc));
    }
    auto *DIPoolMD = getMDOfKind(*NamedMD, "sapfor.di.pool");
    if (!DIPoolMD || !extractMD<GlobalVariable>(*DIPoolMD).first ||
         !extractMD<ConstantInt>(*DIPoolMD).first)
      report_fatal_error(Twine("'sapfor.di.pool' is not available for ") +
        M->getSourceFileName());
    PoolSize += extractMD<ConstantInt>(*DIPoolMD).first->getValue();
    auto *InitDIMD = getMDOfKind(*NamedMD, "sapfor.init.di");
    if (!InitDIMD || !extractMD<Function>(*InitDIMD).first)
      report_fatal_error(Twine("'sapfor.init.di' is not available for ") +
        M->getSourceFileName());
    auto *InitDIFunc = extractMD<Function>(*InitDIMD).first;
    CallInst::Create(InitDIFunc, { &*InitFunc->arg_begin() }, "", BB);
    auto *RegTyMD = getMDOfKind(*NamedMD, "sapfor.register.type");
    if (!RegTyMD || !extractMD<Function>(*RegTyMD).first ||
        !extractMD<ConstantInt>(*RegTyMD).first)
      report_fatal_error(Twine("'sapfor.register.type' is not available for ") +
        M->getSourceFileName());
    auto *RegTyFunc = extractMD<Function>(*RegTyMD).first;
    CallInst::Create(RegTyFunc, { &*InitFunc->arg_begin() }, "", BB);
    if (auto *RegGlobalMD = getMDOfKind(*NamedMD, "sapfor.register.global"))
      if (auto *RegGlobalFunc = extractMD<Function>(*RegGlobalMD).first)
        CallInst::Create(RegGlobalFunc, {}, "", BB);
      else
        report_fatal_error(
          Twine("'sapfor.register.global' is not available for ") +
          M->getSourceFileName());
    auto FreeId =
      BinaryOperator::CreateNUW(BinaryOperator::Add, &*InitFunc->arg_begin(),
        extractMD<ConstantInt>(*RegTyMD).first, "add", BB);
    ReturnInst::Create(M->getContext(), FreeId, BB);
  }
  auto *EntryM = Entry.getParent();
  assert(EntryM && "Entry point must be in a module!");
  auto *InsertBefore = &Entry.getEntryBlock().front();
  auto AllocatePoolFunc = getDeclaration(EntryM, IntrinsicId::allocate_pool);
  auto PoolSizeV = ConstantInt::get(PoolSizeTy, PoolSize);
  auto *DIPool = getOrCreateDIPool(*EntryM).first;
  if (!DIPool)
    report_fatal_error(Twine("'sapfor.di.pool' is not available for ") +
      EntryM->getSourceFileName());
  auto CallAPF =
    CallInst::Create(AllocatePoolFunc, { DIPool, PoolSizeV}, "", InsertBefore);
  auto InstrMD = MDNode::get(EntryM->getContext(), {});
  CallAPF->setMetadata("sapfor.da", InstrMD);
  auto IdTy = getInstrIdType(Entry.getContext());
  auto *InitFuncTy = FunctionType::get(IdTy, { IdTy }, false);
  Value *FreeId = llvm::ConstantInt::get(IdTy, 0);
  for (auto Suffix : InitSuffixes) {
    auto InitFunc = EntryM->getOrInsertFunction(
      ("sapfor.init.module" + Twine(Suffix)).str(), InitFuncTy);
    FreeId = CallInst::Create(InitFunc.getFunctionType(), InitFunc.getCallee(),
      {FreeId}, "freeid", InsertBefore);
    cast<CallInst>(FreeId)->setMetadata("sapfor.da", InstrMD);
  }
}
