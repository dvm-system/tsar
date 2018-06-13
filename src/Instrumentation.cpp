//===- tsar_instrumentation.cpp - TSAR Instrumentation Engine ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements LLVM IR level instrumentation engine.
//
//===----------------------------------------------------------------------===//
//
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>

#include "tsar_utility.h"
#include "Instrumentation.h"
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
  MemoryMatcherImmutableWrapper> InstrumentationPassProvider;

STATISTIC(NumInstLoop, "Number of instrumented loops");

INITIALIZE_PROVIDER_BEGIN(InstrumentationPassProvider, "instrumentation-provider",
  "Instrumentation Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
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

void Instrumentation::loopBeginInstr(llvm::Loop *L,
  llvm::BasicBlock &Header, unsigned Idx){
  //looking through all possible loop predeccessors
  for(auto it = pred_begin(&Header), et = pred_end(&Header); it != et; ++it) {
    BasicBlock* Predeccessor = *it;
    if(!L->contains(Predeccessor)) {
      auto ExitInstr = Predeccessor->getTerminator();
      //looking for successor which is our Header
      for(unsigned I = 0; I < ExitInstr->getNumSuccessors(); ++I) {
        //create a new BasicBlock between predeccessor and Header
        //insert a function call there
        if(ExitInstr->getSuccessor(I) == &Header) {
          auto Block4Insert = BasicBlock::Create(Header.getContext(),
            "loop_begin", Header.getParent());
          IRBuilder<> Builder(Block4Insert, Block4Insert->end());
          Builder.CreateBr(ExitInstr->getSuccessor(I));
          ExitInstr->setSuccessor(I, Block4Insert);
          auto DILoop = createPointerToDI(Idx, *Block4Insert->begin());
          auto Region = mRegionInfo->getRegionFor(L);
          auto CanonLoop = mCanonicalLoop->find_as(Region);
	  ConstantInt *First = nullptr, *Last = nullptr, *Step = nullptr;
          if(CanonLoop != mCanonicalLoop->end() && (*CanonLoop)->isCanonical()){
	    //I don't know what is lying under llvm::Value returned from
	    //getStart/getEnd, but looks like it casts to ConstantInt correctly.
	    if(isa<ConstantInt>((*CanonLoop)->getStart()))
              First = cast<ConstantInt>((*CanonLoop)->getStart());
	    if(isa<ConstantInt>((*CanonLoop)->getEnd()))
	      Last = cast<ConstantInt>((*CanonLoop)->getEnd());
	    if(isa<SCEVConstant>((*CanonLoop)->getStep()))
	      Step = cast<SCEVConstant>((*CanonLoop)->getStep())->getValue();
          }
	  //First, Last and Step should be i64. Seems that CanonLoop pass
	  //returns them as i32. So this changes i32 to i64
          First = (First == nullptr) ? ConstantInt::get(Type::getInt64Ty(Header
	    .getContext()), 0) : ConstantInt::get(Type::getInt64Ty(Header
	    .getContext()),First->getSExtValue());
          Last = (Last == nullptr) ? ConstantInt::get(Type::getInt64Ty(Header
	    .getContext()), 0) : ConstantInt::get(Type::getInt64Ty(Header
	    .getContext()), Last->getSExtValue());
          Step = (Step == nullptr) ? ConstantInt::get(Type::getInt64Ty(Header
	    .getContext()), 0) : ConstantInt::get(Type::getInt64Ty(Header
	    .getContext()), Step->getSExtValue());
          Builder.SetInsertPoint(Block4Insert->getTerminator());
	  //void sapforSLBegin(void*, long, long, long)
          auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_begin);
          auto Call = Builder.CreateCall(Fun, {DILoop, First, Last, Step});
        }
      }
    }
  }
}

void Instrumentation::loopEndInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header, unsigned Idx) {
  //Creating a node between inside/outside blocks of the loop. Then insert
  //call of sapforSLEnd() in this node.
  SmallVector<BasicBlock*, 8> ExitBlocks;
  SmallVector<BasicBlock*, 8> ExitingBlocks;
  L->getExitBlocks(ExitBlocks);
  L->getExitingBlocks(ExitingBlocks);
  for(auto Exiting: ExitingBlocks) {
    auto ExitInstr = Exiting->getTerminator();
    for(unsigned SucNumb = 0; SucNumb<ExitInstr->getNumSuccessors(); ++SucNumb){
      for(auto Exit : ExitBlocks) {
        if(Exiting->getTerminator()->getSuccessor(SucNumb) == Exit) {
          auto Block4Insert = BasicBlock::Create(Header.getContext(),
            "loop_exit", Header.getParent());
          IRBuilder<> Builder(Block4Insert, Block4Insert->end());
          Builder.CreateBr(ExitInstr->getSuccessor(SucNumb));
          ExitInstr->setSuccessor(SucNumb, Block4Insert);
          auto DILoop = createPointerToDI(Idx, *Block4Insert->begin());
          Builder.SetInsertPoint(Block4Insert->getTerminator());
	  //void sapforSLEnd(void*)
          auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_end);
          auto Call = Builder.CreateCall(Fun, {DILoop});
        }
      }
    }
  }
}

