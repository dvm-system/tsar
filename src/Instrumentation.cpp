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
#include "tsar_instrumentation.h"
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

Instrumentation::Instrumentation(Module &M, InstrumentationPass* const I)
  : mInstrPass(I) {
  const std::string& ModuleName = M.getModuleIdentifier();
  //create function for types registration
  const std::string& RegTypeFuncName = "RegType" +
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto RegType = FunctionType::get(Type::getInt64Ty(M.getContext()),
    {Type::getInt64Ty(M.getContext())}, false);
  auto RegFunc = Function::Create(RegType,
    GlobalValue::LinkageTypes::InternalLinkage, RegTypeFuncName, &M);
  RegFunc->arg_begin()->setName("x");
  auto NewBlock = BasicBlock::Create(RegFunc->getContext(), "entry", RegFunc);
  auto DIPoolTy = PointerType::getUnqual(Type::getInt8PtrTy(M.getContext()));
  mDIPool = new GlobalVariable(M, DIPoolTy, false,
    GlobalValue::LinkageTypes::ExternalLinkage,
    ConstantPointerNull::get(DIPoolTy), "sapfor.di.pool", nullptr);
  mDIPool->setAlignment(4);
  //create function for debug information initialization
  auto Type = FunctionType::get(Type::getVoidTy(M.getContext()),
    {Type::getInt64Ty(M.getContext())}, false);
  mInitDIAll = Function::Create(
    Type, GlobalValue::LinkageTypes::InternalLinkage, "sapfor.init.di.all", &M);
  mInitDIAll->arg_begin()->setName("Offset");
  NewBlock = BasicBlock::Create(mInitDIAll->getContext(), "entry", mInitDIAll);
  ReturnInst::Create(mInitDIAll->getContext(), NewBlock);
  //registrate basic types and global variables
  regGlobals(M);
  //visit all functions
  for(auto& F: M) {
    //not sapforInitDI or RegType function
    if(F.getSubprogram() != nullptr) {
      visitFunction(F);
    }
  }
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

//insert call of sapforRegVar(void*, void*) or
//sapforRegArr(void*, size_t, void*) after specified alloca instruction.
void Instrumentation::visitAllocaInst(llvm::AllocaInst &I) {
  auto Metadata = getMetadata(&I);
  if(Metadata == nullptr)
    return;
  unsigned ID = mTypes.regItem(I.getAllocatedType());
  std::stringstream Debug;
  auto Addr = new BitCastInst(&I, Type::getInt8PtrTy(I.getContext()), "Addr");
  cast<Instruction>(Addr)->insertAfter(&I);
  auto TypeId = I.getAllocatedType()->getTypeID();
  //Alloca instruction has isArrayAllocation method, but it looks like it
  //doesn't work like i wanted it. So checking for array in this way
  if(TypeId == Type::TypeID::ArrayTyID){
    Debug << "type=arr_name*file=" << I.getModule()->getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" <<
      Metadata->getName().data() << "*vtype=" << ID << "*rank=1**";
    auto Idx = mDIStrings.regItem(&I);
    createInitDICall(Debug.str(), Idx, *I.getModule());
    auto DIVar = createPointerToDI(Idx, I);
    auto ArrSize = ConstantInt::get(Type::getInt64Ty(I.getContext()),
      I.getAllocatedType()->getArrayNumElements());
    auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_arr);
    auto Call = CallInst::Create(Fun, {DIVar, ArrSize, Addr}, "");
    Call->insertAfter(Addr);
  } else {
    Debug << "type=var_name*file=" << I.getModule()->getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" <<
      Metadata->getName().data() << "*vtype=" << ID << "**";
    auto Idx = mDIStrings.regItem(&I);
    createInitDICall(Debug.str(), Idx, *I.getModule());
    auto DIVar = createPointerToDI(Idx, I);
    auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_var);
    auto Call = CallInst::Create(Fun, {DIVar, Addr}, "");
    Call->insertAfter(Addr);
  }
}

void Instrumentation::visitCallInst(llvm::CallInst &I) {
  //using template method
  FunctionCallInst(I);
}

void Instrumentation::visitInvokeInst(llvm::InvokeInst &I) {
  //using template method
  FunctionCallInst(I);
}

