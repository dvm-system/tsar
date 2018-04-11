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
  : mInstrPass(I), mLoopInfo(*(new LoopInfo())) {
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
  //insert declaration of pool for debug information
  std::string DbgPoolName = "gSapforDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  //variables names in C can't contain '.'
  for(auto& J : DbgPoolName)
    if(J == '.')
      J = '_';
  auto DbgPool = new GlobalVariable(M, PointerType::getUnqual(Type
    ::getInt8PtrTy(M.getContext())), false, GlobalValue::LinkageTypes
    ::ExternalLinkage, nullptr, DbgPoolName, nullptr);
  DbgPool->setAlignment(4);
  //create function for debug information initialization
  const std::string& InitFuncName = "sapforInitDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto Type = FunctionType::get(Type::getVoidTy(M.getContext()),
    {Type::getInt64Ty(M.getContext())}, false);
  auto Func = Function::Create(Type, GlobalValue::LinkageTypes::InternalLinkage,
    InitFuncName, &M);
  Func->arg_begin()->setName("Offset");
  NewBlock = BasicBlock::Create(Func->getContext(), "entry", Func);
  ReturnInst::Create(Func->getContext(), NewBlock);
  //registrate basic types and global variables
  regBaseTypes(M);
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
    mRegistrator.getDbgStrCounter());
  CallInst::Create(Fun, {DbgPool, Idx}, "", &(*inst_begin(Func)));
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
  unsigned ID = getTypeId(*I.getAllocatedType());
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
    unsigned Idx = regDbgStr(Debug.str(), *I.getModule());
    mRegistrator.regArr(&I, Idx);
    auto DIVar = getDbgPoolElem(Idx, I);
    auto ArrSize = ConstantInt::get(Type::getInt64Ty(I.getContext()),
      I.getAllocatedType()->getArrayNumElements());
    auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_arr);
    auto Call = CallInst::Create(Fun, {DIVar, ArrSize, Addr}, "");
    Call->insertAfter(Addr);
  } else {	  
    Debug << "type=var_name*file=" << I.getModule()->getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" << 
      Metadata->getName().data() << "*vtype=" << ID << "**";
    unsigned Idx = regDbgStr(Debug.str(), *I.getModule());
    mRegistrator.regVar(&I, Idx);
    auto DIVar = getDbgPoolElem(Idx, I);
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
  unsigned Idx = mRegistrator.getFuncDbgIndex(I.getFunction());
  auto DIFunc = getDbgPoolElem(Idx, I);
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
          auto DILoop = getDbgPoolElem(Idx, *Block4Insert->begin());
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
          auto DILoop = getDbgPoolElem(Idx, *Block4Insert->begin());
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
    auto Addr = new BitCastInst(Induction,
      Type::getInt8PtrTy(Header.getContext()), "Addr", Header.getTerminator());
    auto DILoop = getDbgPoolElem(Idx, *Header.getTerminator());
    //void sapforSLIter(void*, void*)
    auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_iter);
    CallInst::Create(Fun, {DILoop, Addr}, "", Header.getTerminator());
  }
} 