void Instrumentation::loopIterInstr(llvm::Loop *L,
  llvm::BasicBlock &Header, unsigned Idx) {
  auto Region = mRegionInfo->getRegionFor(L);
  auto CanonLoop = mCanonicalLoop->find_as(Region);
  //instrumentate iterations only in canonical loops.
  if(CanonLoop != mCanonicalLoop->end() && (*CanonLoop)->isCanonical()){
    auto Induction = (*CanonLoop)->getInduction();
    auto& InsertBefore = Header.front();
    auto Addr = new BitCastInst(Induction,
      Type::getInt8PtrTy(Header.getContext()), "Addr", &InsertBefore);
    auto DILoop = createPointerToDI(Idx, InsertBefore);
    //void sapforSLIter(void*, void*)
    auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_iter);
    CallInst::Create(Fun, {DILoop, Addr}, "", &InsertBefore);
  }
}

void Instrumentation::visitBasicBlock(llvm::BasicBlock &B) {
  if(mLoopInfo->isLoopHeader(&B)) {
    auto Loop = mLoopInfo->getLoopFor(&B);
    unsigned Start = Loop->getStartLoc()->getLine(), End = 0;
    //every loop has start but some could have undefined end (e.g. loops with
    //breaks). Leave End parameter as 0 in that cases.
    if(Loop->getLocRange())
      End = Loop->getLocRange().getEnd().getLine();
    std::stringstream Debug;
    Debug << "type=seqloop*file=" << B.getModule()->getSourceFileName() <<
      "*line1=" << Start << "*line2=" << End << "**";
    auto Idx = mDIStrings.regItem(Loop);
    createInitDICall(Debug.str(), Idx);
    /*auto Region = mRegionInfo->getRegionFor(Loop);
    auto CanonLoop = mCanonicalLoop->find_as(Region);
    if(CanonLoop != mCanonicalLoop->end()) {
    }*/
    loopBeginInstr(Loop, B, Idx);
    loopEndInstr(Loop, B, Idx);
    loopIterInstr(Loop, B, Idx);
  }
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
  auto &Provider = mInstrPass->getAnalysis<InstrumentationPassProvider>(F);
  mLoopInfo = &Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  mRegionInfo = &Provider.get<DFRegionInfoPass>().getRegionInfo();
  mCanonicalLoop = &Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto *M = F.getParent();
  auto *MD = F.getSubprogram();
  auto Idx = mDIStrings[&F];
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto &FirstInst = *inst_begin(F);
  auto DIFunc = createPointerToDI(Idx, FirstInst);
  auto Call = CallInst::Create(Fun, {DIFunc}, "", &FirstInst);
  Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
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
  // instrumentate stores for function formal parameters in special way
  if(Argument::classof(I.getValueOperand()) &&
    AllocaInst::classof(I.getPointerOperand())) {
    auto Idx = mDIStrings[cast<Argument>(I.getValueOperand())->getParent()];
    auto DIFunc = createPointerToDI(Idx, I);
    auto Addr = new BitCastInst(I.getPointerOperand(),
      Type::getInt8PtrTy(I.getContext()), "Addr", &I);
    auto Position = ConstantInt::get(Type::getInt64Ty(I.getContext()),
      cast<Argument>(I.getValueOperand())->getArgNo());
    if(cast<AllocaInst>(I.getPointerOperand())->isArrayAllocation()) {
      auto ArrSize = cast<AllocaInst>(I.getPointerOperand())->getArraySize();
      auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_dummy_arr);
      auto Call = CallInst::Create(Fun, {DIFunc, ArrSize, Addr, Position}, "");
      Call->insertAfter(&I);
    } else {
      auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_dummy_var);
      auto Call = CallInst::Create(Fun, {DIFunc, Addr, Position}, "");
      Call->insertAfter(&I);
    }
  }
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