void Instrumentation::visitReturnInst(llvm::ReturnInst &I) {
  //not llvm function
  if(I.getFunction()->isIntrinsic())
    return;
  //not tsar instrumentation function
  IntrinsicId Id;
  if(getTsarLibFunc(I.getFunction()->getName().data(), Id)) {
    return;
  }
  auto Fun = getDeclaration(I.getModule(), IntrinsicId::func_end);
  unsigned Idx = mDIStrings[I.getFunction()];
  auto DIFunc = createPointerToDI(Idx, I);
  auto Call = CallInst::Create(Fun, {DIFunc}, "");
  Call->insertAfter(DIFunc);
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
    createInitDICall(Debug.str(), Idx, *B.getModule());
    /*auto Region = mRegionInfo->getRegionFor(Loop);
    auto CanonLoop = mCanonicalLoop->find_as(Region);
    if(CanonLoop != mCanonicalLoop->end()) {
    }*/
    loopBeginInstr(Loop, B, Idx);
    loopEndInstr(Loop, B, Idx);
    loopIterInstr(Loop, B, Idx);
  }
  //visit all Instructions
  for(auto& I : B){
    visit(I);
  }
}

void Instrumentation::visitFunction(llvm::Function &F) {
  if(F.isIntrinsic())
    return;
  IntrinsicId Id;
  if(getTsarLibFunc(F.getName(), Id)) {
    return;
  }
  if(F.getLinkage() == Function::LinkOnceAnyLinkage ||
     F.getLinkage() == Function::LinkOnceODRLinkage)
    F.setLinkage(Function::InternalLinkage);
  //get analysis informaion from passes for visited function
  auto& Provider = mInstrPass->template getAnalysis<InstrumentationPassProvider>(F);
  auto& LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  mRegionInfo = &Provider.get<DFRegionInfoPass>().getRegionInfo();
  mLoopInfo = &LI;
  auto CLI  = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  mCanonicalLoop = &CLI;
  //registrate debug information for function
  std::stringstream Debug;
  Debug << "type=function*file=" << F.getParent()->getSourceFileName() <<
    "*line1=" << F.getSubprogram()->getLine() << "*line2=" <<
    (F.getSubprogram()->getLine() + F.getSubprogram()->getScopeLine()) <<
    "*name1=" << F.getSubprogram()->getName().data() << "*vtype=" <<
    mTypes.regItem(F.getReturnType()) << "*rank=" <<
    F.getFunctionType()->getNumParams() << "**";
  auto Idx = mDIStrings.regItem(&F);
  createInitDICall(Debug.str(), Idx, *F.getParent());

  //visit all Blocks
  for(auto &I : F.getBasicBlockList()) {
    visitBasicBlock(I);
  }
  //Insert a call of sapforFuncBegin(void*) in the begginning of the function
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto Begin = inst_begin(F);
  auto DIFunc = createPointerToDI(Idx, *Begin);
  auto Call = CallInst::Create(Fun, {DIFunc}, "");
  Call->insertAfter(DIFunc);
}

void Instrumentation::visitLoadInst(llvm::LoadInst &I) {
  if(!cast<Instruction>(I).getDebugLoc() ||
    !I.getPointerOperand()->stripInBoundsOffsets()->isUsedByMetadata()) {
    return;
  }
  std::stringstream Debug;
  Debug << "type=file_name*file=" << I.getModule()->getSourceFileName() <<
    "*line1=" << cast<Instruction>(I).getDebugLoc().getLine() << "**";
  auto DbgLocIdx = mDIStrings.regItem(I.getDebugLoc().get());
  createInitDICall(Debug.str(), DbgLocIdx, *I.getModule());
  auto DILoc = createPointerToDI(DbgLocIdx, I);
  auto Ptr = I.getPointerOperand()->stripInBoundsOffsets();
  DIStringRegister::IdTy OpIdx = 0;
  if (isa<AllocaInst>(Ptr))
    OpIdx = mDIStrings[cast<AllocaInst>(Ptr)];
  else
    OpIdx = mDIStrings[cast<GlobalVariable>(Ptr)];
  auto DIVar = createPointerToDI(OpIdx, *DILoc);
  auto Addr = new BitCastInst(I.getPointerOperand(),
    Type::getInt8PtrTy(I.getContext()), "Addr");
  cast<Instruction>(Addr)->insertAfter(DILoc);
  Function* Fun;
  auto TypeID =  I.getPointerOperand()->stripInBoundsOffsets()->getType()
    ->getPointerElementType()->getTypeID();
  //depending on operand type insert a sapforReadVar or sapforReadArr
  if(TypeID == Type::TypeID::ArrayTyID){
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_arr);
    auto ArrBase =new BitCastInst(I.getPointerOperand()->stripInBoundsOffsets(),
      Type::getInt8PtrTy(I.getContext()), "ArrBase");
    cast<Instruction>(ArrBase)->insertAfter(Addr);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar, ArrBase}, "");
    Call->insertAfter(ArrBase);
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_var);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar}, "");
    Call->insertAfter(Addr);
  }
}