void Instrumentation::visitBasicBlock(llvm::BasicBlock &B) {
  if(mLoopInfo.isLoopHeader(&B)) {
    auto Loop = mLoopInfo.getLoopFor(&B);
    std::stringstream Debug;
    Debug << "type=seqloop*file=" << B.getModule()->getSourceFileName() <<
      "*line1=" << Loop->getLocRange().getStart().getLine() << "*line2=" <<
      Loop->getLocRange().getEnd().getLine() << "**";
    unsigned Idx = regDbgStr(Debug.str(), *B.getModule());
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
  //get analysis informaion from passes for visited function
  auto& Provider = mInstrPass->template getAnalysis<InstrumentationPassProvider>(F);
  auto& LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  mRegionInfo = &Provider.get<DFRegionInfoPass>().getRegionInfo();
  mLoopInfo = std::move(LI);
  auto CLI  = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  mCanonicalLoop = &CLI;
  //registrate debug information for function
  getTypeId(*F.getReturnType());
  std::stringstream Debug;
  Debug << "type=function*file=" << F.getParent()->getSourceFileName() <<
    "*line1=" << F.getSubprogram()->getLine() << "*line2=" <<
    (F.getSubprogram()->getLine() + F.getSubprogram()->getScopeLine()) <<
    "*name1=" << F.getSubprogram()->getName().data() << "*vtype=" <<
    getTypeId(*F.getReturnType()) << "*rank=" <<
    F.getFunctionType()->getNumParams() << "**";
  unsigned Idx = regDbgStr(Debug.str(), *F.getParent());
  mRegistrator.regFunc(&F, Idx);

  //visit all Blocks
  for(auto &I : F.getBasicBlockList()) {
    visitBasicBlock(I);
  }
  //Insert a call of sapforFuncBegin(void*) in the begginning of the function
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto Begin = inst_begin(F);
  auto DIFunc = getDbgPoolElem(Idx, *Begin);
  auto Call = CallInst::Create(Fun, {DIFunc}, "");
  Call->insertAfter(DIFunc);
}

void Instrumentation::visitLoadInst(llvm::LoadInst &I) {
  if(!cast<Instruction>(I).getDebugLoc())
    return;
  std::stringstream Debug;
  Debug << "type=file_name*file=" << I.getModule()->getSourceFileName() <<
    "*line1=" << cast<Instruction>(I).getDebugLoc().getLine() << "**";
  auto DILoc = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
  unsigned Idx = mRegistrator.getVarDbgIndex(I.getPointerOperand()
    ->stripInBoundsOffsets());
  auto DIVar = getDbgPoolElem(Idx, *DILoc);
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
    auto Idx = mRegistrator.getFuncDbgIndex(cast<Argument>(I.getValueOperand())
      ->getParent());
    auto DIFunc = getDbgPoolElem(Idx, I);
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
  if(!cast<Instruction>(I).getDebugLoc())
    return;
  std::stringstream Debug;
  Debug << "type=file_name*file=" << I.getModule()->getSourceFileName() <<
    "*line1=" << (!cast<Instruction>(I).getDebugLoc() ? 0 :
    cast<Instruction>(I).getDebugLoc().getLine()) << "**";
  auto DILoc = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
  unsigned Idx = mRegistrator.getVarDbgIndex(I.getPointerOperand()
    ->stripInBoundsOffsets());
  auto DIVar = getDbgPoolElem(Idx, I);
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

//Registrate given debug information by inserting a call of 
//sapforInitDI(void**, char*). Returns index in pool which is correspond to
//registrated info.
unsigned Instrumentation::regDbgStr(const std::string& S, Module& M) {
  const std::string& ModuleName = M.getModuleIdentifier();
  const std::string& InitFuncName = "sapforInitDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  std::string DbgPoolName = "gSapforDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  //variables names in C can't contain '.'
  for(auto& J : DbgPoolName)
    if(J == '.')
      J = '_';
  auto F = M.getFunction(InitFuncName);
  if(F != nullptr) {
    auto& B = F->getEntryBlock();
    unsigned Val = mRegistrator.regDbgStr();
    auto Fun4Insert = getDeclaration(&M, IntrinsicId::init_di);
    auto Pool = M.getGlobalVariable(DbgPoolName);
    auto Idx = ConstantInt::get(Type::getInt32Ty(M.getContext()), Val);
    auto LoadArr = new LoadInst(Pool, "LoadArr", &B.back());
    auto Elem = GetElementPtrInst::Create(nullptr, LoadArr, {Idx}, "DI",
      B.getTerminator());
    auto Loc = prepareStrParam(S, B.back());
    CallInst::Create(Fun4Insert, {Elem, Loc, &(*F->arg_begin())}, "",
      &B.back());
    return Val;
  }
  llvm_unreachable((InitFuncName + " was not declared").c_str());
}

void Instrumentation::regTypes(Module& M) {
  const std::string& ModuleName = M.getModuleIdentifier();
  const std::string& RegFuncName = "RegType" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto F = M.getFunction(RegFuncName); // get function for types registration
  if(F != nullptr) {
    DataLayout DL(&M);
    //get all registrated types from registrator. fill vector<llvm::Constant* >
    //with local types indexes and sizes
    auto Types = mRegistrator.getAllRegistratedTypes();
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

GetElementPtrInst* Instrumentation::prepareStrParam(const std::string& S,
  Instruction& I) {
  auto Debug = llvm::ConstantDataArray::getString(I.getContext(), S);
  auto Arg = new llvm::GlobalVariable(*I.getModule(), Debug->getType(), true,
    llvm::GlobalValue::InternalLinkage, Debug);
  auto Int0 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(I.getContext()),
    0);
  return llvm::GetElementPtrInst::CreateInBounds(Arg, {Int0,Int0}, "DIString", &I);
}

//Returns element in debug information pool by its index
LoadInst* Instrumentation::getDbgPoolElem(unsigned Val, Instruction& I) {
  const std::string& ModuleName = I.getModule()->getModuleIdentifier();
  std::string DbgPoolName = "gSapforDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  //variables names in C can't contain '.'
  for(auto& J : DbgPoolName)
    if(J == '.')
      J = '_';
  auto Pool = I.getModule()->getGlobalVariable(DbgPoolName);
  auto Idx = ConstantInt::get(Type::getInt32Ty(I.getContext()), Val);
  auto LoadArr = new LoadInst(Pool, "LoadArr", &I);
  auto Elem = GetElementPtrInst::Create(nullptr, LoadArr, {Idx}, "Elem");
  Elem->insertAfter(LoadArr);
  auto DIFunc = new LoadInst(Elem, "DI");
  DIFunc->insertAfter(Elem);
  return DIFunc;
}

void Instrumentation::regGlobals(Module& M) {
  for(auto I = M.global_begin(); I != M.global_end(); I++) {
    unsigned ID = getTypeId(*I->getValueType());
    auto Metadata = getMetadata(&*I);
    if(Metadata == nullptr) {
      continue;
    }
    std::stringstream Debug;
    Debug << "type=var_name*file=" << M.getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" << 
      Metadata->getName().data() << "*vtype=" << ID << "**";
    unsigned Idx = regDbgStr(Debug.str(), M);
    mRegistrator.regVar(&(*I), Idx);
  }
}

void Instrumentation::regBaseTypes(Module &M) {
  mRegistrator.regType(Type::getVoidTy(M.getContext()));
  mRegistrator.regType(Type::getHalfTy(M.getContext()));
  mRegistrator.regType(Type::getFloatTy(M.getContext()));
  mRegistrator.regType(Type::getDoubleTy(M.getContext()));
  mRegistrator.regType(Type::getX86_FP80Ty(M.getContext()));
  mRegistrator.regType(Type::getFP128Ty(M.getContext()));
  mRegistrator.regType(Type::getPPC_FP128Ty(M.getContext()));
  mRegistrator.regType(Type::getLabelTy(M.getContext()));
  mRegistrator.regType(Type::getMetadataTy(M.getContext()));
  mRegistrator.regType(Type::getX86_MMXTy(M.getContext()));
  mRegistrator.regType(Type::getTokenTy(M.getContext()));
  //have problems with pointer and function types
  //need to put some special registration for them. 
  //don't registrate them as base types now
  for(unsigned i = 1; i <= maxIntBitWidth; ++i) {
    mRegistrator.regType(Type::getIntNTy(M.getContext(), i));
  }
}

unsigned Instrumentation::getTypeId(const Type& T) {
  switch(T.getTypeID()) {
    case(Type::TypeID::VoidTyID): return BaseTypeID::VoidTy;
    case(Type::TypeID::HalfTyID): return BaseTypeID::HalfTy;
    case(Type::TypeID::FloatTyID): return BaseTypeID::FloatTy;
    case(Type::TypeID::DoubleTyID): return BaseTypeID::DoubleTy;
    case(Type::TypeID::X86_FP80TyID): return BaseTypeID::X86_FP80Ty;
    case(Type::TypeID::FP128TyID): return BaseTypeID::FP128Ty;
    case(Type::TypeID::PPC_FP128TyID): return BaseTypeID::PPC_FP128Ty;
    case(Type::TypeID::LabelTyID): return BaseTypeID::LabelTy;
    case(Type::TypeID::MetadataTyID): return BaseTypeID::MetadataTy;
    case(Type::TypeID::X86_MMXTyID): return BaseTypeID::X86_MMXTy;
    case(Type::TypeID::TokenTyID): return BaseTypeID::TokenTy;
    //case(Type::TypeID::FunctionTyID): return BaseTypeID::FunctionTy;
    //case(Type::TypeID::PointerTyID): return BaseTypeID::PointerTy;
    case(Type::TypeID::IntegerTyID): 
      if(cast<IntegerType>(T).getBitWidth() <= maxIntBitWidth) {
        return (BaseTypeID::IntegerTy + cast<IntegerType>(T).getBitWidth() -1);
      }
    default: return mRegistrator.regType(&T) + BaseTypeID::IntegerTy +
      maxIntBitWidth;
  }
}

void Instrumentation::instrumentateMain(Module& M) {
  const std::string& ModuleName = M.getModuleIdentifier();
  auto MainFunc = M.getFunction("main"), RegFunc = M.getFunction("RegType" +
    ModuleName.substr(ModuleName.find_last_of("/\\")+1)), InitFunc =
    M.getFunction("sapforInitDI" + ModuleName.substr(ModuleName
    .find_last_of("/\\")+1));
  if(MainFunc == nullptr || RegFunc == nullptr || InitFunc == nullptr)
    return;
  auto& B = MainFunc->getEntryBlock();
  auto Int0 = llvm::ConstantInt::get(Type::getInt64Ty(M.getContext()), 0);
  CallInst::Create(RegFunc, {Int0}, "", &(*B.begin()));
  CallInst::Create(InitFunc, {Int0}, "", &(*B.begin()));
}
