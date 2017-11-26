#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include "Instrumentation.h"
#include "Intrinsics.h"

using namespace llvm;
using namespace tsar;

Instrumentation::Instrumentation(LoopInfo &LI, Registrator &R, Function &F)
  : mLoopInfo(LI), mRegistrator(R) {
  //insert extern declaration of DIVarPool if it wasn't declared in this
  //module yet
  if(!F.getParent()->getGlobalVariable("DIVarPool")) {
    auto Pool = new GlobalVariable((*F.getParent()), PointerType::getUnqual(Type
      ::getInt8PtrTy(F.getContext())), false, GlobalValue::LinkageTypes
      ::ExternalLinkage, nullptr, "DIVarPool", nullptr);
    Pool->setAlignment(4);
  }
  visitFunction(F);
}

void Instrumentation::visitLoadInst(llvm::LoadInst &I) {
  Function* Fun;
  //FIXME: this doesn't correctly separate arrays from variables
  auto TypeID =  I.getPointerOperand()->getType()->getPointerElementType()
    ->getTypeID();
  //depending on operand type insert a sapforReadVar or sapforReadArr
  //consider all pointers as arrays
  if(TypeID != Type::TypeID::PointerTyID && TypeID != Type::TypeID::ArrayTyID){
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_var);
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_arr);
  }
  CallInst::Create(Fun, {}, "", &I);
}

void Instrumentation::visitStoreInst(llvm::StoreInst &I) {
  Function* Fun;
  //FIXME: this doesn't correctly separate arrays from variables
  auto TypeID =  I.getPointerOperand()->getType()->getPointerElementType()
    ->getTypeID();
  //depending on operand type insert a sapforWriteVarEnd or sapforWriteArrEnd
  //consider all pointers as arrays
  if(TypeID != Type::TypeID::PointerTyID && TypeID != Type::TypeID::ArrayTyID) {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_var_end);
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_arr_end);
  }
  auto Call = CallInst::Create(Fun, {}, "");
  Call->insertAfter(&I);
}

//insert call of sapforRegVar(void*, void*) or 
//sapforRegArr(void*, size_t, void*) after specified alloca instruction.
void Instrumentation::visitAllocaInst(llvm::AllocaInst &I) {
  auto Pool = I.getModule()->getGlobalVariable("DIVarPool");
  auto RegInt = ConstantInt::get(Type::getInt32Ty(I.getContext()),
    mRegistrator.regVar());
  //getting element in DIVarPool with mRegistrator.regVar index
  auto LoadArr = new LoadInst(Pool, "LoadArr", &I);
  auto Elem = GetElementPtrInst::Create(nullptr, LoadArr, {RegInt}, "Elem", &I);
  auto DIVar = new LoadInst(Elem, "DIVar", &I);
  //i hope it's always possible to cast alloca instruction, which is pointer 
  //to some type, to i8* 
  auto Addr = new BitCastInst(&I, Type::getInt8PtrTy(I.getContext()), 
    "Addr");
  cast<Instruction>(Addr)->insertAfter(&I);
  auto TypeId = I.getAllocatedType()->getTypeID();
  //Alloca instruction has isArrayAllocation method, but it looks like it
  //doesn't work like i wanted it. So checking for array in this way
  if(TypeId == Type::TypeID::ArrayTyID || TypeId == Type::TypeID::PointerTyID){
    auto ArrSize = ConstantInt::get(Type::getInt64Ty(I.getContext()),
      I.getAllocatedType()->getArrayNumElements());
    auto Int0 = ConstantInt::get(Type::getInt32Ty(I.getContext()), 0);
    auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_arr);
    auto Call = CallInst::Create(Fun, {DIVar, ArrSize, Addr}, "");
    Call->insertAfter(Addr);
  } else {	  
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
  CallInst::Create(Fun, {}, "", &I);
}

void Instrumentation::loopBeginInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header){
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
          //looks like label "loop_begin" would be
	  //renamed automatically to make it unique
          auto Block4Insert = BasicBlock::Create(Header.getContext(),
	   "loop_begin", Header.getParent());
          IRBuilder<> Builder(Block4Insert, Block4Insert->end());
          Builder.CreateBr(ExitInstr->getSuccessor(I));
          ExitInstr->setSuccessor(I, Block4Insert);
          Builder.SetInsertPoint(&(*const_cast<BasicBlock *>
	    (Block4Insert)->begin()));
          auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_begin);
          auto Call = Builder.CreateCall(Fun);
	}
      }
    }
  }
}

void Instrumentation::loopEndInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header) {
  //Creating a node between inside/outside blocks of the loop. Then insert
  //call of tsarSLEnd() in this node.
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
          Builder.SetInsertPoint(&(*const_cast<BasicBlock *>
	    (Block4Insert)->begin()));
          auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_end);
          auto Call = Builder.CreateCall(Fun);
	}
      }
    }
  }
}

void Instrumentation::loopIterInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header) {
  auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_iter);
  CallInst::Create(Fun, {}, "", Header.getTerminator());
}

void Instrumentation::visitBasicBlock(llvm::BasicBlock &B) {
  if(mLoopInfo.isLoopHeader(&B)) {
    auto Loop = mLoopInfo.getLoopFor(&B);
    loopBeginInstr(Loop, B);
    loopEndInstr(Loop, B);
    loopIterInstr(Loop, B);
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
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto Begin = inst_begin(F);
  CallInst::Create(Fun, {}, "", &(*Begin));
  //visit all Blocks
  for(auto &I : F.getBasicBlockList()) {
    visitBasicBlock(I);
  }
}