void Instrumentation::visitStoreInst(llvm::StoreInst &I) {
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
  if(!cast<Instruction>(I).getDebugLoc() ||
    !I.getPointerOperand()->stripInBoundsOffsets()->isUsedByMetadata())
    return;
  std::stringstream Debug;
  Debug << "type=file_name*file=" << I.getModule()->getSourceFileName() <<
    "*line1=" << (!cast<Instruction>(I).getDebugLoc() ? 0 :
    cast<Instruction>(I).getDebugLoc().getLine()) << "**";
  auto DbgLocIdx = mDIStrings.regItem(I.getDebugLoc().get());
  createInitDICall(Debug.str(), DbgLocIdx, *I.getModule());
  auto DILoc = createPointerToDI(DbgLocIdx, I);
  auto Ptr = I.getPointerOperand()->stripInBoundsOffsets();
  DIStringRegister::IdTy OpIdx = 0;
  if (isa<AllocaInst>(Ptr))
    OpIdx = mDIStrings[cast<AllocaInst>(Ptr)];
  else
    OpIdx = mDIStrings[cast<GlobalVariable>(Ptr)];
  auto DIVar = createPointerToDI(OpIdx, I);
  auto Addr = new BitCastInst(I.getPointerOperand(),
    Type::getInt8PtrTy(I.getContext()), "Addr", &I);
  Function* Fun;
  auto TypeID =  I.getPointerOperand()->stripInBoundsOffsets()->getType()
    ->getPointerElementType()->getTypeID();
  //depending on operand type insert a sapforWriteVarEnd or sapforWriteArrEnd
  if(TypeID != Type::TypeID::ArrayTyID) {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_var_end);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar}, "");
    Call->insertAfter(&I);
  } else {
    auto ArrBase =new BitCastInst(I.getPointerOperand()->stripInBoundsOffsets(),
      Type::getInt8PtrTy(I.getContext()), "ArrBase", &I);
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_arr_end);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar, ArrBase}, "");
    Call->insertAfter(&I);
  }
}

void Instrumentation::regTypes(Module& M) {
  const std::string& ModuleName = M.getModuleIdentifier();
  const std::string& RegFuncName = "RegType" +
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto F = M.getFunction(RegFuncName); // get function for types registration
  if(F != nullptr) {
    DataLayout DL(&M);
    // Get all registered types and fill std::vector<llvm::Constant*>
    // with local indexes and sizes of these types.
    auto &Types = mTypes.getRegister<llvm::Type *>();
    auto Int64Ty = Type::getInt64Ty(M.getContext());
    auto Int0 = ConstantInt::get(Int64Ty, 0);
    std::vector<Constant* > Ids, Sizes;
    for(auto I: Types) {
      Ids.push_back(Constant::getIntegerValue(Int64Ty, APInt(64, I.second)));
      Sizes.push_back(I.first->isSized() ? Constant::getIntegerValue(Int64Ty,
	APInt(64, DL.getTypeSizeInBits(const_cast<Type*>(I.first)))) : Int0);
    }
    //create global values for idexes and sizes. initialize them with local
    //values
    auto Size = ConstantInt::get(Int64Ty, Types.size());
    auto ArrayTy = ArrayType::get(Int64Ty, Types.size());
    auto IdsArray = new GlobalVariable(M, ArrayTy, false,
      GlobalValue::LinkageTypes::ExternalLinkage, ConstantArray::get(ArrayTy,
      Ids), "IdsArray", nullptr);
    auto SizesArray = new GlobalVariable(M, ArrayTy, false,
      GlobalValue::LinkageTypes::ExternalLinkage, ConstantArray::get(ArrayTy,
      Sizes), "SizesArray", nullptr);
    //create a loop to update local indexes to their real values.
    //
    //add loop counter to entry block and initialize it with 0
    //store function parameter
    auto AI = new AllocaInst(F->arg_begin()->getType(), nullptr, 8, "x.addr",
      &F->getEntryBlock());
    auto Counter = new AllocaInst(Int64Ty, nullptr, 8, "i",
      &F->getEntryBlock());
    auto SI = new StoreInst(&(*F->arg_begin()), AI, &F->getEntryBlock());
    auto Set0 = new StoreInst(Int0, Counter, false, 8,
      &F->getEntryBlock());
    //create condition, body, increment and end blocks for the loop.
    auto CondBlock = BasicBlock::Create(M.getContext(), "cond", F);
    auto BodyBlock = BasicBlock::Create(M.getContext(), "body", F);
    auto IncBlock = BasicBlock::Create(M.getContext(), "inc", F);
    auto EndBlock = BasicBlock::Create(M.getContext(), "end", F);
    //create jumps between loop blocks
    BranchInst::Create(CondBlock, &F->getEntryBlock());
    auto LoadCounter = new LoadInst(Counter, "", false, 8, CondBlock);
    auto Cmp = new ICmpInst(*CondBlock, CmpInst::ICMP_ULT, LoadCounter,
      Size, "cmp");
    BranchInst::Create(BodyBlock, EndBlock, Cmp, CondBlock);
    BranchInst::Create(IncBlock, BodyBlock);
    BranchInst::Create(CondBlock, IncBlock);
    //fill body block with corresponding instructions:
    //IdsArray[i] += x  ;; x - function parameter
    LoadCounter = new LoadInst(Counter, "", false, 8, &BodyBlock->back());
    auto Elem = GetElementPtrInst::Create(nullptr, IdsArray, {Int0,
      LoadCounter}, "arrayidx", &BodyBlock->back());
    auto LoadArg = new LoadInst(AI, "", false, 8, &BodyBlock->back());
    auto LoadArg2 = new LoadInst(Elem, "", false, 8, &BodyBlock->back());
    auto Sum = BinaryOperator::CreateNSW(BinaryOperator::Add, LoadArg,
      LoadArg2, "", &BodyBlock->back());
    auto NewElem = new StoreInst(Sum, Elem, false, 8, &BodyBlock->back());
    //fill increment block with corresponding instructions:
    //i++
    LoadCounter = new LoadInst(Counter, "", false, 8, &IncBlock->back());
    Sum = BinaryOperator::CreateNSW(BinaryOperator::Add, LoadCounter,
      ConstantInt::get(Int64Ty, 1), "", &IncBlock->back());
    auto NewCounter = new StoreInst(Sum, Counter, false, 8, &IncBlock->back());
    //insert call of sapforDeclTypes(i64, i64*, i64*) to the end block
    auto Fun4Insert = getDeclaration(&M, IntrinsicId::decl_types);
    auto IdsArrayElem = GetElementPtrInst::Create(nullptr, IdsArray,
      {Int0, Int0}, "Ids", EndBlock);
    auto SizesArrayElem = GetElementPtrInst::Create(nullptr, SizesArray,
      {Int0, Int0}, "Sizes", EndBlock);
    CallInst::Create(Fun4Insert, {Size, IdsArrayElem, SizesArrayElem}, "",
      EndBlock);
    //return numb of regitrated types
    ReturnInst::Create(F->getContext(), Size, EndBlock);
    return;
  }
  llvm_unreachable((RegFuncName + " was not declared").c_str());
}

void Instrumentation::createInitDICall(const llvm::Twine &Str,
    DIStringRegister::IdTy Idx, Module &M) {
  assert(mDIPool && "Pool of metadata strings must not be null!");
  assert(mInitDIAll &&
    "Metadata strings initialization function must not be null!");
  auto &BB = mInitDIAll->getEntryBlock();
  auto *T = BB.getTerminator();
  assert(T && "Terminator must not be null!");
  auto InitDIFunc = getDeclaration(&M, IntrinsicId::init_di);
  auto IdxV = ConstantInt::get(Type::getInt64Ty(M.getContext()), Idx);
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
  auto Int0 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0);
  return GetElementPtrInst::CreateInBounds(
    Var, { Int0,Int0 }, "distring", &InsertBefore);
}

LoadInst* Instrumentation::createPointerToDI(
    DIStringRegister::IdTy Idx, Instruction& InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto IdxV = ConstantInt::get(Type::getInt64Ty(Ctx), Idx);
  auto DIPoolPtr = new LoadInst(mDIPool, "dipool", &InsertBefore);
  auto GEP = GetElementPtrInst::Create(nullptr, DIPoolPtr, {IdxV}, "arrayidx");
  GEP->insertAfter(DIPoolPtr);
  auto DI = new LoadInst(GEP, "di");
  DI->insertAfter(GEP);
  return DI;
}

void Instrumentation::regGlobals(Module& M) {
  for(auto I = M.global_begin(); I != M.global_end(); I++) {
    unsigned ID = mTypes.regItem(I->getValueType());
    auto Metadata = getMetadata(&*I);
    if(Metadata == nullptr) {
      continue;
    }
    std::stringstream Debug;
    Debug << "type=var_name*file=" << M.getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" <<
      Metadata->getName().data() << "*vtype=" << ID << "**";
    auto Idx = mDIStrings.regItem(&(*I));
    createInitDICall(Debug.str(), Idx, M);
  }
}

void Instrumentation::instrumentateMain(Module& M) {
  const std::string& ModuleName = M.getModuleIdentifier();
  auto MainFunc = M.getFunction("main"), RegFunc = M.getFunction("RegType" +
    ModuleName.substr(ModuleName.find_last_of("/\\") + 1));
  if(MainFunc == nullptr || RegFunc == nullptr || mInitDIAll == nullptr)
    return;
  auto& B = MainFunc->getEntryBlock();
  auto Int0 = llvm::ConstantInt::get(Type::getInt64Ty(M.getContext()), 0);
  CallInst::Create(RegFunc, {Int0}, "", &(*B.begin()));
  CallInst::Create(mInitDIAll, {Int0}, "", &(*B.begin()));
}
